/* DFORM - one-voice auto-triggered analog-style percussion engine. MIT.
 *
 * Original implementation (textbook subtractive/FM building blocks, no
 * third-party code), in the spirit of a two-VCO + FM + ladder percussion
 * synth:
 *
 *   VCO1 (square, polyBLEP) --FM--> VCO2 (triangle, through-zero linear FM)
 *     both <- pitch env;  FM index <- FM env
 *   VCO1 + VCO2 + noise -> drive -> 4-pole nonlinear ladder LP (cutoff <- VCF env)
 *     -> VCA (amp env) -> DC block -> VOLUME
 *
 * The oscillators and the ladder run at 2x (88.2 kHz) and are decimated by a
 * half-band FIR. A hit retriggers all four envelopes FROM THEIR CURRENT VALUE
 * (no reset to zero, no oscillator phase reset), so a 1/16 roll with decays
 * longer than a step overlaps into one continuous, moving sound, click-free.
 *
 * Every function here is called from the SPI audio callback: no allocation,
 * no I/O, bounded work. */
#ifndef BK_ENGINE_H
#define BK_ENGINE_H

#include <stdint.h>

#define BK_SR 44100.0f
#define BK_OS 2
#define BK_HB_TAPS 31            /* half-band decimator length (odd) */

enum {
    /* main page, knobs 1-8 */
    BP_VOLUME, BP_VCF_DECAY, BP_VCA_DECAY, BP_FM_AMOUNT,
    BP_VCF_ENV, BP_CUTOFF, BP_RESO_DRIVE, BP_PITCH,
    /* Tone page */
    BP_ROOT, BP_SCALE, BP_PENV_AMT, BP_PENV_DECAY, BP_VCO2_TUNE, BP_VCO2_LEVEL, BP_NOISE,
    /* Motion page (plugin-level). Recording is armed by the REC PAD, which is
     * runtime state (bk_motion.write), not a parameter. */
    BP_LENGTH,
    BP_COUNT
};

typedef struct {
    const char *key;
    float min, max, def;
    int is_int;              /* enum: integral values only */
} bk_param_info;

extern const bk_param_info bk_params[BP_COUNT];

/* ---- pitch: the knob IS a note of the scale ---------------------------------
 * PITCH is an integer INDEX into the notes of (ROOT, SCALE) between MIDI 24
 * (C1) and 72 (C5): every detent is the next scale note, nothing is quantized
 * afterwards (29 notes in C Phrygian, 49 chromatic, so BP_PITCH's table max
 * is 48 and the served chain_params carry the current scale's top). An int,
 * not a float: Schwung's knob engine moves a float 0.5 % of its range per
 * detent (200 detents end to end). */
#define BK_PITCH_LO 24
#define BK_PITCH_HI 72
#define BK_SCALE_COUNT 12
extern const char *const bk_scale_names[BK_SCALE_COUNT];
int  bk_pitch_count(int root, int scale);                 /* notes in C1..C5 */
int  bk_pitch_midi(int index, int root, int scale);       /* clamps the index */
int  bk_pitch_index_of_midi(float midi, int root, int scale);   /* nearest; tie goes down */
/* Nearest note of (root, scale) to BK_PITCH_LO + semis; ties go down. Only
 * used to read v1/v2 states, whose PITCH was a semitone position. */
int  bk_quantize_pitch(float semis, int root, int scale);
/* "C1", "Db2", ... (flats, as Move's own UI). buf >= 5. */
void bk_note_name(int midi, char *buf, int size);

/* ---- knob -> physical value (documented ranges, see README) ------------- */
float bk_vcf_decay_s(float x);     /* 0.08 .. 2.5 s   (to -60 dB)  */
float bk_vca_decay_s(float x);     /* 0.05 .. 2.0 s   (to -60 dB)  */
float bk_fm_index(float x);        /* 0 .. 2.6 peak linear-FM index */
float bk_vcf_env_oct(float x);     /* 0 .. 7 octaves */
float bk_cutoff_hz(float x);       /* 25 Hz .. 15.8 kHz */
float bk_reso_k(float x);          /* ladder feedback 0 .. 3.85 (4 = self-osc) */
float bk_drive_gain(float x);      /* 1 .. 6 into the ladder */
float bk_penv_st(float x);         /* 0 .. 36 semitones */
float bk_penv_decay_s(float x);    /* 5 .. 400 ms */

/* One envelope, 1 -> 0. Retrigger ramps from the current value up to 1. */
typedef struct {
    float v;
    int attacking;
    float att_inc;           /* per (oversampled) sample */
    float dec_mul;
} bk_env;

typedef struct {
    float param[BP_COUNT];   /* BASE values: what the knobs set and state saves */
    float ovr[BP_COUNT];     /* motion playback override, NAN = none */

    /* voice */
    float ph1, ph2;          /* 0..1 */
    float tri1;              /* VCO1 triangle, integrated from nothing: derived */
    float note_hz;           /* latched at the hit: PITCH applies per trigger */
    int note_midi;
    float hit_penv_st;       /* P.ENV depth in semitones, latched at the hit */
    bk_env amp, fenv, penv, menv;
    float release_mul;       /* transport stop: fast fade, 1 = off */
    int releasing;
    uint32_t rng;

    /* ladder */
    float y[4], w[4];

    /* smoothed controls (one-pole) */
    float s_cut_oct, s_env_oct, s_fm, s_k, s_drive, s_vol, s_vco2, s_noise, s_tune;
    int smooth_primed;

    /* decimator + output */
    float hb_hist[BK_HB_TAPS];
    int hb_pos;
    float dc_x, dc_y;
    int hits;                /* diagnostics / tests */
} bk_engine;

void  bk_engine_init(bk_engine *e, uint32_t seed);
void  bk_engine_set_param(bk_engine *e, int idx, float value);   /* clamps */
float bk_engine_value(const bk_engine *e, int idx);              /* override or base */
void  bk_engine_set_override(bk_engine *e, int idx, float value);/* NAN clears; clamps */
/* A 1/16 hit: latches PITCH, retriggers the four envelopes. */
void  bk_engine_trigger(bk_engine *e);
/* Transport stopped: ~25 ms fade, then silence. Cancelled by the next hit. */
void  bk_engine_release(bk_engine *e);
void  bk_engine_render(bk_engine *e, float *out, int frames);     /* mono, 44.1k */

#endif
