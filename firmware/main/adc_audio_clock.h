/* Fixed ADC sample clock for the 16 kHz radio-audio path. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define ADC_AUDIO_RATE_HZ 32000

typedef struct {
    uint32_t source_hz;
    uint32_t div_num;
    uint32_t div_a;
    uint32_t div_b;
    uint32_t interval;
    uint32_t configured_hz;
    bool exact;
} adc_audio_clock_t;

/* Read back the clock programmed during the most recent ADC start. */
void adc_audio_clock_read(adc_audio_clock_t *out);
