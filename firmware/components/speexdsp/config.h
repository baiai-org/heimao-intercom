/* Local configuration for the upstream SpeexDSP preprocessor subset. */
#pragma once
#define FIXED_POINT 1
#define USE_KISS_FFT 1
#define OS_SUPPORT_CUSTOM 1
#define ORL_SPEEX_NO_ECHO 1
#define EXPORT

/* Keep this copy of KISS FFT private to SpeexDSP, alongside Opus. */
#define kiss_fft_alloc orl_speex_kiss_fft_alloc
#define kiss_fft orl_speex_kiss_fft
#define kiss_fft_stride orl_speex_kiss_fft_stride
#define kiss_fft_cleanup orl_speex_kiss_fft_cleanup
#define kiss_fft_next_fast_size orl_speex_kiss_fft_next_fast_size
#define kiss_fftr_alloc orl_speex_kiss_fftr_alloc
#define kiss_fftr orl_speex_kiss_fftr
#define kiss_fftr2 orl_speex_kiss_fftr2
#define kiss_fftri orl_speex_kiss_fftri
#define kiss_fftri2 orl_speex_kiss_fftri2
