/* DFORM - Schwung plugin API v2 adapter. MIT.
 *
 * EVERY entry point here runs on the SPI audio callback (plugin_api_v1.h), so:
 * no allocation (instances come from a static pool), no file I/O, no logging,
 * bounded work.
 *
 * There is no note input: while Move's transport runs the voice fires on
 * every 1/16 by itself (host get_beat_position, MIDI clock fallback), and it
 * is silent while the transport is stopped. MIDI notes are ignored.
 *
 * The pads (ui_core.mjs, taken while the screen is up) talk to it through
 * three keys: "write" (the REC pad arms motion recording), "gate_toggle"
 * (the 16 trigger steps) and "randomize" (a Phrase Gen phrase into PITCH). */
#include "host/plugin_api_v1.h"
#include "bk_engine.h"
#include "bk_clock.h"
#include "bk_motion.h"
#include "bk_phrase.h"
#include "bk_metadata.h"
#include <ctype.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "instance pool needs lock-free atomics");

/* Four chain slots can each hold one; spares cover a swap in flight. */
#define BK_INSTANCES 6

/* The shim parks a slot whose output stays below 5 LSB for ~1 s and renders
 * it only every ~0.5 s after that (schwung_shim.c, DSP_IDLE_THRESHOLD), so the
 * first hits after Play would be late. A block that would be silent carries a
 * 6 LSB DC offset (-75 dBFS, inaudible, removed by any DC blocker) -- except at
 * VOLUME 0, which is exact digital silence. Same scheme as FMDRUM. */
#define BK_KEEPALIVE_LSB 6
#define BK_SILENCE_LSB 4

typedef struct {
    atomic_int used;
    bk_engine engine;
    bk_clock clock;
    bk_motion motion;
    double last_pos16;       /* monotonic playhead (host beat jitter) */
    uint16_t gates;          /* trigger pads: bit n = step n+1 of 16 fires */
    bk_phrase phrase;        /* the RANDOM pad */
    uint32_t frames;         /* since create: entropy for the RANDOM pad */
    /* MIDI clock fallback, used only when the host has no get_beat_position. */
    int clk_running, clk_awaiting;
    long clk_ticks;
    float mono[MOVE_FRAMES_PER_BLOCK];
} bk_instance;

static bk_instance g_pool[BK_INSTANCES];
static const host_api_v1_t *g_host;
static atomic_uint g_seed = 0x5B1CA7E1u;

/* The knob grid is Schwung's own page engine, driven from ui_chain.js so the
 * module sees the knobs' touch sensors. Served as `grid_hierarchy`, while
 * `ui_hierarchy` is served EMPTY on purpose: a hierarchy there makes the
 * shadow UI draw its stock grid, which runs no module code and so never sees
 * a knob touch (FMDRUM's arrangement). */
static const char bk_hierarchy[] =
    "{\"modes\":null,\"levels\":{"
      "\"root\":{\"label\":\"DFORM\","
        "\"knobs\":[\"volume\",\"vcf_decay\",\"p_env\",\"fm_amount\",\"p_decay\",\"cutoff\",\"reso_drive\",\"pitch\"],"
        "\"params\":[\"volume\",\"vcf_decay\",\"p_env\",\"fm_amount\",\"p_decay\",\"cutoff\",\"reso_drive\",\"pitch\","
          "{\"level\":\"tone\",\"label\":\"Tone\"},{\"level\":\"scale\",\"label\":\"Scale\"},"
          "{\"level\":\"motion\",\"label\":\"Motion\"}]},"
      "\"tone\":{\"label\":\"Tone\","
        "\"knobs\":[\"vca_decay\",\"vcf_env\",\"vco2_tune\",\"vco2_level\",\"noise\"],"
        "\"params\":[\"vca_decay\",\"vcf_env\",\"vco2_tune\",\"vco2_level\",\"noise\"]},"
      "\"scale\":{\"label\":\"Scale\","
        "\"knobs\":[\"root\",\"scale\"],"
        "\"params\":[\"root\",\"scale\"]},"
      "\"motion\":{\"label\":\"Motion\","
        "\"knobs\":[\"length\"],"
        "\"params\":[\"length\"]}"
    "}}";

