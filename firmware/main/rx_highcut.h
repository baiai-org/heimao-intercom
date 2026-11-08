/* Fixed receive-only 3.7 kHz high cut, before 32 kHz -> 16 kHz decimation.
 *
 * 12th-order elliptic low-pass in second-order sections. Design:
 * scipy.signal.ellipord(3400, 3700, 0.1, 84, fs=32000), then ellip(..., 'lowpass').
 * Pass band: 0..3400 Hz, nominal ripple <=0.1 dB. Transition: 3400..3700 Hz.
 * Stop band: >=3700 Hz, >=80 dB after float32 implementation checks.
 * Fixed linear filtering only; no level detector or automatic gain.
 */
#pragma once

#include <string.h>

#define RX_HIGHCUT_RATE_HZ 32000
#define RX_HIGHCUT_SECTIONS 6

typedef struct {
    float z[RX_HIGHCUT_SECTIONS][2];
} rx_highcut_t;

/* b0, b1, b2, a1, a2; a0 is one. Rounded explicitly to float32. */
static const float s_rx_highcut_sos[RX_HIGHCUT_SECTIONS][5] = {
    {0.0003178353945f, 0.0003640086798f, 0.0003178353945f, -1.579708219f, 0.6384068727f},
    {1.0f, -0.6858986616f, 1.0f, -1.570046544f, 0.7255891562f},
    {1.0f, -1.230491161f, 1.0f, -1.558754563f, 0.8320843577f},
    {1.0f, -1.409009099f, 1.0f, -1.55177784f, 0.9112851024f},
    {1.0f, -1.476668715f, 1.0f, -1.550867081f, 0.9592587352f},
    {1.0f, -1.500738978f, 1.0f, -1.557391286f, 0.9883534312f},
};

static inline void rx_highcut_reset(rx_highcut_t *f)
{
    memset(f, 0, sizeof(*f));
}

static inline float rx_highcut_run(rx_highcut_t *f, float x)
{
    for (int i = 0; i < RX_HIGHCUT_SECTIONS; ++i) {
        const float *c = s_rx_highcut_sos[i];
        const float y = c[0] * x + f->z[i][0];
        f->z[i][0] = c[1] * x - c[3] * y + f->z[i][1];
        f->z[i][1] = c[2] * x - c[4] * y;
        x = y;
    }
    return x;
}
