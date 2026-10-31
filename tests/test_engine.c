/* DFORM engine / clock / motion tests. Built with ASan+UBSan by
 * scripts/test.sh.
 *   test-engine                 run the tests
 *   test-engine --wav <file>    also render a demo 1/16 roll (mono 16-bit) */
#include "bk_engine.h"
#include "bk_clock.h"
#include "bk_motion.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static bk_engine E;
static bk_motion M;
#define BUF_N (44100 * 12)
static float buf[BUF_N];

static int in_phrygian_c(int midi) {
    static const int deg[7] = {0, 1, 3, 5, 7, 8, 10};
    for (int i = 0; i < 7; ++i) if (midi % 12 == deg[i]) return 1;
    return 0;
}

static float rms_db(const float *x, int n) {
    double s = 0;
    for (int i = 0; i < n; ++i) s += (double)x[i] * x[i];
    return 10.0f * log10f((float)(s / (n > 0 ? n : 1)) + 1e-20f);
}

static float peak(const float *x, int n) {
    float p = 0;
    for (int i = 0; i < n; ++i) if (fabsf(x[i]) > p) p = fabsf(x[i]);
    return p;
}

/* `seconds` of a hit every 1/16 at `bpm` (the plugin's job, done by hand). */
static int roll(bk_engine *e, float *out, float seconds, float bpm) {
    int total = (int)(seconds * BK_SR);
    if (total > BUF_N) total = BUF_N;
    int step = (int)(BK_SR * 60.0f / bpm / 4.0f);
    int i = 0;
    while (i < total) {
        if (i % step == 0) bk_engine_trigger(e);
        int n = step - i % step;
        if (n > 128) n = 128;
        if (i + n > total) n = total - i;
        bk_engine_render(e, out + i, n);
        i += n;
    }
    return total;
}

static void test_scale(void) {
    int prev = 0, seen[128] = {0};
    for (int i = 0; i <= 1000; ++i) {
        int n = bk_quantize_pitch(48.0f * i / 1000.0f, 0, 0);
        CHECK(n >= BK_PITCH_LO && n <= BK_PITCH_HI, "range %d", n);
        CHECK(in_phrygian_c(n), "C Phrygian only: %d", n);
        CHECK(n >= prev, "monotonic");
        prev = n;
        seen[n] = 1;
    }
    CHECK(bk_quantize_pitch(0.0f, 0, 0) == 24 && bk_quantize_pitch(48.0f, 0, 0) == 72, "C1..C5");
    for (int n = 24; n <= 72; ++n) CHECK(seen[n] == in_phrygian_c(n), "degree %d reachable", n);
    /* the integer knob: every semitone position is a note of the scale or
     * snaps to a neighbour one semitone away, never further */
    for (int s = 0; s <= 48; ++s)
        CHECK(abs(bk_quantize_pitch((float)s, 0, 0) - (24 + s)) <= 1, "position %d", s);
    CHECK(bk_quantize_pitch(2.0f, 0, 0) == 25, "D -> Db (tie goes down)");
    CHECK(bk_quantize_pitch(3.0f, 0, 0) == 27, "Eb stays Eb");
    /* root + scale move the set; chromatic is plain rounding */
    for (int root = 0; root < 12; ++root)
        for (int sc = 0; sc < BK_SCALE_COUNT; ++sc)
            for (int i = 0; i <= 100; ++i) {
                int n = bk_quantize_pitch(0.48f * i, root, sc);
                CHECK(n >= BK_PITCH_LO && n <= BK_PITCH_HI, "range");
                if (sc == BK_SCALE_COUNT - 1)
                    CHECK(fabsf((float)n - (24.0f + i * 0.48f)) <= 0.5f, "chromatic %d", n);
                if (sc == 0) {
                    int rel = ((n - root) % 12 + 12) % 12;
                    CHECK(in_phrygian_c(rel), "phrygian on root %d: %d", root, n);
                }
            }
    char name[8];
    bk_note_name(24, name, sizeof(name)); CHECK(!strcmp(name, "C1"), "%s", name);
    bk_note_name(39, name, sizeof(name)); CHECK(!strcmp(name, "Eb2"), "%s", name);
    bk_note_name(72, name, sizeof(name)); CHECK(!strcmp(name, "C5"), "%s", name);
    CHECK(bk_quantize_pitch(NAN, 0, 0) == 24 && bk_quantize_pitch(900.0f, 0, 0) == 72, "junk safe");
    /* the knob IS a note of the scale: index -> note, every one in the scale */
    CHECK(bk_pitch_count(0, 0) == 29, "C Phrygian has 29 notes in C1..C5: %d", bk_pitch_count(0, 0));
    CHECK(bk_pitch_count(0, BK_SCALE_COUNT - 1) == 49, "chromatic 49");
    CHECK(bk_pitch_midi(0, 0, 0) == 24 && bk_pitch_midi(7, 0, 0) == 36 && bk_pitch_midi(28, 0, 0) == 72, "index -> note");
    CHECK(bk_pitch_midi(99, 0, 0) == 72 && bk_pitch_midi(-3, 0, 0) == 24, "index clamps");
    for (int sc = 0; sc < BK_SCALE_COUNT; ++sc)
        for (int root = 0; root < 12; ++root) {
            int cnt = bk_pitch_count(root, sc), prevm = 0;
            for (int i = 0; i < cnt; ++i) {
                int m = bk_pitch_midi(i, root, sc);
                CHECK(m > prevm, "ascending, no repeats (every detent is a new note)");
                prevm = m;
                CHECK(bk_pitch_index_of_midi((float)m, root, sc) == i, "round trip");
                if (sc == 0) CHECK(in_phrygian_c(((m - root) % 12 + 12) % 12), "in scale");
            }
        }
    int j = bk_quantize_pitch(900.0f, 99, -3);
    CHECK(j >= BK_PITCH_LO && j <= BK_PITCH_HI, "junk root/scale safe: %d", j);
}

