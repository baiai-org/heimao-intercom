#pragma once

/* Float KISS FFT for the display. The fixed-point Speex build has its own
 * symbols and cannot retain the weak signals displayed by this analyser. */
#define FLOATING_POINT 1
#define kiss_fft_alloc spectrum_kiss_alloc
#define kiss_fft spectrum_kiss_fft
#define kiss_fft_stride spectrum_kiss_stride
#define kiss_fft_cleanup spectrum_kiss_cleanup
#define kiss_fft_next_fast_size spectrum_kiss_next_fast_size
#define kiss_fftr_alloc spectrum_kiss_real_alloc
#define kiss_fftr spectrum_kiss_real
#define kiss_fftr2 spectrum_kiss_real_packed
#define kiss_fftri spectrum_kiss_inverse
#define kiss_fftri2 spectrum_kiss_inverse_packed
#define kf_shuffle spectrum_kiss_shuffle
#define kf_work spectrum_kiss_work
#define kf_factor spectrum_kiss_factor

#include "../components/speexdsp/upstream/libspeexdsp/kiss_fftr.h"
