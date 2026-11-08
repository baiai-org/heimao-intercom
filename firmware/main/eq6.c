/*
 * Six-band voice EQ -- coefficient math and the per-sample runner.
 *
 * The coefficients are the Audio EQ Cookbook formulae by Robert
 * Bristow-Johnson, as standardised for the Web Audio API:
 *
 *   https://webaudio.github.io/Audio-EQ-Cookbook/audio-eq-cookbook.html
 *
 * (Espressif ships an equalizer in ESP-ADF and a bare biquad in esp-dsp, but
 * neither is a self-contained band module for a standalone app: ADF drags in
 * the whole audio framework, and esp-dsp has no coefficient design.  The
 * Cookbook is the community-standard formula set, so we use it with our own
 * tiny DF1 runner -- the same biquad structure voice.c already uses.)
 *
 * Three of the Cookbook's nine filter types are needed: lowShelf and highShelf
 * at the extremes (S = 0.5 softens the slope, so a -12 dB cut does not notch
 * the 150-300 Hz body of speech), and peakingEQ for the four middle bands.
 * Each section's f0/Q/S is a fixed constant -- only the gains are user data.
 */
#include "eq6.h"
#include "audio_i2s.h"   /* AUDIO_SAMPLE_RATE_HZ / AUDIO_PKT_SAMPLES */

#include <math.h>

#define EQ6_SR ((float)AUDIO_SAMPLE_RATE_HZ)

typedef enum {
    EQ6F_LOW_SHELF,
    EQ6F_PEAKING,
    EQ6F_HIGH_SHELF,
} eq6_filter_type_t;

typedef struct {
    eq6_filter_type_t type;
    float hz;
    float q_or_s;   /* Q for peaking, shelf slope S otherwise */
} eq6_band_def_t;

/* Order matches eq6.h / the panel / the API keys. */
static const eq6_band_def_t s_bands[EQ6_BANDS] = {
    { EQ6F_LOW_SHELF,  150.0f, 0.5f },
    { EQ6F_PEAKING,    400.0f, 1.0f },
    { EQ6F_PEAKING,    900.0f, 1.0f },
    { EQ6F_PEAKING,   2000.0f, 1.0f },
    { EQ6F_PEAKING,   3200.0f, 1.5f },
    { EQ6F_HIGH_SHELF, 3800.0f, 0.5f },
};

static inline float biquad_run(eq6_band_t *b, float x)
{
    const float y = b->b0 * x + b->b1 * b->x1 + b->b2 * b->x2
                    - b->a1 * b->y1 - b->a2 * b->y2;
    b->x2 = b->x1;  b->x1 = x;
    b->y2 = b->y1;  b->y1 = y;
    return y;
}

/* Cookbook common: prewarp-free digital form, a0 normalised to 1. */
static void coeff_low_shelf(eq6_band_t *q, float A, float hz, float S)
{
    const float w0 = 2.0f * 3.14159265358979f * hz / EQ6_SR;
    const float cw = cosf(w0), sw = sinf(w0);
    const float sa = sqrtf(A);
    const float alpha = sw / 2.0f
        * sqrtf((A + 1.0f / A) * (1.0f / S - 1.0f) + 2.0f);
    const float a0 = (A + 1.0f) + (A - 1.0f) * cw + 2.0f * sa * alpha;
    q->b0 = A * ((A + 1.0f) - (A - 1.0f) * cw + 2.0f * sa * alpha) / a0;
    q->b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cw) / a0;
    q->b2 = A * ((A + 1.0f) - (A - 1.0f) * cw - 2.0f * sa * alpha) / a0;
    q->a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * cw) / a0;
    q->a2 = ((A + 1.0f) + (A - 1.0f) * cw - 2.0f * sa * alpha) / a0;
}

static void coeff_peaking(eq6_band_t *q, float A, float hz, float Q)
{
    const float w0 = 2.0f * 3.14159265358979f * hz / EQ6_SR;
    const float cw = cosf(w0), sw = sinf(w0);
    const float alpha = sw / (2.0f * Q);
    const float a0 = 1.0f + alpha / A;
    q->b0 = (1.0f + alpha * A) / a0;
    q->b1 = -2.0f * cw / a0;
    q->b2 = (1.0f - alpha * A) / a0;
    q->a1 = -2.0f * cw / a0;
    q->a2 = (1.0f - alpha / A) / a0;
}

static void coeff_high_shelf(eq6_band_t *q, float A, float hz, float S)
{
    const float w0 = 2.0f * 3.14159265358979f * hz / EQ6_SR;
    const float cw = cosf(w0), sw = sinf(w0);
    const float sa = sqrtf(A);
    const float alpha = sw / 2.0f
        * sqrtf((A + 1.0f / A) * (1.0f / S - 1.0f) + 2.0f);
    const float a0 = (A + 1.0f) - (A - 1.0f) * cw + 2.0f * sa * alpha;
    q->b0 = A * ((A + 1.0f) + (A - 1.0f) * cw + 2.0f * sa * alpha) / a0;
    q->b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cw) / a0;
    q->b2 = A * ((A + 1.0f) + (A - 1.0f) * cw - 2.0f * sa * alpha) / a0;
    q->a1 = 2.0f * ((A - 1.0f) - (A + 1.0f) * cw) / a0;
    q->a2 = ((A + 1.0f) - (A - 1.0f) * cw - 2.0f * sa * alpha) / a0;
}

static void set_band(eq6_band_t *q, float A, const eq6_band_def_t *def)
{
    switch (def->type) {
    case EQ6F_LOW_SHELF:
        coeff_low_shelf(q, A, def->hz, def->q_or_s);
        break;
    case EQ6F_HIGH_SHELF:
        coeff_high_shelf(q, A, def->hz, def->q_or_s);
        break;
    case EQ6F_PEAKING:
        coeff_peaking(q, A, def->hz, def->q_or_s);
        break;
    }
}

static int clamp_db(int db)
{
    if (db < EQ6_MIN_DB) { return EQ6_MIN_DB; }
    if (db > EQ6_MAX_DB) { return EQ6_MAX_DB; }
    return db;
}

void eq6_set(eq6_t *e, const int db[EQ6_BANDS])
{
    int flat = 1;
    for (int i = 0; i < EQ6_BANDS; ++i) {
        e->db[i] = clamp_db(db[i]);
        if (e->db[i] != 0) {
            flat = 0;
        }
        /* A == 1 (0 dB) makes every Cookbook formula a unity wire. */
        const float A = powf(10.0f, e->db[i] / 40.0f);
        set_band(&e->band[i], A, &s_bands[i]);
    }
    e->flat = flat;
    eq6_reset_state(e);
}

void eq6_reset_state(eq6_t *e)
{
    for (int i = 0; i < EQ6_BANDS; ++i) {
        e->band[i].x1 = e->band[i].x2 = 0.0f;
        e->band[i].y1 = e->band[i].y2 = 0.0f;
    }
}

bool eq6_is_flat(const eq6_t *e)
{
    return e->flat;
}

void eq6_run(eq6_t *e, int16_t *buf, size_t n)
{
    if (e->flat) {
        return;
    }
    for (size_t i = 0; i < n; ++i) {
        float v = (float)buf[i];
        for (int b = 0; b < EQ6_BANDS; ++b) {
            v = biquad_run(&e->band[b], v);
        }
        if (v > 32767.0f)      { v = 32767.0f; }
        else if (v < -32768.0f) { v = -32768.0f; }
        buf[i] = (int16_t)v;
    }
}
