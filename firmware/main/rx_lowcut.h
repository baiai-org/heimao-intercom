/* Fixed receive-only low cut, after the SA818 AF_OUT ADC and before gain.
 *
 * 32 kHz, 13th-order elliptic high-pass in second-order sections. Design:
 * scipy.signal.ellipord(300, 270, 0.1, 84, fs=32000), rounded up to odd order
 * before ellip(..., 'highpass') to retain an exact DC zero.
 * Stop band: 0..270 Hz, >=80 dB after float32 coefficient/implementation checks.
 * Transition: 270..300 Hz. Pass band: >=300 Hz, nominal ripple <=0.1 dB.
 * This is a fixed linear filter; there is no detector or automatic gain.
 */
#pragma once

#include <string.h>

#define RX_LOWCUT_RATE_HZ 32000
#define RX_LOWCUT_SECTIONS 7

typedef struct {
    float z[RX_LOWCUT_SECTIONS][2];
} rx_lowcut_t;

/* b0, b1, b2, a1, a2; a0 is one. Rounded explicitly to float32. */
static const float s_rx_lowcut_sos[RX_LOWCUT_SECTIONS][5] = {
    {0.8270903826f, -0.8270903826f, 0.0f, -0.8374056816f, 0.0f},
    {1.0f, -1.999565482f, 1.0f, -1.860301733f, 0.8733974695f},
    {1.0f, -1.998645663f, 1.0f, -1.952458501f, 0.9589277506f},
    {1.0f, -1.997820735f, 1.0f, -1.979587317f, 0.9841102958f},
    {1.0f, -1.997296929f, 1.0f, -1.989485383f, 0.9933073521f},
    {1.0f, -1.99702394f, 1.0f, -1.993694901f, 0.9972377419f},
    {1.0f, -1.99691236f, 1.0f, -1.995803356f, 0.9992456436f},
};

static inline void rx_lowcut_reset(rx_lowcut_t *f)
{
    memset(f, 0, sizeof(*f));
}

static inline float rx_lowcut_run_section(rx_lowcut_t *f, float x, int i)
{
    const float *c = s_rx_lowcut_sos[i];
    const float y = c[0] * x + f->z[i][0];
    f->z[i][0] = c[1] * x - c[3] * y + f->z[i][1];
    f->z[i][1] = c[2] * x - c[4] * y;
    return y;
}

static inline float rx_lowcut_run(rx_lowcut_t *f, float x)
{
    for (int i = 0; i < RX_LOWCUT_SECTIONS; ++i) {
        x = rx_lowcut_run_section(f, x, i);
    }
    return x;
}