/* ---- lifecycle ---------------------------------------------------------- */
static void *create_instance(const char *module_dir, const char *defaults) {
    (void)module_dir; (void)defaults;
    for (int i = 0; i < BK_INSTANCES; ++i) {
        int expected = 0;
        if (atomic_compare_exchange_strong(&g_pool[i].used, &expected, 1)) {
            bk_instance *inst = &g_pool[i];
            bk_engine_init(&inst->engine, atomic_fetch_add(&g_seed, 0x9E3779B9u));
            bk_clock_init(&inst->clock);
            bk_motion_init(&inst->motion);
            inst->last_pos16 = -1.0;
            inst->gates = 0xFFFFu;
            bk_phrase_init(&inst->phrase, atomic_fetch_add(&g_seed, 0x7F4A7C15u));
            inst->frames = 0;
            inst->clk_running = inst->clk_awaiting = 0;
            inst->clk_ticks = 0;
            return inst;
        }
    }
    return NULL;
}

static void destroy_instance(void *ptr) {
    if (ptr) atomic_store(&((bk_instance *)ptr)->used, 0);
}

/* ---- MIDI: only the clock (fallback transport) -------------------------- */
static void on_midi(void *ptr, const uint8_t *msg, int len, int source) {
    (void)source;
    if (!ptr || !msg || len < 1) return;
    bk_instance *inst = ptr;
    switch (msg[0]) {
    case 0xFA: inst->clk_running = 1; inst->clk_awaiting = 1; inst->clk_ticks = 0; break;
    case 0xFB: inst->clk_running = 1; break;
    case 0xFC: inst->clk_running = 0; break;
    case 0xF8:
        if (!inst->clk_running) { inst->clk_running = 1; inst->clk_awaiting = 1; }
        if (inst->clk_awaiting) { inst->clk_awaiting = 0; inst->clk_ticks = 0; }
        else inst->clk_ticks++;
        break;
    default: break;          /* notes, CCs: this voice plays itself */
    }
}

/* ---- parameters --------------------------------------------------------- */
static int param_index(const char *key) {
    for (int i = 0; i < BP_COUNT; ++i) if (!strcmp(key, bk_params[i].key)) return i;
    return -1;
}

static const char *skip_ws(const char *p) {
    while (*p && isspace((unsigned char)*p)) ++p;
    return p;
}

/* A whole-string number, rejecting NaN/Inf/trailing junk. */
static int parse_float(const char *s, float *out) {
    char *end;
    float v = strtof(s, &end);
    if (end == s || *skip_ws(end) || !isfinite(v)) return 0;
    *out = v;
    return 1;
}

/* ---- state blob -----------------------------------------------------------
 * {"v":3,"p":[16 numbers],"m":"<lane>:<data>;...","g":<gates>}
 * Self-contained, so autosave and My Presets reproduce the sound exactly.
 * "m" carries only lanes that hold automation; <data> is BK_POINTS 12-bit
 * values normalized over the parameter's range, two base64url characters
 * each (4095 = point not written). Applied only if every field is valid.
 * v3: PITCH is a scale-note index, "g" the trigger pads, no REC param (the
 * REC pad is never saved). v2 (17 values, PITCH a semitone position 0..48)
 * and v1 (PITCH a 0..1 knob) are converted -- the knob and the pitch lane --
 * to the index of the note they played. */
#define BK_STATE_MAX 12288

static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

static const char *find_key(const char *s, const char *key) {
    size_t n = strlen(key);
    for (const char *p = s; (p = strchr(p, '"')) != NULL; ++p) {
        if (!strncmp(p + 1, key, n) && p[n + 1] == '"') {
            const char *q = skip_ws(p + n + 2);
            if (*q == ':') return skip_ws(q + 1);
        }
    }
    return NULL;
}

/* JSON number grammar only: strtof alone would also take "nan", hex, "+1". */
static const char *read_number(const char *p, float *out) {
    const char *start = p;
    if (*p == '-') ++p;
    if (*p == '0') ++p;
    else if (*p >= '1' && *p <= '9') { while (isdigit((unsigned char)*p)) ++p; }
    else return NULL;
    if (*p == '.') {
        ++p;
        if (!isdigit((unsigned char)*p)) return NULL;
        while (isdigit((unsigned char)*p)) ++p;
    }
    if (*p == 'e' || *p == 'E') {
        ++p;
        if (*p == '+' || *p == '-') ++p;
        if (!isdigit((unsigned char)*p)) return NULL;
        while (isdigit((unsigned char)*p)) ++p;
    }
    char tmp[32];
    size_t n = (size_t)(p - start);
    if (n == 0 || n >= sizeof(tmp)) return NULL;
    memcpy(tmp, start, n);
    tmp[n] = 0;
    float v = strtof(tmp, NULL);
    if (!isfinite(v)) return NULL;
    *out = v;
    return p;
}

