#include "speaker_diag.h"

#include <string.h>
#include "audio_codec.h"
#include "audio_i2s.h"
#include "board_hw.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ptt.h"
#include "voice.h"

#define CAPACITY (AUDIO_SAMPLE_RATE_HZ * 5)
#define DRAIN_SAMPLES (AUDIO_SAMPLE_RATE_HZ * 80 / 1000)

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static speaker_diag_stats_t s;
static int16_t *s_common, *s_local;
static size_t s_limit, s_drain_left;
static int64_t s_deadline_us;
static unsigned s_downloads;
static bool s_rx_was_open;
static bool s_capture_writing;
static int64_t s_last_audio_write_us;

static bool output_mode(speaker_diag_mode_t mode)
{
    return mode >= SPEAKER_DIAG_REPLAY;
}

static void finish_locked(const char *reason)
{
    s.mode = SPEAKER_DIAG_IDLE;
    s_deadline_us = 0;
    strlcpy(s.reason, reason, sizeof(s.reason));
}

static void drain_locked(const char *reason)
{
    s.mode = SPEAKER_DIAG_DRAIN;
    s_drain_left = DRAIN_SAMPLES;
    s_deadline_us = esp_timer_get_time() + 250000;
    strlcpy(s.reason, reason, sizeof(s.reason));
}

/* Called only by the monitor, outside any spinlock. A failed restoration
 * retains the PTT reservation and retries; it must never mute the next TX. */
static void complete_output(const char *reason)
{
    portENTER_CRITICAL(&s_mux);
    s.mode = SPEAKER_DIAG_RESTORE;
    const bool pending = s.codec_restore_pending;
    portEXIT_CRITICAL(&s_mux);
    uint8_t readback = 0;
    const esp_err_t err = pending ? audio_codec_diag_mute_end(&readback) : ESP_OK;
    portENTER_CRITICAL(&s_mux);
    if (pending) {
        ++s.codec_restore_attempts;
        s.codec_error = err;
        if (err == ESP_OK) { s.codec_readback = readback; s.codec_restore_pending = false; }
    }
    if (err == ESP_OK) {
        finish_locked(reason);
    } else {
        s_deadline_us = esp_timer_get_time() + 100000;
        strlcpy(s.reason, "codec restore failed; TX inhibited", sizeof(s.reason));
    }
    portEXIT_CRITICAL(&s_mux);
    board_amp_enable(false);
    if (err == ESP_OK) { ptt_local_audio_end(); }
}