static void test_curves(void) {
    CHECK(fabsf(bk_vcf_decay_s(0) - 0.08f) < 1e-4f && fabsf(bk_vcf_decay_s(1) - 2.5f) < 1e-3f, "vcf decay 80ms..2.5s");
    CHECK(bk_vcf_decay_s(0.5f) > 0.5f, "VCF decay long at mid-knob: %.3f", bk_vcf_decay_s(0.5f));
    CHECK(fabsf(bk_vca_decay_s(0) - 0.05f) < 1e-4f && fabsf(bk_vca_decay_s(1) - 2.0f) < 1e-3f, "vca decay 50ms..2s");
    CHECK(bk_vca_decay_s(0.5f) > 0.25f, "VCA mid decay > two 1/16 at 120 BPM");
    CHECK(fabsf(bk_cutoff_hz(0) - 25.0f) < 0.01f && bk_cutoff_hz(1) > 15000.0f, "cutoff 25 Hz..15.8 kHz");
    CHECK(bk_reso_k(1) < 4.0f, "never the full self-oscillation gain");
    CHECK(bk_vcf_env_oct(0.1f) < 0.3f, "low env amount ~ fixed cutoff");
    CHECK(bk_fm_index(1) <= 2.6f, "FM index capped");
}

static void test_clock(void) {
    bk_clock c;
    bk_clock_init(&c);
    int offs[BK_CLOCK_MAX_EVENTS], hits = 0;
    for (int b = 0; b < 100; ++b) CHECK(bk_clock_advance(&c, -1.0, 120, 128, offs) == 0, "stopped: silent");
    double beat = 0;
    for (int b = 0; b < (int)(2 * 44100 / 128); ++b) {
        hits += bk_clock_advance(&c, beat, 120, 128, offs);
        beat += 128.0 / (44100.0 * 0.5);
    }
    CHECK(hits == 16, "16 x 1/16 in 2 s at 120 BPM: %d", hits);
}

