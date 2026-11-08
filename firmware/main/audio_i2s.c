/*
 * I2S transport -- implementation.
 */
#include "audio_i2s.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/*
 * audio_i2s.c -> audio_codec.c is a deliberate one-way use, and only for the
 * listening test's volume ladder: stepping the codec's digital volume while a
 * steady tone plays is the direct check of es8311_voice_volume_set().  The
 * normal playback/capture paths do not touch the codec at all.
 */
#include "audio_codec.h"
#include "board_pins.h"

static const char *TAG = "audio";

static i2s_chan_handle_t s_tx;
static i2s_chan_handle_t s_rx;
static bool s_running;
static portMUX_TYPE s_diag_mux = portMUX_INITIALIZER_UNLOCKED;
static audio_i2s_diag_t s_diag;
static TaskHandle_t s_writer;
static unsigned s_writers;
static int64_t s_last_write_start, s_receive_start;
static StaticSemaphore_t s_write_lock_storage;
static SemaphoreHandle_t s_write_lock;

static bool tx_sent(i2s_chan_handle_t channel, i2s_event_data_t *event, void *ctx)
{
    (void)channel; (void)ctx;
    portENTER_CRITICAL_ISR(&s_diag_mux);
    ++s_diag.dma_callbacks;
    s_diag.dma_frames += event->size / (2 * sizeof(int16_t));
    portEXIT_CRITICAL_ISR(&s_diag_mux);
    return false;
}

static bool tx_queue_stall(i2s_chan_handle_t channel, i2s_event_data_t *event, void *ctx)
{
    (void)channel; (void)event; (void)ctx;
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL_ISR(&s_diag_mux);
    ++s_diag.send_queue_overflows;
    if (s_diag.receive_active && now - s_receive_start > 100000) {
        ++s_diag.receive_queue_stalls;
    }
    portEXIT_CRITICAL_ISR(&s_diag_mux);
    return false;
}

static bool rx_queue_stall(i2s_chan_handle_t channel, i2s_event_data_t *event, void *ctx)
{
    (void)channel; (void)event; (void)ctx;
    portENTER_CRITICAL_ISR(&s_diag_mux);
    ++s_diag.receive_queue_overflows;
    portEXIT_CRITICAL_ISR(&s_diag_mux);
    return false;
}

void audio_i2s_diag_receive(bool active)
{
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_diag_mux);
    if (active && !s_diag.receive_active) { s_receive_start = now; ++s_diag.receive_windows; }
    s_diag.receive_active = active;
    portEXIT_CRITICAL(&s_diag_mux);
}

void audio_i2s_diag_get(audio_i2s_diag_t *out)
{
    portENTER_CRITICAL(&s_diag_mux);
    *out = s_diag;
    portEXIT_CRITICAL(&s_diag_mux);
}

esp_err_t audio_i2s_start_clock_only(void)
{
    if (s_tx) {
        if (!s_running) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(i2s_channel_enable(s_tx));
            if (s_rx) {
                ESP_ERROR_CHECK_WITHOUT_ABORT(i2s_channel_enable(s_rx));
            }
            s_running = true;
        }
        return ESP_OK;
    }

    if (!s_write_lock) { s_write_lock = xSemaphoreCreateMutexStatic(&s_write_lock_storage); }

    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;

    /*
     * DMA depth -- this is the single biggest latency in the audio path.
     *
     * I2S_CHANNEL_DEFAULT_CONFIG sets dma_desc_num = 6 and dma_frame_num = 240.
     * dma_frame_num is in FRAMES, so at 8 kHz each descriptor holds 240/8000 =
     * 30 ms and the default chain is 6 x 30 = 180 ms of buffering.  Keeping that
     * buffer full -- which is exactly what a writer that keeps up does -- means
     * the audio played out is 180 ms behind the audio written, and that is heard
     * as "always one step behind" compared with an analogue handset.
     *
     * 4 x 64 frames = 32 ms is the same shape as the 16 ms processing block with
     * margin, which keeps latency down without risking underruns.
     */
    /*
     * 6 x 96 frames = 576 frames = 48 ms of ring per direction.
     *
     * This used to be 4 x 64 = 256 frames, which was 32 ms at 8 kHz (16 ms
     * block -> 16 ms of slack).  After the move to 12 kHz the same ring is only
     * 21 ms while a talk block is still 16 ms, leaving ~5 ms of slack: any WiFi
     * or httpd burst longer than that drops mic samples (RX) or starves the
     * codec DAC (TX), which is heard on air as a continuous crackle that sounds
     * like overload but is really lost frames.
     */
    /*
     * 6 descriptors, each holding enough frames for the ring to be 48 ms at
     * whatever rate the stream runs.  The frame count used to be a literal 96,
     * which was 48 ms at 12 kHz and only 36 ms at 16 kHz -- the ring silently
     * shrank every time the rate went up, eating the very margin the comment
     * above argues for.
     */
