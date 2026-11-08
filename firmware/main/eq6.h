/*
 * Six-band voice EQ -- OpenRadioLink-v0.3.
 *
 * One module, two instances (speaker path and microphone path), global by
 * design: whatever the operator sets here applies to every session of that
 * direction, persisted in NVS.
 *
 * Band plan, fixed (it is a voice radio, not a music deck).  The extremes are
 * gentle shelves (S = 0.5), the four middle bands are 1/3-octave-spaced
 * peaking sections across the speech range:
 *
 *   #   type        f0        Q/S     role
 *   0   low-shelf   150 Hz    S 0.5   depth / rumble
 *   1   peaking     400 Hz    Q 1.0   warmth, boxiness
 *   2   peaking     900 Hz    Q 1.0   voice body
 *   3   peaking     2 kHz     Q 1.0   clarity
 *   4   peaking     3.2 kHz   Q 1.5   presence (Q raised: at the 8 kHz sample
 *                                     rate a 3.2 kHz section is only 1.25x
 *                                     Nyquist, and Q 1.0 at full +12 dB puts
 *                                     the second pole outside the unit circle)
 *   5   high-shelf  3.8 kHz   S 0.5   air / sibilance
 *
 * Coefficients are the Audio EQ Cookbook formulae (Robert Bristow-Johnson,
 * the set the Web Audio API standardised).  Properties that matter for the
 * panel:
 *
 *   * at 0 dB every section is an exact wire (unity gain, flat phase), so
 *     "all zeros" is truly no processing -- and the run() cost drops to zero;
 *   * a boost of N dB followed by a cut of N dB at the same band cancels to
 *     unity, which is what makes the sliders round-trip intuitively.
 *
 * Both directions run at 8 kHz (the codec's native rate on this board); the
 * S3's FPU makes six biquads per sample negligible.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EQ6_MIN_DB   (-12)
#define EQ6_MAX_DB   12
#define EQ6_BANDS    6            /* 150, 400, 900, 2k, 3.2k, 3.8k */

typedef struct {
    float b0, b1, b2, a1, a2; /* a0 normalised to 1 */
    float x1, x2, y1, y2;
} eq6_band_t;

typedef struct {
    int db[EQ6_BANDS];           /* the values currently in effect   */
    eq6_band_t band[EQ6_BANDS];  /* coefficients + filter state      */
    bool flat;                   /* all bands 0 dB -> run() is a no-op */
} eq6_t;

/*
 * Set the six band gains (each clamped to EQ6_MIN_DB..EQ6_MAX_DB), recompute
 * the coefficients and reset the filter state so a parameter change cannot
 * ring.  Safe to call from the HTTP task while the audio task runs: the
 * coefficient store is plain float state and a torn read can only produce one
 * sample of the old/new mix at worst (the state reset is the real
 * discontinuity, and it is the same either way).
 */
void eq6_set(eq6_t *e, const int db[EQ6_BANDS]);

/* Clear the filter state only (e.g. when a route restarts). */
void eq6_reset_state(eq6_t *e);

/* True while all six bands are 0 dB. */
bool eq6_is_flat(const eq6_t *e);

/*
 * Apply the chain in place.  Clamps to the int16 range: with several boosted
 * bands a sample can legitimately overshoot full scale, and an int16 wrap
 * would read as a huge opposite-phase spike.
 */
void eq6_run(eq6_t *e, int16_t *buf, size_t n);

#ifdef __cplusplus
}
#endif