static void test_retrigger(void) {
    bk_engine_init(&E, 1);
    bk_engine_set_param(&E, BP_VCA_DECAY, 1.0f);
    bk_engine_set_param(&E, BP_VCF_DECAY, 1.0f);
    bk_engine_trigger(&E);
    bk_engine_render(&E, buf, 6000);
    float before = E.amp.v, fbefore = E.fenv.v;
    CHECK(before > 0.3f && before < 1.0f, "amp mid-decay %.3f", before);
    bk_engine_trigger(&E);
    bk_engine_render(&E, buf, 1);
    CHECK(E.amp.v >= before && E.fenv.v >= fbefore, "re-attack from the CURRENT value, never from 0");

    /* long VCA decay: the next 1/16 arrives with the last still sounding */
    bk_engine_init(&E, 1);
    bk_engine_set_param(&E, BP_VCA_DECAY, 0.9f);
    float mn = 1;
    for (int i = 0; i < 16; ++i) {
        bk_engine_trigger(&E);
        for (int j = 0; j < 5504; j += 128) {
            bk_engine_render(&E, buf, 128);
            if (i > 0 && E.amp.v < mn) mn = E.amp.v;
        }
    }
    CHECK(mn > 0.3f, "long VCA decay overlaps the next 1/16: min amp %.3f", mn);

    /* no click at a retrigger: a pure low tone, hit mid-decay; the largest
     * sample step at the hit is no bigger than the tone's own steps allow */
    bk_engine_init(&E, 3);
    bk_engine_set_param(&E, BP_CUTOFF, 0.15f);
    bk_engine_set_param(&E, BP_VCF_ENV, 0.0f);
    bk_engine_set_param(&E, BP_FM_AMOUNT, 0.0f);
    bk_engine_set_param(&E, BP_NOISE, 0.0f);
    bk_engine_set_param(&E, BP_PENV_AMT, 0.0f);
    bk_engine_set_param(&E, BP_RESO_DRIVE, 0.0f);
    bk_engine_set_param(&E, BP_VCA_DECAY, 0.8f);
    bk_engine_trigger(&E);
    bk_engine_render(&E, buf, 8000);
    float steady = 0;
    for (int i = 4001; i < 8000; ++i) steady = fmaxf(steady, fabsf(buf[i] - buf[i - 1]));
    float last = buf[7999];
    bk_engine_trigger(&E);
    bk_engine_render(&E, buf, 200);
    float jump = fabsf(buf[0] - last);
    for (int i = 1; i < 200; ++i) jump = fmaxf(jump, fabsf(buf[i] - buf[i - 1]));
    CHECK(jump < 3.0f * steady + 1e-3f, "retrigger is click-free: step %.4f vs steady %.4f", jump, steady);
}

static void test_volume_and_release(void) {
    bk_engine_init(&E, 5);
    bk_engine_set_param(&E, BP_VOLUME, 0.0f);
    int n = roll(&E, buf, 1.0f, 120);
    int nonzero = 0;
    for (int i = 4410; i < n; ++i) nonzero += lrintf(buf[i] * 32767.0f) != 0;
    CHECK(nonzero == 0, "VOLUME 0 = digital silence (%d samples)", nonzero);

    bk_engine_init(&E, 5);
    bk_engine_set_param(&E, BP_VCA_DECAY, 1.0f);
    roll(&E, buf, 0.5f, 120);
    bk_engine_release(&E);
    bk_engine_render(&E, buf, 4410);                 /* 100 ms */
    CHECK(peak(buf + 2205, 2205) < 0.001f, "transport stop: silent within 100 ms (%.5f)", peak(buf + 2205, 2205));
    bk_engine_trigger(&E);                           /* the next Play sounds again */
    bk_engine_render(&E, buf, 2000);
    CHECK(peak(buf, 2000) > 0.05f, "a hit after a release sounds");
}

/* RESONANCE / DRIVE: louder and denser, never a jump. */
static void test_reso_drive_level(void) {
    float prev = 0, lo = 1e9, hi = -1e9;
    for (int s = 0; s <= 20; ++s) {
        bk_engine_init(&E, 7);
        bk_engine_set_param(&E, BP_RESO_DRIVE, s / 20.0f);
        int n = roll(&E, buf, 1.0f, 120);
        float db = rms_db(buf + 4410, n - 4410);
        float pk = peak(buf, n);
        CHECK(pk < 1.0f, "no clipping at res/drive %.2f: peak %.3f", s / 20.0f, pk);
        if (s) CHECK(fabsf(db - prev) < 2.5f, "res/drive %.2f -> step %.2f dB", s / 20.0f, db - prev);
        prev = db;
        lo = fminf(lo, db); hi = fmaxf(hi, db);
    }
    CHECK(hi - lo < 9.0f, "res/drive level spread %.1f dB", hi - lo);
}