esp_err_t speaker_diag_command(const char *op, unsigned ms)
{
    if (!op) { return ESP_ERR_INVALID_ARG; }
    const bool arm = !strcmp(op, "arm");
    const bool stop = !strcmp(op, "stop");
    speaker_diag_mode_t want = SPEAKER_DIAG_IDLE;
    if (!strcmp(op, "replay")) { want = SPEAKER_DIAG_REPLAY; }
    if (!strcmp(op, "silence")) { want = SPEAKER_DIAG_SILENCE; }
    if (!strcmp(op, "tone")) { want = SPEAKER_DIAG_TONE; }
    if (!strcmp(op, "dacmute")) { want = SPEAKER_DIAG_DAC_MUTE; }
    if (!arm && !stop && want == SPEAKER_DIAG_IDLE) { return ESP_ERR_INVALID_ARG; }
    if (ms < 250 || ms > 5000) { return ESP_ERR_INVALID_ARG; }

    if (stop) {
        portENTER_CRITICAL(&s_mux);
        if (output_mode(s.mode) && s.mode != SPEAKER_DIAG_DRAIN && s.mode != SPEAKER_DIAG_RESTORE) {
            drain_locked("stopped; draining");
        } else if (!output_mode(s.mode)) {
            finish_locked("stopped");
        }
        portEXIT_CRITICAL(&s_mux);
        return ESP_OK;
    }

    portENTER_CRITICAL(&s_mux);
    const bool busy = s.mode != SPEAKER_DIAG_IDLE || s_capture_writing || (arm && s_downloads != 0);
    const bool recorded = s.captured != 0;
    portEXIT_CRITICAL(&s_mux);
    if (busy) { return ESP_ERR_INVALID_STATE; }
    if (want == SPEAKER_DIAG_REPLAY && !recorded) { return ESP_ERR_NOT_FOUND; }

    if (arm && !s_common) {
        int16_t *common = heap_caps_malloc(CAPACITY * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        int16_t *local = heap_caps_malloc(CAPACITY * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (!common || !local) {
            heap_caps_free(common); heap_caps_free(local);
            return ESP_ERR_NO_MEM;
        }
        s_common = common; s_local = local;
    }
    if (!arm && !ptt_local_audio_begin()) { return ESP_ERR_INVALID_STATE; }
    uint8_t original = 0;
    if (want == SPEAKER_DIAG_DAC_MUTE) {
        const esp_err_t err = audio_codec_diag_mute_begin(&original);
        if (err != ESP_OK) { ptt_local_audio_end(); return err; }
    }

    const int speaker_step = voice_get_speaker_level();
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_mux);
    /* A WAV download can start while capture memory is being allocated. */
    if (arm && (s_downloads || s_capture_writing)) {
        portEXIT_CRITICAL(&s_mux);
        return ESP_ERR_INVALID_STATE;
    }
    ++s.session;
    s.position = 0;
    s_limit = (size_t)ms * AUDIO_SAMPLE_RATE_HZ / 1000;
    if (arm) {
        s.mode = SPEAKER_DIAG_ARMED;
        s.captured = 0;
        s.capacity = (uint32_t)s_limit;
        s.capture_speaker_step = speaker_step;
        s_deadline_us = now + 60000000; /* wait at most 60 s for module COS */
        strlcpy(s.reason, "waiting for module squelch", sizeof(s.reason));
    } else {
        s.mode = want;
        s.codec_restore_pending = want == SPEAKER_DIAG_DAC_MUTE;
        s.codec_original = s.codec_readback = original;
        s.codec_error = s.codec_apply_error = 0;
        s.codec_restore_attempts = 0;
        s.codec_mute_applied = false;
        s_last_audio_write_us = now;
        ++s.test_runs;
        memset(&s.test, 0, sizeof(s.test));
        if (want == SPEAKER_DIAG_REPLAY && s_limit > s.captured) {
            s_limit = s.captured;
        }
        s_deadline_us = now + (int64_t)ms * 1000 + 250000;
        strlcpy(s.reason, "local only; TX inhibited", sizeof(s.reason));
    }
    portEXIT_CRITICAL(&s_mux);
    return ESP_OK;
}

bool speaker_diag_output_active(void)
{
    portENTER_CRITICAL(&s_mux);
    const bool active = output_mode(s.mode);
    portEXIT_CRITICAL(&s_mux);
    return active;
}

bool speaker_diag_output_amp_on(void)
{
    portENTER_CRITICAL(&s_mux);
    const bool on = output_mode(s.mode) && s.mode != SPEAKER_DIAG_RESTORE;
    portEXIT_CRITICAL(&s_mux);
    return on;
}

void speaker_diag_tick(void)
{
    const int64_t now = esp_timer_get_time();
    bool release = false;
    portENTER_CRITICAL(&s_mux);
    if (s.mode != SPEAKER_DIAG_IDLE && s_deadline_us && now >= s_deadline_us) {
        if (s.mode == SPEAKER_DIAG_ARMED || s.mode == SPEAKER_DIAG_RECORD) {
            finish_locked(s.mode == SPEAKER_DIAG_ARMED ? "no carrier within 60 s" : "record timed out");
        } else if (s.mode == SPEAKER_DIAG_DRAIN) {
            /* A blocked/partial last audio write can finish after stop was
             * requested. Time this fallback from its completion as well. */
            if (now - s_last_audio_write_us >= 80000) { release = true; }
            else { s_deadline_us = s_last_audio_write_us + 80000; }
        } else if (s.mode == SPEAKER_DIAG_RESTORE) {
            release = true;
        } else {
            drain_locked("time limit; draining");
        }
    }
    portEXIT_CRITICAL(&s_mux);
    if (release) { complete_output("time limit; codec restored"); }
}

void speaker_diag_monitor_begin(void)
{
    portENTER_CRITICAL(&s_mux);
    s_rx_was_open = false;
    portEXIT_CRITICAL(&s_mux);
}

void speaker_diag_monitor_end(void)
{
    /* No further writer from this monitor can submit test PCM. Keep the
     * reservation through the complete 48 ms hardware ring, clocks running. */
    if (ptt_local_audio_active()) {
        board_amp_enable(false);
        vTaskDelay(pdMS_TO_TICKS(80));
        do {
            complete_output("monitor stopped; codec restored");
            if (ptt_local_audio_active()) { vTaskDelay(pdMS_TO_TICKS(100)); }
        } while (ptt_local_audio_active());
    }
    portENTER_CRITICAL(&s_mux);
    finish_locked("monitor stopped");
    s_rx_was_open = false;
    portEXIT_CRITICAL(&s_mux);
}

void speaker_diag_observe(const int16_t *common, const int16_t *local,
                          size_t n, bool receive_open, uint32_t work_us)
{
    speaker_meter_t cm = {0}, lm = {0};
    if (receive_open) {
        speaker_meter_add(&cm, common, n);
        speaker_meter_add(&lm, local, n);
    }
    const int64_t now = esp_timer_get_time();
    size_t offset = 0, take = 0;
    portENTER_CRITICAL(&s_mux);
    if (receive_open) {
        if (!s_rx_was_open) { ++s.receive_windows; }
        s.common.samples += cm.samples; s.common.squares += cm.squares;
        s.common.rail_samples += cm.rail_samples;
        if (cm.peak > s.common.peak) { s.common.peak = cm.peak; }
        s.local.samples += lm.samples; s.local.squares += lm.squares;
        s.local.rail_samples += lm.rail_samples;
        if (lm.peak > s.local.peak) { s.local.peak = lm.peak; }
        if (work_us > s.receive_block_max_us) { s.receive_block_max_us = work_us; }
    }
    s_rx_was_open = receive_open;
    if ((s.mode == SPEAKER_DIAG_ARMED || s.mode == SPEAKER_DIAG_RECORD) &&
        now >= s_deadline_us) {
        finish_locked(s.mode == SPEAKER_DIAG_ARMED ? "no carrier within 60 s" : "recorded");
    }
    if (s.mode == SPEAKER_DIAG_ARMED && receive_open) {
        s.mode = SPEAKER_DIAG_RECORD;
        s_deadline_us = now + (int64_t)s_limit * 1000000 / AUDIO_SAMPLE_RATE_HZ + 1000000;
        strlcpy(s.reason, "recording receive PCM", sizeof(s.reason));
    }
    if (s.mode == SPEAKER_DIAG_RECORD) {
        if (!receive_open) {
            finish_locked("carrier ended / route changed");
        } else {
            offset = s.captured;
            take = s_limit - offset;
            if (take > n) { take = n; }
            s_capture_writing = take != 0;
        }
    }
    portEXIT_CRITICAL(&s_mux);

    /* Only the monitor writes these arrays. Commands are serialized with
     * voice lifecycle; downloads cannot lease unfinished recording memory. */
    if (take) {
        memcpy(s_common + offset, common, take * sizeof(int16_t));
        memcpy(s_local + offset, local, take * sizeof(int16_t));
        portENTER_CRITICAL(&s_mux);
        s.captured += take;
        s_capture_writing = false;
        if (s.mode == SPEAKER_DIAG_RECORD && s.captured >= s_limit) {
            finish_locked("recorded");
        }
        portEXIT_CRITICAL(&s_mux);
    }
}

bool speaker_diag_render(int16_t *pcm, size_t n)
{
    portENTER_CRITICAL(&s_mux);
    const bool apply_mute = s.mode == SPEAKER_DIAG_DAC_MUTE && !s.codec_mute_applied;
    portEXIT_CRITICAL(&s_mux);
    if (apply_mute) {
        uint8_t readback = 0;
        const esp_err_t err = audio_codec_diag_mute_set(true, &readback);
        portENTER_CRITICAL(&s_mux);
        s.codec_error = s.codec_apply_error = err;
        if (err == ESP_OK) { s.codec_mute_applied = true; s.codec_readback = readback; }
        else { drain_locked("codec mute failed; draining"); }
        portEXIT_CRITICAL(&s_mux);
    }
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_mux);
    speaker_diag_mode_t mode = s.mode;
    if (!output_mode(mode)) { portEXIT_CRITICAL(&s_mux); return false; }
    if (mode != SPEAKER_DIAG_DRAIN && mode != SPEAKER_DIAG_RESTORE && now >= s_deadline_us) {
        drain_locked("time limit; draining"); mode = SPEAKER_DIAG_DRAIN;
    }
    const size_t position = s.position;
    size_t take = mode == SPEAKER_DIAG_DRAIN || mode == SPEAKER_DIAG_RESTORE ? 0 : s_limit - position;
    if (take > n) { take = n; }
    const size_t total = s_limit;
    /* Finishing the drain is committed only AFTER its final zero block has
     * been submitted, so no TX claim can race pending diagnostic samples. */
    portEXIT_CRITICAL(&s_mux);
    memset(pcm, 0, n * sizeof(int16_t));
    if (mode == SPEAKER_DIAG_REPLAY) {
        memcpy(pcm, s_common + position, take * sizeof(int16_t));
    } else if (mode == SPEAKER_DIAG_TONE) {
        for (size_t i = 0; i < take; ++i) { pcm[i] = speaker_test_tone(position + i, total); }
    }
    portENTER_CRITICAL(&s_mux);
    if (s.mode == mode && mode != SPEAKER_DIAG_DRAIN && mode != SPEAKER_DIAG_RESTORE) {
        s.position += take;
        if (s.position >= s_limit) { drain_locked("test complete; draining"); }
    }
    /* Drain accounting is in written(), keeping the last test block out. */
    portEXIT_CRITICAL(&s_mux);
    return mode == SPEAKER_DIAG_DRAIN;
}

