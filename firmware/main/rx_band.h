/* Fixed 270 Hz / 3.7 kHz receive band, entirely before decimation and gain. */
#pragma once

#include "rx_lowcut.h"
#include "rx_highcut.h"

typedef struct {
    rx_lowcut_t lowcut;
    rx_highcut_t highcut;
} rx_band_t;

static inline void rx_band_reset(rx_band_t *f)
{
    rx_lowcut_reset(&f->lowcut);
    rx_highcut_reset(&f->highcut);
}

static inline float rx_band_run(rx_band_t *f, float x)
{
    /* The low cut's first (first-order) section removes ADC bias first.
     * Put the high cut before the remaining high-Q low-cut sections: a large
     * out-of-band high-frequency input otherwise excites float32 roundoff near
     * the low-cut edge. Reordering the same fixed sections preserves the ideal
     * frequency response, section count and gain; there is no level detector.
     */
    x = rx_lowcut_run_section(&f->lowcut, x, 0);
    x = rx_highcut_run(&f->highcut, x);
    for (int i = 1; i < RX_LOWCUT_SECTIONS; ++i) {
        x = rx_lowcut_run_section(&f->lowcut, x, i);
    }
    return x;
}
