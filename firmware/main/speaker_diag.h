/* Explicit, volatile local-speaker diagnosis. No RF/network transmission. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "speaker_diag_core.h"

typedef enum {
    SPEAKER_DIAG_IDLE, SPEAKER_DIAG_ARMED, SPEAKER_DIAG_RECORD,
    SPEAKER_DIAG_REPLAY, SPEAKER_DIAG_SILENCE, SPEAKER_DIAG_TONE,
    SPEAKER_DIAG_DAC_MUTE, SPEAKER_DIAG_DRAIN, SPEAKER_DIAG_RESTORE
} speaker_diag_mode_t;

typedef struct {
    uint32_t session, receive_windows, test_runs;
    speaker_diag_mode_t mode;
    uint32_t captured, capacity, position, remaining_ms;
    uint32_t receive_block_max_us;
    int capture_speaker_step;
    bool codec_restore_pending;
    bool codec_mute_applied;
    uint8_t codec_original, codec_readback;
    int codec_error, codec_apply_error;
    uint32_t codec_restore_attempts;
    speaker_meter_t common, local, test;
    char reason[40];
} speaker_diag_stats_t;

/* voice control serializes commands with start/stop; record does not inhibit TX. */
esp_err_t speaker_diag_command(const char *op, unsigned ms);
bool speaker_diag_output_active(void);
bool speaker_diag_output_amp_on(void);
void speaker_diag_tick(void);
void speaker_diag_monitor_begin(void);
void speaker_diag_monitor_end(void);
void speaker_diag_observe(const int16_t *common, const int16_t *local,
                          size_t n, bool receive_open, uint32_t work_us);
/* Used only by the existing monitor writer. Paces cached output from I2S,
 * drains ADC separately, never calls the bridge/SDR taps. Includes drain zeros. */
bool speaker_diag_render(int16_t *pcm, size_t n);
void speaker_diag_render_written(const int16_t *pcm, size_t n, bool was_drain);
void speaker_diag_render_failed(bool was_drain);
void speaker_diag_get(speaker_diag_stats_t *out);
const char *speaker_diag_mode_name(speaker_diag_mode_t mode);
/* Immutable capture lease: blocks a new recording, never blocks audio. */
bool speaker_diag_capture_acquire(const int16_t **common, const int16_t **local,
                                  size_t *n);
void speaker_diag_capture_release(void);