void speaker_diag_render_written(const int16_t *pcm, size_t n, bool was_drain)
{
    speaker_meter_t meter = {0};
    speaker_meter_add(&meter, pcm, n);
    bool release = false;
    portENTER_CRITICAL(&s_mux);
    if (!was_drain && s.mode != SPEAKER_DIAG_RESTORE) { s_last_audio_write_us = esp_timer_get_time(); }
    s.test.samples += meter.samples; s.test.squares += meter.squares;
    s.test.rail_samples += meter.rail_samples;
    if (meter.peak > s.test.peak) { s.test.peak = meter.peak; }
    if (was_drain && s.mode == SPEAKER_DIAG_DRAIN) {
        if (s_drain_left > n) { s_drain_left -= n; }
        else { release = true; }
    }
    portEXIT_CRITICAL(&s_mux);
    if (release) { complete_output("test complete; codec restored"); }
}

void speaker_diag_render_failed(bool was_drain)
{
    portENTER_CRITICAL(&s_mux);
    if (!was_drain && s.mode != SPEAKER_DIAG_RESTORE) { s_last_audio_write_us = esp_timer_get_time(); }
    if (output_mode(s.mode) && s.mode != SPEAKER_DIAG_DRAIN && s.mode != SPEAKER_DIAG_RESTORE) {
        drain_locked("I2S write failed; draining");
    }
    portEXIT_CRITICAL(&s_mux);
}