/* Parse into scratch first; the instance changes only if all of it is valid.
 * Static scratch is safe: every call comes from the one SPI callback thread. */
static uint16_t g_scratch[BK_MOTION_LANES][BK_POINTS];

/* A v1/v2 PITCH (semitones above C1, 0..48) as a v3 scale index. */
static float legacy_pitch_index(float semis, int root, int scale) {
    return (float)bk_pitch_index_of_midi((float)bk_quantize_pitch(semis, root, scale), root, scale);
}

static int restore_state(bk_instance *inst, const char *text) {
    bk_engine *e = &inst->engine;
    bk_motion *mo = &inst->motion;
    if (!text || strlen(text) > BK_STATE_MAX) return 0;
    const char *s = skip_ws(text);
    if (*s != '{') return 0;
    float v, raw[BP_COUNT + 1], prm[BP_COUNT];
    const char *p = find_key(s, "v");
    if (!p || !read_number(p, &v) || (v != 1.0f && v != 2.0f && v != 3.0f)) return 0;
    const int count = v >= 3.0f ? BP_COUNT : BP_COUNT + 1;      /* v1/v2 also carry REC */
    p = find_key(s, "p");
    if (!p || *p != '[') return 0;
    p = skip_ws(p + 1);
    for (int i = 0; i < count; ++i) {
        p = read_number(p, &raw[i]);
        if (!p) return 0;
        p = skip_ws(p);
        if (i < count - 1) {
            if (*p != ',') return 0;
            p = skip_ws(p + 1);
        }
    }
    if (*p != ']') return 0;
    for (int i = 0; i < BP_COUNT; ++i) {
        prm[i] = raw[i];                      /* REC was the last value: dropped */
        const bk_param_info *pi = &bk_params[i];
        float hi = pi->max;
        if (i == BP_PITCH && v == 1.0f) hi = 1.0f;
        if (prm[i] < pi->min || prm[i] > hi) return 0;
        if (pi->is_int && !(i == BP_PITCH && v == 1.0f) && prm[i] != floorf(prm[i])) return 0;
    }
    if (v < 3.0f && (raw[BP_COUNT] < 0.0f || raw[BP_COUNT] > 1.0f)) return 0;
    const int root = (int)prm[BP_ROOT], scale = (int)prm[BP_SCALE];
    if (v == 1.0f) prm[BP_PITCH] = floorf(prm[BP_PITCH] * 48.0f + 0.5f);
    if (v < 3.0f) prm[BP_PITCH] = legacy_pitch_index(prm[BP_PITCH], root, scale);
    else if (prm[BP_PITCH] > (float)(bk_pitch_count(root, scale) - 1)) return 0;

    for (int k = 0; k < BK_MOTION_LANES; ++k)
        for (int i = 0; i < BK_POINTS; ++i) g_scratch[k][i] = BK_EMPTY;
    p = find_key(s, "m");
    if (!p || *p != '"') return 0;
    ++p;
    unsigned seen = 0;
    while (*p != '"') {
        const char *colon = strchr(p, ':');
        if (!colon || colon - p >= 24) return 0;
        char key[24];
        memcpy(key, p, (size_t)(colon - p));
        key[colon - p] = 0;
        int k = bk_motion_lane(param_index(key));
        if (k < 0 || (seen & (1u << k))) return 0;
        seen |= 1u << k;
        p = colon + 1;
        for (int i = 0; i < BK_POINTS; ++i) {
            int hi = b64_val(p[0]), lo = hi < 0 ? -1 : b64_val(p[1]);
            if (hi < 0 || lo < 0) return 0;
            int q = hi * 64 + lo;
            g_scratch[k][i] = q == 4095 ? BK_EMPTY : (q > BK_QMAX ? BK_QMAX : (uint16_t)q);
            p += 2;
        }
        if (*p == ';') ++p;
        else if (*p != '"') return 0;
    }
    float g = 65535.0f;
    if (v >= 3.0f) {
        p = find_key(s, "g");
        if (!p || !(p = read_number(p, &g)) || g != floorf(g) || g < 0.0f || g > 65535.0f) return 0;
        if (*skip_ws(p) != '}') return 0;         /* "g" is last: a cut-off document is refused */
    }
    /* An old pitch lane held semitone positions: the notes it played, as indices. */
    if (v < 3.0f) {
        int pk = bk_motion_lane(BP_PITCH);
        for (int i = 0; i < BK_POINTS; ++i) {
            if (g_scratch[pk][i] == BK_EMPTY) continue;
            float semis = bk_motion_dq(g_scratch[pk][i]) * 48.0f;
            float idx = legacy_pitch_index(floorf(semis + 0.5f), root, scale);
            g_scratch[pk][i] = bk_motion_q(bk_motion_norm(BP_PITCH, idx));
        }
    }

    /* Root and scale first: PITCH is clamped to the scale in force. */
    bk_engine_set_param(e, BP_ROOT, prm[BP_ROOT]);
    bk_engine_set_param(e, BP_SCALE, prm[BP_SCALE]);
    for (int i = 0; i < BP_COUNT; ++i) bk_engine_set_param(e, i, prm[i]);
    bk_motion_clear(mo, e, -1);
    memcpy(mo->pt, g_scratch, sizeof(mo->pt));
    inst->gates = (uint16_t)g;
    return 1;
}