#define I2S_RING_MS     48
#define I2S_DESC_NUM    6
    chan_cfg.dma_desc_num  = I2S_DESC_NUM;
    chan_cfg.dma_frame_num = (AUDIO_SAMPLE_RATE_HZ * I2S_RING_MS / 1000) / I2S_DESC_NUM;

    /* Duplex: the RX side shares the TX clock domain. */
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx, &s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE_HZ),
        /*
         * STEREO slots, as in Espressif's own i2s_es8311 example.
         *
         * This was MONO, which is a real deviation from the reference: the
         * ES8311 is configured for the standard I2S frame of two 16-bit slots,
         * and the mono slot mode changes how the controller frames the data on
         * the wire.  The conversion between our mono samples and the codec's
         * stereo frames is done explicitly in audio_i2s_write()/read() below,
         * which is easier to reason about than relying on the slot mode.
         */
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                       I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = PIN_I2S_MCLK,
            .bclk = PIN_I2S_BCLK,
            .ws   = PIN_I2S_LRCK,
            /* The trap: the codec's DAC input is ESP-IDF's .dout ... */
            .dout = PIN_I2S_TO_CODEC,
            /* ... and the codec's ADC output is ESP-IDF's .din. */
            .din  = PIN_I2S_FROM_CODEC,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = AUDIO_MCLK_MULTIPLE;

    err = i2s_channel_init_std_mode(s_tx, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init TX: %s", esp_err_to_name(err));
        goto fail;
    }
    if (s_rx) {
        err = i2s_channel_init_std_mode(s_rx, &std_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "init RX: %s", esp_err_to_name(err));
            goto fail;
        }
    }

    const i2s_event_callbacks_t tx_events = {.on_sent = tx_sent, .on_send_q_ovf = tx_queue_stall};
    err = i2s_channel_register_event_callback(s_tx, &tx_events, NULL);
    if (err != ESP_OK) { goto fail; }
    if (s_rx) {
        const i2s_event_callbacks_t rx_events = {.on_recv_q_ovf = rx_queue_stall};
        err = i2s_channel_register_event_callback(s_rx, &rx_events, NULL);
        if (err != ESP_OK) { goto fail; }
    }
    err = i2s_channel_enable(s_tx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "enable TX: %s", esp_err_to_name(err));
        goto fail;
    }
    if (s_rx) {
        err = i2s_channel_enable(s_rx);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "enable RX: %s", esp_err_to_name(err));
            goto fail;
        }
    }
    s_running = true;

    ESP_LOGI(TAG, "I2S0 duplex master: %d Hz, MCLK=%d kHz on GPIO%d",
             AUDIO_SAMPLE_RATE_HZ,
             AUDIO_SAMPLE_RATE_HZ * AUDIO_MCLK_MULTIPLE / 1000, PIN_I2S_MCLK);
    ESP_LOGI(TAG, "  BCLK=GPIO%d LRCK=GPIO%d  dout(->codec)=GPIO%d  din(codec->)=GPIO%d",
             PIN_I2S_BCLK, PIN_I2S_LRCK, PIN_I2S_TO_CODEC, PIN_I2S_FROM_CODEC);
    return ESP_OK;

fail:
    if (s_tx) { i2s_del_channel(s_tx); s_tx = NULL; }
    if (s_rx) { i2s_del_channel(s_rx); s_rx = NULL; }
    return err;
}

bool audio_i2s_clock_running(void) { return s_running; }

