#include "sa818_freq.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>

const char *sa818_band_name(sa818_band_t band)
{
    return band == SA818_BAND_UHF ? "UHF" :
           band == SA818_BAND_VHF ? "VHF" : "UNSET";
}

float sa818_band_min(sa818_band_t band)
{
    return band == SA818_BAND_UHF ? 400.0f :
           band == SA818_BAND_VHF ? 134.0f : 0.0f;
}

float sa818_band_max(sa818_band_t band)
{
    return band == SA818_BAND_UHF ? 480.0f :
           band == SA818_BAND_VHF ? 174.0f : 0.0f;
}

static bool freq_valid(sa818_band_t band, float mhz)
{
    if ((band != SA818_BAND_UHF && band != SA818_BAND_VHF) ||
        !isfinite(mhz) || mhz < sa818_band_min(band) || mhz > sa818_band_max(band)) {
        return false;
    }
    /* Existing files store float MHz: allow its < 16 Hz quantization error. */
    const double hz = (double)mhz * 1000000.0;
    return fabs(hz - round(hz / 12500.0) * 12500.0) < 20.0;
}

bool sa818_freq_pair_valid(sa818_band_t band, float tx, float rx)
{
    return freq_valid(band, tx) && freq_valid(band, rx);
}

bool sa818_freq_parse(const char *text, float *mhz)
{
    if (!text || !mhz) { return false; }
    while (isspace((unsigned char)*text)) { ++text; }
    /* Reject hex floats, exponents, NaN and partial numbers from API input. */
    const char *p = text;
    bool digit = false, dot = false;
    while ((*p >= '0' && *p <= '9') || *p == '.') {
        if (*p == '.') {
            if (dot) { return false; }
            dot = true;
        } else {
            digit = true;
        }
        ++p;
    }
    if (!digit) { return false; }
    while (isspace((unsigned char)*p)) { ++p; }
    if (*p) { return false; }
    const double value = strtod(text, NULL);
    if (!isfinite(value) || value <= 0.0 || value > 480.0) { return false; }
    const double hz = value * 1000000.0;
    if (fabs(hz - round(hz / 12500.0) * 12500.0) > 0.001) { return false; }
    const float f = (float)value;
    if (!sa818_freq_pair_valid(SA818_BAND_UHF, f, f) &&
        !sa818_freq_pair_valid(SA818_BAND_VHF, f, f)) { return false; }
    *mhz = f;
    return true;
}