static int write_state(const bk_engine *e, const bk_motion *mo, unsigned gates, char *buf, int size) {
    int n = snprintf(buf, size, "{\"v\":3,\"p\":[");
    for (int i = 0; i < BP_COUNT && n > 0 && n < size; ++i)
        n += snprintf(buf + n, size - n, "%s%.4f", i ? "," : "", e->param[i]);
    if (n > 0 && n < size) n += snprintf(buf + n, size - n, "],\"m\":\"");
    unsigned mask = bk_motion_mask(mo);
    int first = 1;
    for (int k = 0; k < BK_MOTION_LANES && n > 0 && n < size; ++k) {
        if (!(mask & (1u << k))) continue;
        n += snprintf(buf + n, size - n, "%s%s:", first ? "" : ";", bk_params[bk_motion_param[k]].key);
        first = 0;
        if (n < 0 || n + 2 * BK_POINTS >= size) { buf[0] = 0; return -1; }
        for (int i = 0; i < BK_POINTS; ++i) {
            int q = mo->pt[k][i] == BK_EMPTY ? 4095 : mo->pt[k][i];
            buf[n++] = b64[q >> 6];
            buf[n++] = b64[q & 63];
        }
        buf[n] = 0;
    }
    if (n > 0 && n < size) n += snprintf(buf + n, size - n, "\",\"g\":%u}", gates);
    if (n < 0 || n >= size) { buf[0] = 0; return -1; }
    return n;
}

/* "touch" = "<key>,<0|1>" from ui_chain.js: the knob's capacitive sensor.
 * "motion_clear" = "<key>" or "all". */
static void set_touch(bk_instance *inst, const char *val) {
    char key[32];
    const char *comma = strchr(val, ',');
    if (!comma || (size_t)(comma - val) >= sizeof(key)) return;
    memcpy(key, val, (size_t)(comma - val));
    key[comma - val] = 0;
    int lane = bk_motion_lane(param_index(key));
    if (lane < 0 || (strcmp(comma + 1, "0") && strcmp(comma + 1, "1"))) return;
    bk_motion_touch(&inst->motion, &inst->engine, lane, comma[1] == '1');
}

/* A new root or scale re-snaps PITCH -- the knob, what is playing, and every
 * recorded pitch point -- to the nearest note of the new scale, so the voice
 * does not jump to whatever note now sits at the old index. */
static void set_key(bk_instance *inst, int idx, float f) {
    bk_engine *e = &inst->engine;
    int r0 = (int)e->param[BP_ROOT], s0 = (int)e->param[BP_SCALE];
    float old_pitch = e->param[BP_PITCH];
    bk_engine_set_param(e, idx, f);
    int r1 = (int)e->param[BP_ROOT], s1 = (int)e->param[BP_SCALE];
    if (r0 == r1 && s0 == s1) return;
#define RESNAP(i) (float)bk_pitch_index_of_midi((float)bk_pitch_midi((int)((i) + 0.5f), r0, s0), r1, s1)
    bk_engine_set_param(e, BP_PITCH, RESNAP(old_pitch));
    if (!isnan(e->ovr[BP_PITCH])) e->ovr[BP_PITCH] = RESNAP(e->ovr[BP_PITCH]);
    int pk = bk_motion_lane(BP_PITCH);
    for (int i = 0; i < BK_POINTS; ++i) {
        uint16_t q = inst->motion.pt[pk][i];
        if (q == BK_EMPTY) continue;
        float x = bk_motion_denorm(BP_PITCH, bk_motion_dq(q));
        inst->motion.pt[pk][i] = bk_motion_q(bk_motion_norm(BP_PITCH, RESNAP(x)));
    }
#undef RESNAP
}

