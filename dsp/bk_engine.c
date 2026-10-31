/* DFORM - auto-triggered analog-style percussion engine. MIT. See bk_engine.h. */
#include "bk_engine.h"

#include <math.h>
#include <string.h>

#define BK_PI 3.14159265358979f
#define BK_FS_OS (BK_SR * BK_OS)

/* Keys, ranges and defaults. scripts/metadata.py checks this table against
 * module.json's chain_params, so the knob the host shows and the value the
 * DSP uses cannot drift apart. */
const bk_param_info bk_params[BP_COUNT] = {
    {"volume",      0.0f,   1.0f,  0.75f, 0},
    {"vcf_decay",   0.0f,   1.0f,  0.60f, 0},
    {"vca_decay",   0.0f,   1.0f,  0.55f, 0},
    {"fm_amount",   0.0f,   1.0f,  0.25f, 0},
    {"vcf_env",     0.0f,   1.0f,  0.60f, 0},
    {"cutoff",      0.0f,   1.0f,  0.35f, 0},
    {"reso_drive",  0.0f,   1.0f,  0.45f, 0},
    {"pitch",       0.0f,  48.0f,  7.0f,  1},   /* scale note index; 7 = C2 in C Phrygian */
    {"root",        0.0f,  11.0f,  0.0f,  1},
    {"scale",       0.0f,  11.0f,  0.0f,  1},
    {"p_env",       0.0f,   1.0f,  0.35f, 0},
    {"p_decay",     0.0f,   1.0f,  0.45f, 0},
    {"vco2_tune", -24.0f,  24.0f,  7.3f,  0},
    {"vco2_level",  0.0f,   1.0f,  0.60f, 0},
    {"noise",       0.0f,   1.0f,  0.05f, 0},
    {"length",      0.0f,   3.0f,  1.0f,  1},
};

/* ---- scales --------------------------------------------------------------- */
const char *const bk_scale_names[BK_SCALE_COUNT] = {
    "Phrygian", "Minor", "Dorian", "Major", "Mixolyd", "Lydian",
    "Locrian", "HarmMin", "PhrygDom", "MinPent", "In", "Chromatic",
};

/* Bit n = semitone n above the root is in the scale. */
static const uint16_t scale_mask[BK_SCALE_COUNT] = {
    (1u<<0)|(1u<<1)|(1u<<3)|(1u<<5)|(1u<<7)|(1u<<8)|(1u<<10),   /* Phrygian */
    (1u<<0)|(1u<<2)|(1u<<3)|(1u<<5)|(1u<<7)|(1u<<8)|(1u<<10),   /* Minor (Aeolian) */
    (1u<<0)|(1u<<2)|(1u<<3)|(1u<<5)|(1u<<7)|(1u<<9)|(1u<<10),   /* Dorian */
    (1u<<0)|(1u<<2)|(1u<<4)|(1u<<5)|(1u<<7)|(1u<<9)|(1u<<11),   /* Major */
    (1u<<0)|(1u<<2)|(1u<<4)|(1u<<5)|(1u<<7)|(1u<<9)|(1u<<10),   /* Mixolydian */
    (1u<<0)|(1u<<2)|(1u<<4)|(1u<<6)|(1u<<7)|(1u<<9)|(1u<<11),   /* Lydian */
    (1u<<0)|(1u<<1)|(1u<<3)|(1u<<5)|(1u<<6)|(1u<<8)|(1u<<10),   /* Locrian */
    (1u<<0)|(1u<<2)|(1u<<3)|(1u<<5)|(1u<<7)|(1u<<8)|(1u<<11),   /* Harmonic minor */
    (1u<<0)|(1u<<1)|(1u<<4)|(1u<<5)|(1u<<7)|(1u<<8)|(1u<<10),   /* Phrygian dominant */
    (1u<<0)|(1u<<3)|(1u<<5)|(1u<<7)|(1u<<10),                   /* Minor pentatonic */
    (1u<<0)|(1u<<1)|(1u<<5)|(1u<<7)|(1u<<8),                    /* In (Miyako-bushi) */
    0x0FFFu,                                                    /* Chromatic */
};

static void clamp_key(int *root, int *scale) {
    *root = ((*root % 12) + 12) % 12;
    if (*scale < 0 || *scale >= BK_SCALE_COUNT) *scale = 0;
}

static int in_scale(int midi, int root, int scale) {
    return (scale_mask[scale] >> ((midi - root + 120) % 12)) & 1u;
}

