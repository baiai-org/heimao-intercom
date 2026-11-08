#pragma once

#include <stdbool.h>

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
/* Strict decimal MHz input on the documented 12.5 kHz grid. */
bool sa818_freq_parse(const char *text, float *mhz);
