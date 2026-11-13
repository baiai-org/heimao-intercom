#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Software setting resolution; this is not a measured RF/PLL specification. */
#define SA818_FREQ_GRID_HZ 500

typedef enum {
    SA818_BAND_UNSET = 0,
    SA818_BAND_UHF = 1,
    SA818_BAND_VHF = 2,
} sa818_band_t;

const char *sa818_band_name(sa818_band_t band);
float sa818_band_min(sa818_band_t band);
float sa818_band_max(sa818_band_t band);
/* Both frequencies must belong to the same physical module's band. */
bool sa818_freq_pair_valid(sa818_band_t band, float tx, float rx);
/* Strict decimal MHz input; 500 Hz is the common grid of 1/2.5/5/12.5 kHz. */
bool sa818_freq_parse(const char *text, float *mhz);
/* Recover canonical four-decimal MHz from legacy float storage. */
bool sa818_freq_format(float mhz, char *out, size_t out_len);