int bk_pitch_count(int root, int scale) {
    clamp_key(&root, &scale);
    int n = 0;
    for (int m = BK_PITCH_LO; m <= BK_PITCH_HI; ++m) n += in_scale(m, root, scale);
    return n;
}

int bk_pitch_midi(int index, int root, int scale) {
    clamp_key(&root, &scale);
    if (index < 0) index = 0;
    int last = BK_PITCH_LO;
    for (int m = BK_PITCH_LO; m <= BK_PITCH_HI; ++m) {
        if (!in_scale(m, root, scale)) continue;
        if (index-- == 0) return m;
        last = m;
    }
    return last;                                  /* past the top: the top note */
}

int bk_pitch_index_of_midi(float midi, int root, int scale) {
    clamp_key(&root, &scale);
    int best = 0, i = 0;
    float bd = 1e9f;
    for (int m = BK_PITCH_LO; m <= BK_PITCH_HI; ++m) {
        if (!in_scale(m, root, scale)) continue;
        float d = fabsf((float)m - midi);
        if (d < bd - 1e-6f) { bd = d; best = i; }     /* ascending: a tie keeps the lower */
        ++i;
    }
    return best;
}

int bk_quantize_pitch(float semis, int root, int scale) {
    if (!(semis >= 0.0f)) semis = 0.0f;
    if (semis > (float)(BK_PITCH_HI - BK_PITCH_LO)) semis = (float)(BK_PITCH_HI - BK_PITCH_LO);
    root = ((root % 12) + 12) % 12;
    if (scale < 0 || scale >= BK_SCALE_COUNT) scale = 0;
    float target = (float)BK_PITCH_LO + semis;
    int best = -1;
    float best_d = 1e9f;
    for (int n = BK_PITCH_LO; n <= BK_PITCH_HI; ++n) {
        if (!(scale_mask[scale] & (1u << ((n - root + 120) % 12)))) continue;
        float d = fabsf((float)n - target);
        if (d < best_d - 1e-6f) { best_d = d; best = n; }   /* ascending: a tie keeps the lower */
    }
    return best < 0 ? BK_PITCH_LO : best;
}

void bk_note_name(int midi, char *buf, int size) {
    static const char *const names[12] = {"C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"};
    if (!buf || size <= 0) return;
    if (midi < 0) midi = 0;
    if (midi > 127) midi = 127;
    int n = 0;
    const char *s = names[midi % 12];
    while (*s && n < size - 1) buf[n++] = *s++;
    int oct = midi / 12 - 1;                       /* scientific: C1 = 32.7 Hz = MIDI 24 */
    if (n < size - 1) buf[n++] = (char)('0' + (oct < 0 ? 0 : oct));
    buf[n] = 0;
}

/* ---- knob curves ------------------------------------------------------------ */
static inline float clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}
static inline float unit(float x) { return clampf(x, 0.0f, 1.0f); }

/* VCF decay: most of the travel is the long, "breathing" region -- the middle
 * of the knob is already ~0.6 s, four 1/16s at 120 BPM. */
float bk_vcf_decay_s(float x) { return 0.08f * powf(31.25f, powf(unit(x), 0.75f)); }
float bk_vca_decay_s(float x) { return 0.05f * powf(40.0f, powf(unit(x), 0.80f)); }
/* FM: the tom -> clang travel is spread out; the top is capped where the
 * pitch would stop being a pitch. */
float bk_fm_index(float x) { return 2.6f * powf(unit(x), 1.5f); }
float bk_vcf_env_oct(float x) { return 7.0f * powf(unit(x), 1.4f); }
float bk_cutoff_hz(float x) { return 25.0f * exp2f(unit(x) * 9.3f); }
float bk_reso_k(float x) { return 3.85f * powf(unit(x), 1.4f); }
float bk_drive_gain(float x) { return 1.0f + 5.0f * powf(unit(x), 2.2f); }
float bk_penv_st(float x) { return 36.0f * powf(unit(x), 1.5f); }
float bk_penv_decay_s(float x) { return 0.005f * powf(80.0f, unit(x)); }