void speaker_diag_get(speaker_diag_stats_t *out)
{
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_mux);
    *out = s;
    out->remaining_ms = s_deadline_us > now ? (uint32_t)((s_deadline_us - now) / 1000) : 0;
    portEXIT_CRITICAL(&s_mux);
}

const char *speaker_diag_mode_name(speaker_diag_mode_t mode)
{
    static const char *const names[] = {"idle", "armed", "record", "replay", "silence", "tone", "dacmute", "drain", "restore"};
    return (unsigned)mode < sizeof(names) / sizeof(names[0]) ? names[mode] : "unknown";
}

bool speaker_diag_capture_acquire(const int16_t **common, const int16_t **local, size_t *n)
{
    portENTER_CRITICAL(&s_mux);
    const bool ok = s.captured && !s_capture_writing && s.mode != SPEAKER_DIAG_RECORD && s.mode != SPEAKER_DIAG_ARMED;
    if (ok) { ++s_downloads; *common = s_common; *local = s_local; *n = s.captured; }
    portEXIT_CRITICAL(&s_mux);
    return ok;
}

void speaker_diag_capture_release(void)
{
    portENTER_CRITICAL(&s_mux);
    if (s_downloads) { --s_downloads; }
    portEXIT_CRITICAL(&s_mux);
}