esp_err_t audio_i2s_set_enabled(bool on)
{
    if (!s_tx) {
        return ESP_ERR_INVALID_STATE;
    }
    if (on == s_running) {
        return ESP_OK;
    }
    esp_err_t err = on ? i2s_channel_enable(s_tx) : i2s_channel_disable(s_tx);
    if (err != ESP_OK) {
        return err;
    }
    if (s_rx) {
        err = on ? i2s_channel_enable(s_rx) : i2s_channel_disable(s_rx);
        if (err != ESP_OK) {
            return err;
        }
    }
    s_running = on;
    return ESP_OK;
}

/*
 * Mono <-> stereo frame conversion.
 *
 * The ES8311 runs the standard I2S frame: two 16-bit slots per LRCK period.
 * Our signal path is mono, so every sample is duplicated into both slots on
 * transmit, and the left slot is taken on receive.  Chunked so no large buffer
 * is needed and the caller's `count` is honoured for any size.
 */
#define I2S_FRAME_CHUNK 256

esp_err_t audio_i2s_write(const int16_t *samples, size_t count)
{
    if (!s_tx || !s_running || !s_write_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!samples && count) { return ESP_ERR_INVALID_ARG; }
    const int64_t start = esp_timer_get_time();
    const TaskHandle_t writer = xTaskGetCurrentTaskHandle();
    char name[20];
    strlcpy(name, pcTaskGetName(writer), sizeof(name));
    portENTER_CRITICAL(&s_diag_mux);
    ++s_diag.calls;
    s_diag.requested_frames += count;
    if (s_writers++) { ++s_diag.concurrent_calls; }
    if (s_writer && s_writer != writer) { ++s_diag.writer_changes; }
    s_writer = writer;
    memcpy(s_diag.writer, name, sizeof(name));
    if (s_last_write_start) {
        const uint32_t gap = (uint32_t)(start - s_last_write_start);
        if (gap > s_diag.gap_max_us) { s_diag.gap_max_us = gap; }
        if (s_diag.receive_active && start - s_receive_start > 100000 &&
            gap > s_diag.receive_gap_max_us) { s_diag.receive_gap_max_us = gap; }
    }
    s_last_write_start = start;
    portEXIT_CRITICAL(&s_diag_mux);
    /* Serialize before filling the shared stereo conversion array. */
    xSemaphoreTake(s_write_lock, portMAX_DELAY);
    static int16_t frames[I2S_FRAME_CHUNK * 2];
    esp_err_t err = ESP_OK;

    while (count > 0) {
        const size_t n = (count > I2S_FRAME_CHUNK) ? I2S_FRAME_CHUNK : count;
        for (size_t i = 0; i < n; ++i) {
            frames[2 * i]     = samples[i];   /* left  */
            frames[2 * i + 1] = samples[i];   /* right */
        }
        size_t written = 0;
        err = i2s_channel_write(s_tx, frames, n * 2 * sizeof(int16_t),
                                &written, pdMS_TO_TICKS(1000));
        const size_t expected = n * 2 * sizeof(int16_t);
        portENTER_CRITICAL(&s_diag_mux);
        s_diag.written_frames += written / (2 * sizeof(int16_t));
        if (written != expected) { ++s_diag.short_writes; }
        portEXIT_CRITICAL(&s_diag_mux);
        if (err != ESP_OK || written != expected) {
            if (err == ESP_OK) { err = ESP_ERR_INVALID_SIZE; }
            break;
        }
        samples += n;
        count   -= n;
    }
    xSemaphoreGive(s_write_lock);
    const uint32_t elapsed = (uint32_t)(esp_timer_get_time() - start);
    portENTER_CRITICAL(&s_diag_mux);
    --s_writers;
    if (err != ESP_OK) { ++s_diag.errors; s_diag.last_error = err; }
    if (elapsed > s_diag.write_max_us) { s_diag.write_max_us = elapsed; }
    portEXIT_CRITICAL(&s_diag_mux);
    return err;
}