/* Every combination stays finite and inside full scale. */
static void test_extremes(void) {
    const float v[3] = {0.0f, 0.5f, 1.0f};
    int worst_clip = 0;
    for (int a = 0; a < 3; ++a) for (int b = 0; b < 3; ++b) for (int c = 0; c < 3; ++c)
    for (int d = 0; d < 3; ++d) for (int f = 0; f < 3; ++f) {
        bk_engine_init(&E, 11);
        bk_engine_set_param(&E, BP_FM_AMOUNT, v[a]);
        bk_engine_set_param(&E, BP_CUTOFF, v[b]);
        bk_engine_set_param(&E, BP_RESO_DRIVE, v[c]);
        bk_engine_set_param(&E, BP_VCF_ENV, v[d]);
        bk_engine_set_param(&E, BP_PITCH, 28.0f * v[f]);
        bk_engine_set_param(&E, BP_VOLUME, 1.0f);
        bk_engine_set_param(&E, BP_NOISE, 1.0f);
        bk_engine_set_param(&E, BP_VCO2_LEVEL, 1.0f);
        int n = roll(&E, buf, 0.3f, 180);
        int finite = 1;
        for (int i = 0; i < n; ++i) finite &= isfinite(buf[i]);
        CHECK(finite, "finite");
        if (peak(buf, n) > 1.0f) worst_clip++;
    }
    CHECK(worst_clip == 0, "%d of 243 extreme settings clip at VOLUME 100%%", worst_clip);
}

/* Cutoff/FM/env/res steps are smoothed (no zipper); PITCH and the decays are not. */
static void test_smoothing(void) {
    bk_engine_init(&E, 9);
    bk_engine_set_param(&E, BP_CUTOFF, 0.1f);
    bk_engine_trigger(&E);
    bk_engine_render(&E, buf, 1000);
    bk_engine_set_param(&E, BP_CUTOFF, 0.9f);
    float prev = E.s_cut_oct, maxd = 0;
    for (int i = 0; i < 2000; ++i) {
        bk_engine_render(&E, buf, 1);
        maxd = fmaxf(maxd, fabsf(E.s_cut_oct - prev));
        prev = E.s_cut_oct;
    }
    CHECK(maxd < 0.05f, "cutoff jump of 7.4 octaves is spread out: max %.3f oct/sample", maxd);
    CHECK(fabsf(E.s_cut_oct - log2f(bk_cutoff_hz(0.9f))) < 0.01f, "...and arrives within 45 ms");

    bk_engine_init(&E, 9);
    bk_engine_trigger(&E);
    bk_engine_render(&E, buf, 128);
    float hz = E.note_hz;
    float st = E.hit_penv_st;
    bk_engine_set_param(&E, BP_PITCH, 20.0f);
    bk_engine_set_param(&E, BP_PENV_AMT, 1.0f);
    bk_engine_render(&E, buf, 2000);
    CHECK(E.note_hz == hz, "PITCH does not move between hits (no glide)");
    CHECK(E.hit_penv_st == st, "nor does the P.ENV depth");
    bk_engine_trigger(&E);
    CHECK(E.note_midi == bk_pitch_midi(20, 0, 0), "PITCH lands exactly on the next hit");
    bk_engine_set_param(&E, BP_PITCH, 40.0f);
    CHECK(E.param[BP_PITCH] == 28.0f, "PITCH clamps to the scale's top note");
    CHECK(fabsf(E.hit_penv_st - 36.0f) < 1e-3f, "P.ENV lands on the next hit");
    bk_engine_set_param(&E, BP_VCA_DECAY, 0.0f);
    bk_engine_render(&E, buf, 16);
    CHECK(fabsf(E.amp.dec_mul - expf(-6.9077553f / (0.05f * BK_SR * BK_OS))) < 1e-6f, "decay applies at once");
}

