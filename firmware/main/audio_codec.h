/*
 * ES8311 codec control -- OpenRadioLink-v0.3.
 *
 * The register sequence comes from the official Espressif component
 * (components/es8311, Apache-2.0, v1.0.0~1) rather than a home-made table: the
 * ES8311 datasheet does not publish a register map, so anything hand-rolled
 * would be guesswork.  See components/es8311/PATCH.md for the one local change
 * (legacy -> new I2C driver binding).
 *
 * Board facts that the configuration depends on:
 *   - the S3 is the I2S master and drives MCLK on GPIO4 (2.048 MHz at 8 kHz)
 *   - the microphone is the ANALOG MEMS part MK1 (not a digital PDM mic)
 *   - the DAC output is single-ended into the LM4871 (DAC_N is terminated)
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* I2C device at 0x18 + es8311_init + sensible voice/mic defaults.
 * Safe to call repeatedly; later calls are no-ops. */
esp_err_t audio_codec_init(void);

/* True once es8311_init() completed. */
bool audio_codec_ready(void);

/* Volume 0..100.  Returns the value the codec reports back after the write,
 * which doubles as proof that an I2C write+read round trip really happened. */
esp_err_t audio_codec_set_volume(int volume);
esp_err_t audio_codec_get_volume(int *volume);

/*
 * Microphone capture gain (ES8311 REG16): index 0..7 -> 0/6/12/18/24/30/36/42 dB.
 * And the ADC volume (REG17), written as 0xC8 by the vendor's own init.
 */
esp_err_t audio_codec_set_mic_gain(int gain_index);
int       audio_codec_get_mic_gain(int *gain_db);
esp_err_t audio_codec_set_adc_volume(int volume);
int       audio_codec_get_adc_volume(void);

esp_err_t audio_codec_mute(bool mute);

/* Volatile DAC test: preserve every original REG31 bit. Other mute writers
 * are refused until the original byte has been restored and read back. */
esp_err_t audio_codec_diag_mute_begin(uint8_t *original);
esp_err_t audio_codec_diag_mute_set(bool mute, uint8_t *readback);
esp_err_t audio_codec_diag_mute_end(uint8_t *readback);

#define AUDIO_CODEC_DIAG_REG_COUNT 27
typedef struct {
    uint8_t address[AUDIO_CODEC_DIAG_REG_COUNT];
    uint8_t value[AUDIO_CODEC_DIAG_REG_COUNT];
    unsigned count;
    int error;
    bool clock_format_match;
} audio_codec_diag_t;
/* Read-only; stops at the first bus error. Refused during the DAC mute test. */
esp_err_t audio_codec_diag_read(audio_codec_diag_t *out);

/* Print the codec's register file (es8311_register_dump). */
void audio_codec_dump(void);

#ifdef __cplusplus
}
#endif
