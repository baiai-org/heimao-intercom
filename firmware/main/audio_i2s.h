/*
 * I2S / ES8311 transport -- stage 2.
 *
 * Board wiring (see docs/PINMAP.md):
 *   I2S_MCLK  GPIO4  -> ES8311 pin 2
 *   I2S_BCLK  GPIO5  -> ES8311 pin 6
 *   I2S_LRCK  GPIO6  -> ES8311 pin 8
 *   I2S_DIN   GPIO7  -> ES8311 pin 9 (DSDIN, DAC data)  == ESP-IDF .dout
 *   I2S_DOUT  GPIO8  <- ES8311 pin 7 (ASDOUT, ADC data) == ESP-IDF .din
 *
 * THE NAMING TRAP: the board names these two nets from the CODEC's point of
 * view, which is the opposite of ESP-IDF's std-I2S field names.  Use the
 * PIN_I2S_TO_CODEC / PIN_I2S_FROM_CODEC aliases from board_pins.h and never the
 * raw names, or you get silence with no build error.
 *
 * The ESP32-S3 is the I2S master and drives MCLK.  A full-duplex (TX+RX) pair
 * is created so playback and capture share one clock domain.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 16 kHz.
 *
 * The 25 kHz channel spacing mode of the SA818 carries audio up to ~5 kHz, so
 * 8 kHz sampling (4 kHz Nyquist) cut it off and 12 kHz (6 kHz Nyquist) only just
 * covered it.  16 kHz gives 8 kHz of band: real headroom above the modulator's
 * limit, which is what makes the wideband setting sound open instead of tight.
 *
 * Cost, measured on the board: the Opus frame grows 480 -> 640 samples, the
 * bitrate 24 -> 32 kbit/s to keep the same 2 bits/sample, so the stream goes
 * 2.93 -> 3.91 KB/s (+33%) and the encoder ~38.7% -> ~51% of one core.
 */
#define AUDIO_SAMPLE_RATE_HZ   16000
/* One network packet carries one Opus frame: 20 ms at the 16 kHz stream rate. */
#define AUDIO_PKT_MS           20
#define AUDIO_PKT_SAMPLES      (AUDIO_SAMPLE_RATE_HZ * AUDIO_PKT_MS / 1000)
/* 256 fs = 3.072 MHz at 12 kHz, which is the ES8311's preferred MCLK ratio. */
#define AUDIO_MCLK_MULTIPLE    256

/*
 * Bring up the duplex I2S port: MCLK/BCLK/LRCK toggle and both directions are
 * enabled.  Safe to call repeatedly.
 */
esp_err_t audio_i2s_start_clock_only(void);

/* True once the I2S channels are enabled and the clocks are running. */
bool audio_i2s_clock_running(void);

/* Enable/disable the I2S pair (stops the clocks when disabled). */
esp_err_t audio_i2s_set_enabled(bool on);

/* Write `count` mono 16-bit frames to the codec's DAC (blocking). */
esp_err_t audio_i2s_write(const int16_t *samples, size_t count);

typedef struct {
    uint64_t requested_frames, written_frames, dma_frames, dma_callbacks;
    uint32_t calls, errors, short_writes, writer_changes, concurrent_calls;
    uint32_t send_queue_overflows, receive_queue_overflows;
    uint32_t receive_gap_max_us, gap_max_us, write_max_us;
    uint32_t receive_queue_stalls, receive_windows;
    int last_error;
    char writer[20];
    bool receive_active;
} audio_i2s_diag_t;

/* Measurement only. Queue-stall events are not inferred from PCM amplitude.
 * Counts exclude the first 100 ms of a receive window. */
void audio_i2s_diag_receive(bool active);
void audio_i2s_diag_get(audio_i2s_diag_t *out);

/* Read up to `count` mono 16-bit frames from the codec's ADC (blocking). */
esp_err_t audio_i2s_read(int16_t *samples, size_t count, size_t *read_out);

/*
 * Emit a sine tone for `ms` milliseconds and return.
 */
esp_err_t audio_i2s_play_tone(int freq_hz, int ms, int amplitude);

/*
 * Log-sweep from `f_start` to `f_stop` over `ms` milliseconds.
 * Phase-continuous, so there are no discontinuities along the way.
 */
esp_err_t audio_i2s_play_sweep(int f_start, int f_stop, int ms, int amplitude);

/* Push `ms` milliseconds of silence (keeps the clocks running). */
esp_err_t audio_i2s_play_silence(int ms);

/*
 * The full listening test: reference tone, low/mid/high tones, a 200 Hz -> 4 kHz
 * log sweep, and a codec-volume ladder.  Roughly 15 seconds.  Segments are
 * announced on the console as they start so the log can be matched to what is
 * heard, and every segment is faded in/out over 5 ms to avoid clicks.
 *
 * Requires the amplifier to be enabled by the caller (board_amp_enable(true)).
 */
esp_err_t audio_i2s_play_test_pattern(void);

/* Result of a capture-and-measure pass. */
typedef struct {
    int   samples;      /* frames actually looked at            */
    int   mean;         /* DC offset, 16-bit signed scale       */
    int   rms;          /* RMS amplitude                        */
    int   peak;         /* max |sample|                         */
    int   distinct;     /* how many different values were seen  */
    int   clipped;      /* samples at the int16 rails           */
    bool  alive;        /* distinct > 8 and rms > 4             */
} audio_stats_t;

/*
 * Capture `ms` milliseconds from the codec ADC and measure it.
 *
 * This is the objective test for the microphone path: a dead ADC link reads a
 * constant value (distinct == 1), while a live one shows noise/voice and a
 * nonzero RMS.
 */
esp_err_t audio_measure_mic(int ms, audio_stats_t *out);

#ifdef __cplusplus
}
#endif