/* The RANDOM pad: a new Phrase Gen phrase, written as PITCH automation --
 * each note set as a knob value of the scale in use (nearest note,
 * transposed to ROOT), never quantized afterwards. Phrase Gen's sources span
 * ~6 octaves, far too wide for a percussion voice, so each note is folded by
 * octaves into a two-octave window around the knob's own note (a fifth below
 * to 17 semitones above): the melody keeps its note names, knob 8 sets its
 * register. The sources sit high, so the fold WRAPS over two octaves first:
 * only folding them down piled every note into the window's top octave. At the
 * ends of the range the window is cut, never slid, so a C1 knob stays at C1
 * to F2. Every point of a step holds its note (a hit reads the step's
 * first); a 1/2-bar loop gets an 8-step phrase, anything longer a 16-step
 * one, repeated to the loop's end. Same as LXRDRUM's RANDOM pad. */
static void randomize(bk_instance *inst) {
    bk_engine *e = &inst->engine;
    int len = bk_motion_len_steps(e), plen = len == 8 ? 8 : 16, notes[16];
    if (!bk_phrase_generate(&inst->phrase, plen, inst->frames, notes)) return;
    int r = (int)e->param[BP_ROOT], sc = (int)e->param[BP_SCALE];
    int pk = bk_motion_lane(BP_PITCH);
    int knob = bk_pitch_midi((int)e->param[BP_PITCH], r, sc);
    int lo = knob - 7, hi = knob + 17;
    if (lo < BK_PITCH_LO) lo = BK_PITCH_LO;
    if (hi > BK_PITCH_HI) hi = BK_PITCH_HI;
    for (int st = 0; st < len; ++st) {
        int n = notes[st % plen];
        if (n < 0) n = knob - r;
        n += r;                                   /* the phrases are in C */
        n = lo + (((n - lo) % 24) + 24) % 24;
        while (n > hi) n -= 12;
        while (n < lo) n += 12;
        while (n > BK_PITCH_HI) n -= 12;
        while (n < BK_PITCH_LO) n += 12;
        uint16_t q = bk_motion_q(bk_motion_norm(BP_PITCH, (float)bk_pitch_index_of_midi((float)n, r, sc)));
        for (int j = 0; j < BK_SUB; ++j) inst->motion.pt[pk][st * BK_SUB + j] = q;
    }
}

static void set_param(void *ptr, const char *key, const char *val) {
    if (!ptr || !key || !val) return;
    bk_instance *inst = ptr;
    bk_engine *e = &inst->engine;
    float f;
    if (!strcmp(key, "state")) { (void)restore_state(inst, val); return; }
    /* The pads. */
    if (!strcmp(key, "write")) {
        if (!strcmp(val, "0") || !strcmp(val, "1")) inst->motion.write = val[0] == '1';
        return;
    }
    if (!strcmp(key, "gate_toggle")) {
        if (parse_float(val, &f) && f == floorf(f) && f >= 0.0f && f < 16.0f)
            inst->gates ^= (uint16_t)(1u << (int)f);
        return;
    }
    if (!strcmp(key, "randomize")) { randomize(inst); return; }
    if (!strcmp(key, "touch")) { set_touch(inst, val); return; }
    if (!strcmp(key, "motion_clear")) {
        if (!strcmp(val, "all")) bk_motion_clear(&inst->motion, e, -1);
        else {
            int k = bk_motion_lane(param_index(val));
            if (k >= 0) bk_motion_clear(&inst->motion, e, k);
        }
        return;
    }
    int idx = param_index(key);
    if (idx < 0 || !parse_float(val, &f)) return;
    if (idx == BP_ROOT || idx == BP_SCALE) { set_key(inst, idx, f); return; }
    bk_engine_set_param(e, idx, f);
}