esp_err_t audio_i2s_read(int16_t *samples, size_t count, size_t *read_out)
{
    if (read_out) { *read_out = 0; }
    if (!s_rx || !s_running) {
        return ESP_ERR_INVALID_STATE;
    }
    static int16_t frames[I2S_FRAME_CHUNK * 2];
    size_t total = 0;
    esp_err_t err = ESP_OK;

    while (total < count) {
        const size_t n = ((count - total) > I2S_FRAME_CHUNK)
                         ? I2S_FRAME_CHUNK : (count - total);
        size_t got = 0;
        err = i2s_channel_read(s_rx, frames, n * 2 * sizeof(int16_t),
                               &got, pdMS_TO_TICKS(1000));
        if (err != ESP_OK) {
            break;
        }
        const size_t got_frames = got / (2 * sizeof(int16_t));
        for (size_t i = 0; i < got_frames; ++i) {
            samples[total + i] = frames[2 * i];   /* left slot */
        }
        total += got_frames;
        if (got_frames == 0) {
            break;
        }
    }
    if (read_out) {
        *read_out = total;
    }
    return err;
}

/*
 * Generate one segment.  With f_start == f_stop this is a plain tone; otherwise
 * it is a phase-continuous logarithmic sweep, which is what makes the sweep
 * useful for hearing frequency response rather than just "a noise".
 *
 * The 5 ms fades matter: without them every segment boundary clicks, and the
 * click is loud enough to be mistaken for an amplifier fault.
 */
static esp_err_t play_segment(int f_start, int f_stop, int ms, int amplitude,
                              const char *label)
{
    esp_err_t err = audio_i2s_start_clock_only();
    if (err != ESP_OK) {
        return err;
    }
    if (label) {
        printf("    %s\n", label);
    }
    if (amplitude <= 0)    { amplitude = 8000; }
    if (amplitude > 32000) { amplitude = 32000; }

    enum { CHUNK = 128 };
    int16_t buf[CHUNK];
    const int total = AUDIO_SAMPLE_RATE_HZ * ms / 1000;
    const int fade  = AUDIO_SAMPLE_RATE_HZ / 200;   /* 5 ms */
    const double fs = (double)AUDIO_SAMPLE_RATE_HZ;
    double phase = 0.0;
    int sent = 0;

    while (sent < total) {
        int n = total - sent;
        if (n > CHUNK) {
            n = CHUNK;
        }
        for (int i = 0; i < n; ++i) {
            const int idx = sent + i;
            double f;
            if (f_start == f_stop) {
                f = (double)f_start;
            } else {
                const double t = (double)idx / (double)total;
                f = (double)f_start * pow((double)f_stop / (double)f_start, t);
            }
            phase += 2.0 * M_PI * f / fs;
            if (phase > 2.0 * M_PI) {
                phase -= 2.0 * M_PI;
            }
            double amp = (double)amplitude;
            if (idx < fade) {
                amp *= (double)idx / (double)fade;
            }
            if (idx >= total - fade) {
                amp *= (double)(total - idx) / (double)fade;
            }
            buf[i] = (int16_t)(sin(phase) * amp);
        }
        err = audio_i2s_write(buf, (size_t)n);
        if (err != ESP_OK) {
            return err;
        }
        sent += n;
    }
    return ESP_OK;
}

esp_err_t audio_i2s_play_tone(int freq_hz, int ms, int amplitude)
{
    return play_segment(freq_hz, freq_hz, ms, amplitude, NULL);
}

esp_err_t audio_i2s_play_sweep(int f_start, int f_stop, int ms, int amplitude)
{
    return play_segment(f_start, f_stop, ms, amplitude, NULL);
}

esp_err_t audio_i2s_play_silence(int ms)
{
    esp_err_t err = audio_i2s_start_clock_only();
    if (err != ESP_OK) {
        return err;
    }
    enum { CHUNK = 256 };
    int16_t buf[CHUNK];
    memset(buf, 0, sizeof(buf));
    const int total = AUDIO_SAMPLE_RATE_HZ * ms / 1000;
    int sent = 0;
    while (sent < total) {
        int n = total - sent;
        if (n > CHUNK) {
            n = CHUNK;
        }
        err = audio_i2s_write(buf, (size_t)n);
        if (err != ESP_OK) {
            return err;
        }
        sent += n;
    }
    return ESP_OK;
}