/* ---- building blocks -------------------------------------------------------- */
/* Rational tanh: slope 1 at 0, exactly +-1 with zero slope at |x| = 3. */
static inline float soft_tanh(float x) {
    if (x > 3.0f) return 1.0f;
    if (x < -3.0f) return -1.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

/* Per-sample multiplier reaching -60 dB in t seconds at rate fs. */
static float decay_mul(float t, float fs) {
    if (t < 0.0005f) t = 0.0005f;
    return expf(-6.9077553f / (t * fs));
}

static inline float smooth_coef(float tau_s) {
    return 1.0f - expf(-1.0f / (tau_s * BK_SR));
}

static inline float white(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return (float)(int32_t)x * (1.0f / 2147483648.0f);
}

static inline float poly_blep(float t, float dt) {
    if (dt <= 0.0f) return 0.0f;
    if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
    if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
    return 0.0f;
}

static inline void env_step(bk_env *v) {
    if (v->attacking) {
        v->v += v->att_inc;
        if (v->v >= 1.0f) { v->v = 1.0f; v->attacking = 0; }
    } else {
        v->v *= v->dec_mul;
        if (v->v < 1e-7f) v->v = 0.0f;
    }
}

/* Re-attack from wherever the envelope is: a hit never snaps a still-open
 * filter or a ringing VCA to zero, so fast retriggers are click-free. */
static inline void env_retrigger(bk_env *v) { v->attacking = 1; }

/* Half-band low-pass (windowed sinc, cutoff fs/4 of the 2x rate). */
static float g_hb[BK_HB_TAPS];
static int g_hb_ready;

static void hb_init(void) {
    if (g_hb_ready) return;
    const int mid = BK_HB_TAPS / 2;
    float sum = 0.0f;
    for (int n = 0; n < BK_HB_TAPS; ++n) {
        int m = n - mid;
        float s = m == 0 ? 0.5f : sinf(0.5f * BK_PI * (float)m) / (BK_PI * (float)m);
        float w = 0.42f - 0.5f * cosf(2.0f * BK_PI * (float)n / (float)(BK_HB_TAPS - 1))
                + 0.08f * cosf(4.0f * BK_PI * (float)n / (float)(BK_HB_TAPS - 1));
        g_hb[n] = s * w;
        sum += g_hb[n];
    }
    for (int n = 0; n < BK_HB_TAPS; ++n) g_hb[n] /= sum;
    g_hb_ready = 1;
}

/* ---- parameters ------------------------------------------------------------- */
void bk_engine_set_param(bk_engine *e, int idx, float value) {
    if (idx < 0 || idx >= BP_COUNT || !isfinite(value)) return;
    const bk_param_info *p = &bk_params[idx];
    float hi = p->max;
    if (idx == BP_PITCH)                          /* the scale's top note, not 48 */
        hi = (float)(bk_pitch_count((int)e->param[BP_ROOT], (int)e->param[BP_SCALE]) - 1);
    value = clampf(value, p->min, hi);
    if (p->is_int) value = floorf(value + 0.5f);
    e->param[idx] = value;
}

float bk_engine_value(const bk_engine *e, int idx) {
    float o = e->ovr[idx];
    return isnan(o) ? e->param[idx] : o;
}

void bk_engine_set_override(bk_engine *e, int idx, float value) {
    if (idx < 0 || idx >= BP_COUNT) return;
    if (isnan(value)) { e->ovr[idx] = NAN; return; }
    if (!isfinite(value)) return;
    const bk_param_info *p = &bk_params[idx];
    e->ovr[idx] = clampf(value, p->min, p->max);
}

static void latch_pitch(bk_engine *e) {
    e->note_midi = bk_pitch_midi((int)(bk_engine_value(e, BP_PITCH) + 0.5f),
                                 (int)e->param[BP_ROOT], (int)e->param[BP_SCALE]);
    e->note_hz = 440.0f * exp2f((float)(e->note_midi - 69) / 12.0f);
    /* the sweep depth too: changing it under a ringing note would bend it */
    e->hit_penv_st = bk_penv_st(bk_engine_value(e, BP_PENV_AMT));
}

void bk_engine_init(bk_engine *e, uint32_t seed) {
    hb_init();
    memset(e, 0, sizeof(*e));
    for (int i = 0; i < BP_COUNT; ++i) {
        e->param[i] = bk_params[i].def;
        e->ovr[i] = NAN;
    }
    e->rng = seed ? seed : 0x2545F491u;
    e->release_mul = 1.0f;
    e->amp.att_inc = 1.0f / (0.0015f * BK_FS_OS);
    e->fenv.att_inc = 1.0f / (0.0007f * BK_FS_OS);
    e->penv.att_inc = 1.0f / (0.0002f * BK_FS_OS);
    e->menv.att_inc = 1.0f / (0.0005f * BK_FS_OS);
    e->amp.dec_mul = e->fenv.dec_mul = e->penv.dec_mul = e->menv.dec_mul = 0.999f;
    latch_pitch(e);
}

void bk_engine_trigger(bk_engine *e) {
    latch_pitch(e);                  /* PITCH is the value at the hit, no glide */
    env_retrigger(&e->amp);
    env_retrigger(&e->fenv);
    env_retrigger(&e->penv);
    env_retrigger(&e->menv);
    e->releasing = 0;
    e->hits++;
}

void bk_engine_release(bk_engine *e) { e->releasing = 1; }

/* ---- render ------------------------------------------------------------------ */
#define CTRL_EVERY 16

void bk_engine_render(bk_engine *e, float *out, int frames) {
    const float a_fast = smooth_coef(0.006f);    /* cutoff, env amount, FM */
    const float a_mid = smooth_coef(0.010f);     /* mixer, VCO2 tune */
    const float a_slow = smooth_coef(0.015f);    /* reso/drive, volume */
    const float rel_mul = decay_mul(0.025f, BK_FS_OS);
    float penv_st = 0.0f, t_cut = 0, t_env = 0, t_fm = 0, t_k = 0, t_drive = 1, t_vol = 0;
    float t_vco2 = 0, t_noise = 0, t_tune = 0;

    for (int i = 0; i < frames; ++i) {
        if (i % CTRL_EVERY == 0) {
            /* control rate: targets and decay rates from the effective values
             * (knob or motion playback). Decays are NOT smoothed, so a step
             * change lands on its hit. */
            e->amp.dec_mul = decay_mul(bk_vca_decay_s(bk_engine_value(e, BP_VCA_DECAY)), BK_FS_OS);
            float vcf_t = bk_vcf_decay_s(bk_engine_value(e, BP_VCF_DECAY));
            e->fenv.dec_mul = decay_mul(vcf_t, BK_FS_OS);
            e->menv.dec_mul = decay_mul(clampf(0.5f * vcf_t, 0.04f, 0.8f), BK_FS_OS);
            e->penv.dec_mul = decay_mul(bk_penv_decay_s(bk_engine_value(e, BP_PENV_DECAY)), BK_FS_OS);
            penv_st = e->hit_penv_st;            /* latched at the hit, like PITCH */
            t_cut = log2f(bk_cutoff_hz(bk_engine_value(e, BP_CUTOFF)));
            t_env = bk_vcf_env_oct(bk_engine_value(e, BP_VCF_ENV));
            t_fm = bk_fm_index(bk_engine_value(e, BP_FM_AMOUNT));
            float rd = bk_engine_value(e, BP_RESO_DRIVE);
            t_k = bk_reso_k(rd);
            t_drive = bk_drive_gain(rd);
            float v = e->param[BP_VOLUME];
            t_vol = v * v;
            t_vco2 = e->param[BP_VCO2_LEVEL];
            t_noise = e->param[BP_NOISE];
            t_tune = e->param[BP_VCO2_TUNE];
            if (!e->smooth_primed) {
                e->s_cut_oct = t_cut; e->s_env_oct = t_env; e->s_fm = t_fm;
                e->s_k = t_k; e->s_drive = t_drive; e->s_vol = t_vol;
                e->s_vco2 = t_vco2; e->s_noise = t_noise; e->s_tune = t_tune;
                e->smooth_primed = 1;
            }
        }
        e->s_cut_oct += a_fast * (t_cut - e->s_cut_oct);
        e->s_env_oct += a_fast * (t_env - e->s_env_oct);
        e->s_fm += a_fast * (t_fm - e->s_fm);
        e->s_k += a_slow * (t_k - e->s_k);
        e->s_drive += a_slow * (t_drive - e->s_drive);
        e->s_vol += a_slow * (t_vol - e->s_vol);
        if (t_vol == 0.0f && e->s_vol < 1e-4f) e->s_vol = 0.0f;    /* VOLUME 0 = true silence */
        e->s_vco2 += a_mid * (t_vco2 - e->s_vco2);
        e->s_noise += a_mid * (t_noise - e->s_noise);
        e->s_tune += a_mid * (t_tune - e->s_tune);

        /* ladder coefficient, once per output sample */
        float fc = exp2f(e->s_cut_oct + e->s_env_oct * e->fenv.v);
        fc = clampf(fc, 20.0f, 18000.0f);
        float g = 1.0f - expf(-2.0f * BK_PI * fc / BK_FS_OS);
        /* the one-sample feedback delay makes a high cutoff ring harder: ease
         * the feedback off towards the top so nothing runs away */
        float k = e->s_k * (1.0f - 0.25f * fc / 18000.0f);
        float drive = e->s_drive;
        float fm_shape = clampf(e->s_fm / 2.6f, 0.0f, 1.0f);
        fm_shape *= fm_shape;                    /* modulator: triangle -> square */
        float ratio2 = exp2f(e->s_tune / 12.0f);

        for (int o = 0; o < BK_OS; ++o) {
            env_step(&e->amp);
            env_step(&e->fenv);
            env_step(&e->penv);
            env_step(&e->menv);
            if (e->releasing) e->release_mul *= rel_mul;
            else e->release_mul = 1.0f;
            if (e->release_mul < 1e-6f) e->release_mul = 0.0f;

            float f1 = e->note_hz * exp2f(penv_st * e->penv.v * (1.0f / 12.0f));
            float inc1 = clampf(f1 / BK_FS_OS, 0.0f, 0.45f);
            float t1 = e->ph1;
            float sq = t1 < 0.5f ? 1.0f : -1.0f;
            sq += poly_blep(t1, inc1);
            float t1h = t1 + 0.5f; if (t1h >= 1.0f) t1h -= 1.0f;
            sq -= poly_blep(t1h, inc1);
            float tri1 = 4.0f * fabsf(t1 - 0.5f) - 1.0f;
            e->ph1 = t1 + inc1;
            if (e->ph1 >= 1.0f) e->ph1 -= 1.0f;

            /* VCO1 -> VCO2 linear through-zero FM; the index opens with the
             * FM env and never fully closes (35 % floor), so FM AMOUNT is a
             * colour as well as an attack. */
            float mod = tri1 + fm_shape * (sq - tri1);
            float idx = e->s_fm * (0.35f + 0.65f * e->menv.v);
            float inc2 = clampf(f1 * ratio2 / BK_FS_OS, 0.0f, 0.45f) * (1.0f + idx * mod);
            float ph2 = e->ph2 + inc2;
            ph2 -= floorf(ph2);
            e->ph2 = ph2;
            float tri2 = 4.0f * fabsf(ph2 - 0.5f) - 1.0f;

            float x = 0.5f * sq + 0.5f * e->s_vco2 * tri2 + 0.45f * e->s_noise * white(&e->rng);

            /* 4-pole transistor-ladder style LP with a tanh at every stage and
             * in the feedback path (Huovilainen-type), bass-compensated so
             * resonance does not hollow the body out. */
            float u = drive * x;
            float t0 = soft_tanh(u - k * (e->y[3] - 0.5f * u));
            e->y[0] += g * (t0 - e->w[0]);  e->w[0] = soft_tanh(e->y[0]);
            e->y[1] += g * (e->w[0] - e->w[1]);  e->w[1] = soft_tanh(e->y[1]);
            e->y[2] += g * (e->w[1] - e->w[2]);  e->w[2] = soft_tanh(e->y[2]);
            e->y[3] += g * (e->w[2] - e->w[3]);  e->w[3] = soft_tanh(e->y[3]);
            float y = e->y[3];

            /* drive makeup: the tanh stages already bound the level, so the
             * macro gets louder and denser, not a volume jump */
            y *= 1.0f / (0.55f + 0.45f * sqrtf(drive));
            y *= e->amp.v * e->release_mul;

            e->hb_hist[e->hb_pos] = y;
            e->hb_pos = e->hb_pos + 1 == BK_HB_TAPS ? 0 : e->hb_pos + 1;
        }
        float acc = 0.0f;
        int p = e->hb_pos;                       /* oldest sample */
        for (int n = 0; n < BK_HB_TAPS; ++n) {
            acc += g_hb[n] * e->hb_hist[p];
            p = p + 1 == BK_HB_TAPS ? 0 : p + 1;
        }
        /* DC block (~7 Hz) */
        float yv = acc - e->dc_x + 0.999f * e->dc_y;
        e->dc_x = acc;
        e->dc_y = fabsf(yv) < 1e-12f ? 0.0f : yv;
        /* output stage: soft knee from 0.6, ceiling 1.0 -- the loudest
         * FM/drive corner saturates instead of clipping, and it sits BEFORE
         * VOLUME so the knob is a plain level (0 = silence, 100 % = 0 dBFS
         * ceiling), not a second drive */
        float s = 1.2f * e->dc_y, a = fabsf(s);
        if (a > 0.6f) s = copysignf(0.6f + 0.4f * soft_tanh((a - 0.6f) * 2.5f), s);
        s *= e->s_vol;
        out[i] = isfinite(s) ? s : 0.0f;
    }
    /* a NaN can only come from a broken state; never let it stick */
    for (int n = 0; n < 4; ++n)
        if (!isfinite(e->y[n]) || !isfinite(e->w[n])) { e->y[n] = e->w[n] = 0.0f; }
}