static int copy_text(char *buf, int size, const char *text) {
    size_t n = strlen(text);
    if (n >= (size_t)size) { buf[0] = 0; return -1; }
    memcpy(buf, text, n + 1);
    return (int)n;
}

static int format_value(const bk_param_info *pi, float x, char *buf, int size) {
    return pi->is_int ? snprintf(buf, size, "%d", (int)x) : snprintf(buf, size, "%.4f", x);
}

static int get_param(void *ptr, const char *key, char *buf, int size) {
    if (!ptr || !key || !buf || size <= 0) return -1;
    buf[0] = 0;
    const bk_instance *inst = ptr;
    const bk_engine *e = &inst->engine;
    int idx = param_index(key);
    int n = -1;
    if (idx >= 0) n = format_value(&bk_params[idx], e->param[idx], buf, size);
    /* eff_<key>: what the sound is using right now (automation playback). */
    else if (!strncmp(key, "eff_", 4) && (idx = param_index(key + 4)) >= 0)
        n = format_value(&bk_params[idx], bk_engine_value(e, idx), buf, size);
    /* One read for the UI:
     * "<lanes with data>,<step or -1>,<recording lanes>,<loop steps>,<REC pad>" */
    else if (!strcmp(key, "motion_status"))
        n = snprintf(buf, size, "%u,%d,%u,%d,%d", bk_motion_mask(&inst->motion),
                     inst->motion.step, bk_motion_recording(&inst->motion, e),
                     bk_motion_len_steps(e), inst->motion.write);
    /* The trigger pads, step 1 first: "1111111111111111". */
    else if (!strcmp(key, "gate_status")) {
        if (size <= 16) return -1;
        for (int i = 0; i < 16; ++i) buf[i] = (inst->gates >> i) & 1u ? '1' : '0';
        buf[16] = 0;
        return 16;
    }
    else if (!strcmp(key, "write")) n = snprintf(buf, size, "%d", inst->motion.write);
    /* The PITCH knob's note names for the scale in use, comma separated: the
     * UI shows "Eb2" rather than an index, and asks again after a key change. */
    else if (!strcmp(key, "pitch_names")) {
        int r = (int)e->param[BP_ROOT], sc = (int)e->param[BP_SCALE];
        int cnt = bk_pitch_count(r, sc);
        n = 0;
        for (int i = 0; i < cnt && n >= 0 && n < size; ++i) {
            char nm[8];
            bk_note_name(bk_pitch_midi(i, r, sc), nm, sizeof(nm));
            n += snprintf(buf + n, size - n, "%s%s", i ? "," : "", nm);
        }
    }
    /* The note the PITCH knob is set to. */
    else if (!strcmp(key, "pitch_note")) {
        bk_note_name(bk_pitch_midi((int)e->param[BP_PITCH], (int)e->param[BP_ROOT],
                                   (int)e->param[BP_SCALE]), buf, size);
        n = (int)strlen(buf);
    }
    else if (!strcmp(key, "state")) return write_state(e, &inst->motion, inst->gates, buf, size);
    /* PITCH's range is the number of notes the scale has in C1..C5, so the
     * knob has no dead travel whatever the scale (module.json carries 28,
     * C Phrygian's top index). */
    else if (!strcmp(key, "chain_params"))
        n = snprintf(buf, size, "%s%d%s", bk_chain_params_head,
                     bk_pitch_count((int)e->param[BP_ROOT], (int)e->param[BP_SCALE]) - 1,
                     bk_chain_params_tail);
    else if (!strcmp(key, "ui_hierarchy")) return 0;
    else if (!strcmp(key, "grid_hierarchy")) return copy_text(buf, size, bk_hierarchy);
    if (n < 0 || n >= size) { buf[0] = 0; return -1; }
    return n;
}

static int get_error(void *ptr, char *buf, int size) {
    (void)ptr;
    if (buf && size > 0) buf[0] = 0;
    return 0;
}

/* ---- audio -------------------------------------------------------------- */
static double transport_beat(bk_instance *inst) {
    if (g_host && g_host->get_beat_position) return g_host->get_beat_position();
    return inst->clk_running ? (double)inst->clk_ticks / 24.0 : -1.0;
}

static float transport_bpm(void) {
    float bpm = (g_host && g_host->get_bpm) ? g_host->get_bpm() : 120.0f;
    return (bpm >= 20.0f && bpm <= 999.0f) ? bpm : 120.0f;
}

