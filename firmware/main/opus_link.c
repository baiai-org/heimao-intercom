#include "opus_link.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "opus.h"

static const char *TAG = "opus_link";

struct opus_link_enc {
    OpusEncoder *st;
};

struct opus_link_dec {
    OpusDecoder *st;
};

opus_link_enc_t *opus_link_enc_new(void)
{
    opus_link_enc_t *e = calloc(1, sizeof(*e));
    if (!e) {
        return NULL;
    }
    int err = OPUS_OK;
    e->st = opus_encoder_create(OPUS_LINK_RATE_HZ, 1, OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK || !e->st) {
        ESP_LOGE(TAG, "opus_encoder_create: %s", opus_strerror(err));
        free(e);
        return NULL;
    }
    opus_encoder_ctl(e->st, OPUS_SET_BITRATE(OPUS_LINK_BITRATE));
    opus_encoder_ctl(e->st, OPUS_SET_COMPLEXITY(0));
    opus_encoder_ctl(e->st, OPUS_SET_VBR(0));            /* CBR: fixed frame size */
    opus_encoder_ctl(e->st, OPUS_SET_DTX(0));
    opus_encoder_ctl(e->st, OPUS_SET_INBAND_FEC(0));
    opus_encoder_ctl(e->st, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    ESP_LOGI(TAG, "Opus encoder: %d Hz mono, %d ms, CBR %d kbit/s, complexity 0",
             OPUS_LINK_RATE_HZ, OPUS_LINK_FRAME_MS, OPUS_LINK_BITRATE / 1000);
    return e;
}

void opus_link_enc_free(opus_link_enc_t *enc)
{
    if (!enc) {
        return;
    }
    if (enc->st) {
        opus_encoder_destroy(enc->st);
    }
    free(enc);
}

int opus_link_encode(opus_link_enc_t *enc, const int16_t *pcm,
                     uint8_t *out, size_t cap)
{
    if (!enc || !enc->st) {
        return -1;
    }
    const int n = opus_encode(enc->st, pcm, OPUS_LINK_SAMPLES, out, (opus_int32)cap);
    if (n < 0) {
        ESP_LOGW(TAG, "opus_encode: %s", opus_strerror(n));
        return -1;
    }
    return n;
}

opus_link_dec_t *opus_link_dec_new(void)
{
    opus_link_dec_t *d = calloc(1, sizeof(*d));
    if (!d) {
        return NULL;
    }
    int err = OPUS_OK;
    d->st = opus_decoder_create(OPUS_LINK_RATE_HZ, 1, &err);
    if (err != OPUS_OK || !d->st) {
        ESP_LOGE(TAG, "opus_decoder_create: %s", opus_strerror(err));
        free(d);
        return NULL;
    }
    return d;
}

void opus_link_dec_free(opus_link_dec_t *dec)
{
    if (!dec) {
        return;
    }
    if (dec->st) {
        opus_decoder_destroy(dec->st);
    }
    free(dec);
}

int opus_link_decode(opus_link_dec_t *dec, const uint8_t *data, size_t len,
                     int16_t *pcm)
{
    if (!dec || !dec->st) {
        return -1;
    }
    /* len == 0 (or a NULL pointer) means "this frame was lost": the decoder runs
     * its packet-loss concealment instead of dropping straight to silence. */
    const int n = opus_decode(dec->st, (len > 0) ? data : NULL, (opus_int32)len,
                              pcm, OPUS_LINK_SAMPLES, 0);
    if (n < 0) {
        return -1;
    }
    return n;
}

const char *opus_link_bench(char *out, size_t n)
{
    static int16_t pcm[OPUS_LINK_SAMPLES];
    static uint8_t pkt[OPUS_LINK_MAX_BYTES];
    static int16_t back[OPUS_LINK_SAMPLES];

    opus_link_enc_t *enc = opus_link_enc_new();
    opus_link_dec_t *dec = opus_link_dec_new();
    if (!enc || !dec) {
        snprintf(out, n, "opus init failed");
        opus_link_enc_free(enc);
        opus_link_dec_free(dec);
        return out;
    }

    /* 300 Hz + 1.2 kHz + noise: roughly speech-like for the encoder. */
    const int frames = 100;
    int bytes_max = 0;
    int64_t t_enc = 0, t_dec = 0;
    unsigned seed = 12345;
    for (int f = 0; f < frames; ++f) {
        for (int i = 0; i < OPUS_LINK_SAMPLES; ++i) {
            const double t = (double)(f * OPUS_LINK_SAMPLES + i) / OPUS_LINK_RATE_HZ;
            double v = 9000.0 * sin(2 * M_PI * 300.0 * t)
                     + 4000.0 * sin(2 * M_PI * 1200.0 * t);
            seed = seed * 1103515245u + 12345u;
            v += (double)((seed >> 16) & 0x3ff) - 512.0;
            pcm[i] = (int16_t)v;
        }
        const int64_t a = esp_timer_get_time();
        const int len = opus_link_encode(enc, pcm, pkt, sizeof(pkt));
        const int64_t b = esp_timer_get_time();
        if (len < 0) {
            snprintf(out, n, "encode failed");
            break;
        }
        if (len > bytes_max) {
            bytes_max = len;
        }
        const int64_t c = esp_timer_get_time();
        (void)opus_link_decode(dec, pkt, (size_t)len, back);
        t_enc += b - a;
        t_dec += esp_timer_get_time() - c;
    }

    const double per_enc_us = (double)t_enc / frames;
    const double per_dec_us = (double)t_dec / frames;
    const double frame_us = OPUS_LINK_FRAME_MS * 1000.0;
    snprintf(out, n,
             "opus %dHz %dms CBR%dk: enc %.0f us, dec %.0f us per frame, "
             "CPU enc %.1f%% dec %.1f%% (of one core), pkt max %d B, wire %.2f KB/s, "
             "heapInt %u K psram %u K",
             OPUS_LINK_RATE_HZ, OPUS_LINK_FRAME_MS, OPUS_LINK_BITRATE / 1000,
             per_enc_us, per_dec_us, per_enc_us * 100.0 / frame_us,
             per_dec_us * 100.0 / frame_us, bytes_max,
             (double)OPUS_LINK_BITRATE / 8.0 / 1024.0,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

    opus_link_enc_free(enc);
    opus_link_dec_free(dec);
    return out;
}

/* ---------------------------------------------------------------- benchmark */

/*
 * Opus runs its arithmetic on a 120 KB pseudostack (PSRAM) plus a few KB of real
 * task stack.  Calling it straight from an httpd handler (10 KB stack) panics
 * the board, so the benchmark -- and, in the real integration, the encoder --
 * gets its own task with a PSRAM-backed stack.
 */
static SemaphoreHandle_t s_bench_done;
static char s_bench_out[320];

static void bench_task(void *arg)
{
    (void)arg;
    opus_link_bench(s_bench_out, sizeof(s_bench_out));
    xSemaphoreGive(s_bench_done);
    vTaskDelete(NULL);
}

const char *opus_link_bench_task(char *out, size_t n, uint32_t timeout_ms)
{
    if (!s_bench_done) {
        s_bench_done = xSemaphoreCreateBinary();
    }
    if (!s_bench_done) {
        snprintf(out, n, "no semaphore");
        return out;
    }
    (void)xSemaphoreTake(s_bench_done, 0);
    s_bench_out[0] = '\0';
    TaskHandle_t th = NULL;
    if (xTaskCreateWithCaps(bench_task, "opusbench", 32768, NULL, 5, &th,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        snprintf(out, n, "could not start bench task");
        return out;
    }
    if (xSemaphoreTake(s_bench_done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        snprintf(out, n, "bench timeout (task may still be running)");
        return out;
    }
    snprintf(out, n, "%s", s_bench_out);
    return out;
}