/* ---- motion ------------------------------------------------------------- */
static void at(double pos16) { bk_motion_update(&M, &E, pos16); }

static void test_motion(void) {
    bk_engine_init(&E, 1);
    bk_motion_init(&M);
    CHECK(M.write == 0, "REC pad starts off");
    M.write = 1;
    int cut = bk_motion_lane(BP_CUTOFF), pit = bk_motion_lane(BP_PITCH);
    CHECK(bk_motion_lane(BP_VOLUME) < 0, "VOLUME is never automated");
    CHECK(cut >= 0 && pit >= 0, "lanes");

    /* untouched: nothing is written, whatever the knob does */
    for (double p = 0; p < 16; p += 0.05) { bk_engine_set_param(&E, BP_CUTOFF, (float)(p / 16)); at(p); }
    CHECK(bk_motion_mask(&M) == 0, "no touch, no automation");

    /* touch at 1/16 #2, sweep the knob 0.2 -> 0.8 across one 1/16, release */
    bk_engine_set_param(&E, BP_CUTOFF, 0.2f);
    at(2.0);
    bk_motion_touch(&M, &E, cut, 1);
    CHECK(bk_motion_recording(&M, &E) == (1u << cut), "recording while touched + running");
    for (int i = 0; i <= 64; ++i) {
        double p = 2.0 + i / 64.0;
        bk_engine_set_param(&E, BP_CUTOFF, 0.2f + 0.6f * (float)(i / 64.0));
        at(p);
        CHECK(isnan(E.ovr[BP_CUTOFF]), "you hear the hand while touching");
    }
    bk_motion_touch(&M, &E, cut, 0);
    bk_engine_set_param(&E, BP_CUTOFF, 0.5f);          /* knob left elsewhere */
    CHECK(bk_motion_mask(&M) == (1u << cut), "one lane");
    /* written: points 16..24 (#2 and the first point of #3), nothing else */
    int written = 0;
    for (int i = 0; i < BK_POINTS; ++i) written += M.pt[cut][i] != BK_EMPTY;
    CHECK(written == 9, "touch time = write time: %d points", written);
    /* playback retraces the sweep INSIDE the 1/16 (not a staircase) */
    at(2.5);
    CHECK(fabsf(bk_engine_value(&E, BP_CUTOFF) - 0.5f) < 0.02f, "mid-step curve: %.3f", bk_engine_value(&E, BP_CUTOFF));
    at(2.25);
    CHECK(fabsf(bk_engine_value(&E, BP_CUTOFF) - 0.35f) < 0.02f, "quarter-step: %.3f", bk_engine_value(&E, BP_CUTOFF));
    at(5.0);
    CHECK(isnan(E.ovr[BP_CUTOFF]) && bk_engine_value(&E, BP_CUTOFF) == 0.5f, "unwritten region plays the knob");
    at(16 + 2.5);                                      /* next loop (1 bar) */
    CHECK(fabsf(bk_engine_value(&E, BP_CUTOFF) - 0.5f) < 0.02f, "loops every bar");
    CHECK(E.param[BP_CUTOFF] == 0.5f, "playback never writes the knob");

    /* PITCH, step input: a value held across #4 and #5 is the value AT each hit */
    at(4.0);
    bk_motion_touch(&M, &E, pit, 1);
    bk_engine_set_param(&E, BP_PITCH, 20.0f);
    at(4.0);
    CHECK(M.pt[pit][32] == bk_motion_q(7.0f / 48.0f), "the step already started keeps its start value");
    at(4.5);
    at(5.0);
    bk_motion_touch(&M, &E, pit, 0);
    bk_engine_set_param(&E, BP_PITCH, 3.0f);
    at(21.0);                                          /* #5 of the next loop, at its hit */
    CHECK(bk_engine_value(&E, BP_PITCH) == 20.0f, "step pitch at the hit, an exact scale note: %.4f",
          bk_engine_value(&E, BP_PITCH));
    bk_engine_trigger(&E);
    CHECK(E.note_midi == bk_pitch_midi(20, 0, 0), "the hit plays the recorded pitch");
    at(20.0);
    CHECK(bk_engine_value(&E, BP_PITCH) == 7.0f, "#4 plays its start value");
    at(20.1);
    CHECK(bk_engine_value(&E, BP_PITCH) == 7.0f, "pitch lane holds, no interpolation");
    at(21.5);
    CHECK(isnan(E.ovr[BP_PITCH]), "released at #5's start: the rest of #5 is the knob's");

    /* touching an automated knob picks up what is playing: no jump */
    at(18.5);
    float playing = bk_engine_value(&E, BP_CUTOFF);
    bk_motion_touch(&M, &E, cut, 1);
    CHECK(fabsf(bk_engine_value(&E, BP_CUTOFF) - playing) < 1e-3f, "touch picks up the playing value");
    bk_motion_touch(&M, &E, cut, 0);

    /* jitter one point back must not "wrap" and rewrite the whole loop */
    int before_n = 0;
    for (int i = 0; i < BK_POINTS; ++i) before_n += M.pt[cut][i] != BK_EMPTY;
    at(30.0);
    bk_motion_touch(&M, &E, cut, 1);
    at(29.99);
    at(30.0);
    bk_motion_touch(&M, &E, cut, 0);
    written = 0;
    for (int i = 0; i < BK_POINTS; ++i) written += M.pt[cut][i] != BK_EMPTY;
    CHECK(written - before_n <= 1, "jitter is not a lap: %d new points", written - before_n);

    /* REC pad off: touch is read-only */
    M.write = 0;
    unsigned before = bk_motion_mask(&M);
    bk_motion_touch(&M, &E, bk_motion_lane(BP_FM_AMOUNT), 1);
    for (double p = 0; p < 16; p += 0.1) at(p);
    bk_motion_touch(&M, &E, bk_motion_lane(BP_FM_AMOUNT), 0);
    CHECK(bk_motion_mask(&M) == before, "REC pad off writes nothing");
    M.write = 1;

    /* stopped: touch records nothing, overrides are gone */
    at(-1);
    CHECK(isnan(E.ovr[BP_CUTOFF]) && isnan(E.ovr[BP_PITCH]), "stopped: knobs play");
    bk_motion_touch(&M, &E, bk_motion_lane(BP_VCA_DECAY), 1);
    CHECK(bk_motion_recording(&M, &E) == 0, "stopped: not recording");
    bk_motion_touch(&M, &E, bk_motion_lane(BP_VCA_DECAY), 0);

    /* LENGTH: a 1/2-bar loop wraps at 8 */
    bk_engine_set_param(&E, BP_LENGTH, 0);
    at(8 + 2.5);
    CHECK(fabsf(bk_engine_value(&E, BP_CUTOFF) - 0.5f) < 0.02f, "8-step loop");
    CHECK(M.step == 2, "step %d", M.step);

    /* a touch nobody restates ends by itself (screen closed mid-touch) */
    at(1.0);
    int fm = bk_motion_lane(BP_FM_AMOUNT);
    bk_motion_touch(&M, &E, fm, 1);
    for (int i = 0; i < 100; ++i) {
        bk_motion_age(&M, 128);
        if (i % 20 == 0) bk_motion_touch(&M, &E, fm, 1);    /* heartbeat every ~58 ms */
    }
    CHECK(M.touched & (1u << fm), "restated touch stays");
    bk_motion_age(&M, BK_TOUCH_TIMEOUT_SAMPLES + 1);
    CHECK(!(M.touched & (1u << fm)), "an unrestated touch times out");
    bk_motion_clear(&M, &E, fm);

    /* the pitch envelope and the page-2 knobs record too */
    int pe = bk_motion_lane(BP_PENV_AMT), pd = bk_motion_lane(BP_PENV_DECAY);
    int vca = bk_motion_lane(BP_VCA_DECAY), venv = bk_motion_lane(BP_VCF_ENV);
    CHECK(pe >= 0 && pd >= 0 && vca >= 0 && venv >= 0, "P.ENV P.DEC VCA DEC VCF ENV are lanes");
    at(3.0);
    bk_motion_touch(&M, &E, pe, 1);
    bk_engine_set_param(&E, BP_PENV_AMT, 0.8f);
    at(3.5);
    bk_motion_touch(&M, &E, pe, 0);
    bk_engine_set_param(&E, BP_PENV_AMT, 0.1f);
    at(8 + 3.5);
    CHECK(fabsf(bk_engine_value(&E, BP_PENV_AMT) - 0.8f) < 1e-3f, "P.ENV plays back");

    bk_motion_clear(&M, &E, cut);
    CHECK(!(bk_motion_mask(&M) & (1u << cut)) && isnan(E.ovr[BP_CUTOFF]), "clear one lane");
    bk_motion_clear(&M, &E, -1);
    CHECK(bk_motion_mask(&M) == 0, "clear all");
}