/* Automation runs at control rate: every 16 samples (0.36 ms) and exactly on
 * each hit. The playhead never steps backwards by less than a 1/16 (that is
 * host jitter, not a new Play). */
#define CTRL_FRAMES 16

static void motion_at(bk_instance *inst, double pos16) {
    if (pos16 >= 0.0 && inst->last_pos16 >= 0.0 &&
        pos16 < inst->last_pos16 && pos16 > inst->last_pos16 - 1.0)
        pos16 = inst->last_pos16;
    inst->last_pos16 = pos16;
    bk_motion_update(&inst->motion, &inst->engine, pos16);
}

static void render_span(bk_instance *inst, double beat, double spb, int from, int to) {
    while (from < to) {
        int n = to - from;
        if (n > CTRL_FRAMES) n = CTRL_FRAMES;
        if (inst->clock.running) motion_at(inst, (beat + (double)from / spb) * 4.0);
        bk_engine_render(&inst->engine, inst->mono + from, n);
        from += n;
    }
}

static void render_block(void *ptr, int16_t *out, int frames) {
    if (!out || frames <= 0) return;
    if (!ptr || frames > MOVE_FRAMES_PER_BLOCK) {
        memset(out, 0, (size_t)frames * 2 * sizeof(int16_t));
        return;
    }
    bk_instance *inst = ptr;
    bk_engine *e = &inst->engine;
    double beat = transport_beat(inst);
    float bpm = transport_bpm();
    double spb = (double)BK_SR * 60.0 / (double)bpm;

    inst->frames += (uint32_t)frames;
    bk_motion_age(&inst->motion, frames);
    int was_running = inst->clock.running;
    int offsets[BK_CLOCK_MAX_EVENTS];
    int n = bk_clock_advance(&inst->clock, beat, bpm, frames, offsets);
    if (!inst->clock.running) {
        if (was_running) bk_engine_release(e);
        inst->last_pos16 = -1.0;
        motion_at(inst, -1.0);
    } else if (!was_running) {
        inst->last_pos16 = -1.0;             /* a new Play */
    }

    /* Split the block at each 1/16 so a hit lands on its sample. Automation
     * is stepped to the hit's exact 1/16 first, so PITCH (and both decays)
     * are the values at the start of that step. */
    int pos = 0;
    for (int k = 0; k < n; ++k) {
        render_span(inst, beat, spb, pos, offsets[k]);
        pos = offsets[k];
        int64_t abs16 = inst->clock.last_abs - (int64_t)(n - 1 - k);
        if (inst->last_pos16 > (double)abs16) inst->last_pos16 = -1.0;   /* restart */
        motion_at(inst, (double)abs16);
        /* The trigger pads: step n of the loop fires if pad (n mod 16) is on.
         * Motion still steps on a silent step, so its automation stays in time. */
        int len = bk_motion_len_steps(e);
        int step = (int)(((abs16 % len) + len) % len) % 16;
        if ((inst->gates >> step) & 1u) bk_engine_trigger(e);
    }
    render_span(inst, beat, spb, pos, frames);

    int peak = 0;
    for (int i = 0; i < frames; ++i) {
        float x = inst->mono[i];
        x = x > 1.0f ? 1.0f : (x < -1.0f ? -1.0f : x);
        int16_t s = (int16_t)lrintf(x * 32767.0f);
        out[i * 2] = s;
        out[i * 2 + 1] = s;
        int a = abs(s);
        if (a > peak) peak = a;
    }
    if (peak <= BK_SILENCE_LSB && e->param[BP_VOLUME] > 0.0f)
        for (int i = 0; i < frames * 2; ++i) out[i] = BK_KEEPALIVE_LSB;
}

static plugin_api_v2_t g_api = {
    .api_version = 2,
    .create_instance = create_instance,
    .destroy_instance = destroy_instance,
    .on_midi = on_midi,
    .set_param = set_param,
    .get_param = get_param,
    .get_error = get_error,
    .render_block = render_block,
};

/* The only exported symbol (-fvisibility=hidden): the Phrase Gen copy's
 * globals can never collide with another module's. */
__attribute__((visibility("default")))
plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *host) {
    if (host && host->sample_rate && host->sample_rate != MOVE_SAMPLE_RATE) return NULL;
    g_host = host;
    bk_phrase_library_init();
    return &g_api;
}