esp_err_t audio_i2s_play_test_pattern(void)
{
    static const struct {
        int         f_start, f_stop, ms, amp;
        const char *label;
    } segs[] = {
        { 1000, 1000, 2000, 8000, "1/7  reference 1000 Hz, 2.0 s" },
        {  300,  300, 1000, 8000, "2/7  low  300 Hz, 1.0 s" },
        { 1000, 1000, 1000, 8000, "3/7  mid  1000 Hz, 1.0 s" },
        { 3000, 3000, 1000, 7000, "4/7  high 3000 Hz, 1.0 s" },
        {  200, 4000, 6000, 7000, "5/7  log sweep 200 -> 4000 Hz, 6.0 s" },
    };

    printf("    --- audio test pattern, ~15 s, listen on J4 ---\n");
    for (size_t i = 0; i < sizeof(segs) / sizeof(segs[0]); ++i) {
        esp_err_t err = play_segment(segs[i].f_start, segs[i].f_stop,
                                     segs[i].ms, segs[i].amp, segs[i].label);
        if (err != ESP_OK) {
            return err;
        }
    }

    /*
     * Codec volume ladder: a steady 1 kHz while the ES8311's digital volume is
     * stepped 30 -> 100.  This is the only part of the chain that software can
     * change, so hearing it step is the direct check of es8311_voice_volume_set.
     */
    printf("    6/7  codec volume ladder 30 -> 100 on a steady 1 kHz, 3.0 s\n");
    static const int steps[] = { 30, 50, 70, 90, 100 };
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); ++i) {
        int applied = -1;
        if (audio_codec_set_volume(steps[i]) == ESP_OK) {
            (void)applied;
        }
        esp_err_t err = play_segment(1000, 1000, 600, 8000, NULL);
        if (err != ESP_OK) {
            return err;
        }
    }

    printf("    7/7  silence, 0.5 s\n");
    esp_err_t err = audio_i2s_play_silence(500);
    if (err != ESP_OK) {
        return err;
    }

    /* Leave the codec at the nominal volume rather than the top of the ladder. */
    audio_codec_set_volume(70);
    printf("    --- pattern finished, codec volume back to 70 ---\n");
    return ESP_OK;
}

esp_err_t audio_measure_mic(int ms, audio_stats_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    esp_err_t err = audio_i2s_start_clock_only();
    if (err != ESP_OK) {
        return err;
    }
    if (!s_rx) {
        return ESP_ERR_INVALID_STATE;
    }

    const int want = AUDIO_SAMPLE_RATE_HZ * ms / 1000;
    enum { CHUNK = 256 };
    int16_t buf[CHUNK];

    int64_t sum = 0;
    int64_t sumsq = 0;
    int n = 0;
    int first = 0;
    bool have_first = false;

    /*
     * 8 KB bitmap over the unsigned 16-bit range, used to count how many
     * distinct sample values arrived.  It lives on the HEAP on purpose: as a
     * stack array it overflowed the 5 KB console task stack and panicked the
     * chip with "A stack overflow in task console has been detected".
     */
    uint8_t *seen = (uint8_t *)calloc(1, 8192);
    if (!seen) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t result = ESP_OK;
    while (n < want) {
        size_t got = 0;
        int need = want - n;
        if (need > CHUNK) { need = CHUNK; }
        err = audio_i2s_read(buf, (size_t)need, &got);
        if (err != ESP_OK) {
            result = err;
            goto done;
        }
        if (got == 0) {
            break;
        }
        for (size_t i = 0; i < got; ++i) {
            const int v = buf[i];
            if (!have_first) { first = v; have_first = true; }
            sum += v;
            sumsq += (int64_t)v * (int64_t)v;
            const int a = abs(v);
            if (a > out->peak) { out->peak = a; }
            if (v >= 32767 || v <= -32768) { ++out->clipped; }
            seen[((uint16_t)v) >> 3] = 1;
        }
        n += (int)got;
    }

    out->samples = n;
    if (n > 0) {
        out->mean = (int)(sum / n);
        out->rms = (int)sqrt((double)(sumsq / n));
    }
    (void)first;
    int distinct = 0;
    for (size_t i = 0; i < 8192; ++i) {
        if (seen[i]) { ++distinct; }
    }
    out->distinct = distinct * 8;   /* lower bound on distinct values */
    /*
     * The meaningful "is the ADC link alive" test is whether the data VARIES.
     * A dead or floating link reads one constant value.  RMS alone is a bad
     * gate: a silent room legitimately gives RMS 4 while still being real
     * audio, so the RMS only feeds the qualitative label below, not the verdict.
     */
    out->alive = (out->distinct > 8);

done:
    free(seen);
    return result;
}