static void write_wav(const char *path) {
    /* 8 s at 120 BPM: defaults; then the decays opened up; then FM + res */
    bk_engine_init(&E, 42);
    int n = roll(&E, buf, 2.0f, 120);
    bk_engine_set_param(&E, BP_VCA_DECAY, 0.85f);
    bk_engine_set_param(&E, BP_VCF_DECAY, 0.85f);
    bk_engine_set_param(&E, BP_VCF_ENV, 0.85f);
    n += roll(&E, buf + n, 2.0f, 120);
    bk_engine_set_param(&E, BP_FM_AMOUNT, 0.7f);
    bk_engine_set_param(&E, BP_RESO_DRIVE, 0.8f);
    n += roll(&E, buf + n, 2.0f, 120);
    static const float seq[8] = {6, 6, 15, 11, 6, 15, 6, 11};   /* C Phrygian note indices */
    for (int s = 0; s < 16 && n < BUF_N - 6000; ++s) {
        bk_engine_set_param(&E, BP_PITCH, seq[s % 8]);
        bk_engine_set_param(&E, BP_CUTOFF, 0.25f + 0.3f * (float)s / 16.0f);
        n += roll(&E, buf + n, 0.125f, 120);
    }
    FILE *f = fopen(path, "wb");
    if (!f) { failures++; return; }
    int bytes = n * 2;
    unsigned char h[44] = {'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,1,0,
                           0x44,0xAC,0,0,0x88,0x58,0x01,0,2,0,16,0,'d','a','t','a',0,0,0,0};
    int riff = 36 + bytes;
    memcpy(h + 4, &riff, 4);
    memcpy(h + 40, &bytes, 4);
    fwrite(h, 1, 44, f);
    for (int i = 0; i < n; ++i) {
        float x = fmaxf(-1.0f, fminf(1.0f, buf[i]));
        short s = (short)lrintf(x * 32767.0f);
        fwrite(&s, 2, 1, f);
    }
    fclose(f);
    printf("wrote %s (%.1f s, peak %.3f)\n", path, n / BK_SR, peak(buf, n));
}

static void bench(void) {
    bk_engine_init(&E, 1);
    clock_t t0 = clock();
    int blocks = 44100 * 4 / 128;
    for (int b = 0; b < blocks; ++b) {
        if (b % 43 == 0) bk_engine_trigger(&E);
        bk_engine_render(&E, buf, 128);
    }
    double us = (double)(clock() - t0) / CLOCKS_PER_SEC * 1e6 / blocks;
    printf("info: %.1f us per 128-frame block on this machine (budget 2902 us)\n", us);
}

int main(int argc, char **argv) {
    test_scale();
    test_curves();
    test_clock();
    test_retrigger();
    test_volume_and_release();
    test_reso_drive_level();
    test_extremes();
    test_smoothing();
    test_motion();
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--wav")) write_wav(argv[i + 1]);
    }
    bench();
    if (failures) { printf("%d FAILED\n", failures); return 1; }
    printf("PASS: scale, curves, clock, retrigger, volume/release, res/drive level, extremes, smoothing, motion\n");
    return 0;
}
