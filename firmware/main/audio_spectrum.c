#include "audio_spectrum.h"
#include "audio_i2s.h"
#include "spectrum_kiss.h"

#include <math.h>
#include <stdatomic.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

enum {
    FFT_SIZE = SPECTRUM_FFT_SIZE,
    HOP_SAMPLES = AUDIO_SAMPLE_RATE_HZ / 8,
    HISTORY_ROWS = 4,
    LEASE_MS = 2000,
    ACTIVE_MS = 350,
};
_Static_assert(AUDIO_SAMPLE_RATE_HZ == 16000 && SPECTRUM_PATH_COUNT < 32,
               "Update the spectrum protocol when the audio format changes");

typedef struct {
    uint32_t sequence, stamp;
    uint8_t bins[SPECTRUM_BINS];
} spectrum_row_t;

typedef struct {
    uint32_t path_time[SPECTRUM_PATH_COUNT];
    audio_spectrum_path_t path, previous_path, selected;
    uint32_t generation, input_revision, sequence, stamp;
    unsigned input_head, input_count, input_new;
    unsigned row_head, row_count;
    int16_t input[FFT_SIZE];
    int16_t snapshot[FFT_SIZE];
    float hann[FFT_SIZE], time_domain[FFT_SIZE];
    kiss_fft_cpx frequency_domain[FFT_SIZE / 2 + 1];
    spectrum_row_t rows[HISTORY_ROWS];
    kiss_fftr_cfg fft;
    void *fft_memory;
    size_t fft_bytes;
} spectrum_state_t;

static StaticSemaphore_t s_mutex_storage;
static SemaphoreHandle_t s_mutex;
static spectrum_state_t *s_state;
static atomic_uint s_lease_until;
static atomic_uint s_missed;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool leased(uint32_t now)
{
    const uint32_t until = atomic_load_explicit(&s_lease_until, memory_order_relaxed);
    return until && (int32_t)(until - now) > 0;
}

static bool path_active(const spectrum_state_t *s, audio_spectrum_path_t path, uint32_t now)
{
    return path && s->path_time[path] && now - s->path_time[path] <= ACTIVE_MS;
}

static unsigned priority(audio_spectrum_path_t path)
{
    switch (path) {
    case SPECTRUM_MIC_RF:
    case SPECTRUM_REMOTE_RF:
        return 6;
    case SPECTRUM_MIC_P2P:
    case SPECTRUM_MIC_MQTT:
    case SPECTRUM_REMOTE_P2P:
    case SPECTRUM_REMOTE_MQTT:
        return 5;
    case SPECTRUM_P2P_RF:
    case SPECTRUM_MQTT_RF:
        return 4;
    case SPECTRUM_P2P_SPEAKER:
    case SPECTRUM_MQTT_SPEAKER:
    case SPECTRUM_P2P_MONITOR:
    case SPECTRUM_MQTT_MONITOR:
        return 3;
    case SPECTRUM_RF_SPEAKER:
    case SPECTRUM_RF_P2P:
    case SPECTRUM_RF_MQTT:
        return 2;
    default:
        return path ? 1 : 0;
    }
}

static void reset_input(spectrum_state_t *s)
{
    s->input_head = s->input_count = s->input_new = 0;
    ++s->input_revision;
}

static void use_path(spectrum_state_t *s, audio_spectrum_path_t path)
{
    if (s->path == path) { return; }
    s->path = path;
    reset_input(s);
    if (path && path != s->previous_path) {
        s->previous_path = path;
        ++s->generation;
        s->row_count = s->row_head = 0;
    }
}

static void choose_path(spectrum_state_t *s, uint32_t now)
{
    if (path_active(s, s->selected, now)) {
        use_path(s, s->selected);
        return;
    }
    s->selected = SPECTRUM_AUTO;
    audio_spectrum_path_t path = path_active(s, s->path, now) ? s->path : SPECTRUM_AUTO;
    for (audio_spectrum_path_t p = 1; p < SPECTRUM_PATH_COUNT; ++p) {
        if (path_active(s, p, now) && priority(p) > priority(path)) { path = p; }
    }
    use_path(s, path);
}

void audio_spectrum_tap(audio_spectrum_path_t path, const int16_t *pcm, size_t samples)
{
    if (!atomic_load_explicit(&s_lease_until, memory_order_relaxed) ||
        path <= SPECTRUM_AUTO || path >= SPECTRUM_PATH_COUNT || !pcm || !samples) { return; }
    const uint32_t now = now_ms();
    if (!leased(now)) { return; }
    const unsigned bit = 1u << path;
    if (xSemaphoreTake(s_mutex, 0) != pdTRUE) {
        atomic_fetch_or_explicit(&s_missed, bit, memory_order_relaxed);
        return;
    }
    spectrum_state_t *s = s_state;
    const uint32_t gap = now - s->path_time[path];
    s->path_time[path] = now ? now : 1;
    if (s->selected == path || !path_active(s, s->path, now) ||
        (!path_active(s, s->selected, now) && priority(path) > priority(s->path))) {
        use_path(s, path);
    }
    if (s->path == path) {
        const bool missed = atomic_fetch_and_explicit(&s_missed, ~bit, memory_order_relaxed) & bit;
        if (gap > 160 || missed) { reset_input(s); }
        if (samples > FFT_SIZE) { pcm += samples - FFT_SIZE; samples = FFT_SIZE; }
        const unsigned room = FFT_SIZE - s->input_head;
        const unsigned first = samples < room ? (unsigned)samples : room;
        memcpy(s->input + s->input_head, pcm, first * sizeof(*pcm));
        memcpy(s->input, pcm + first, (samples - first) * sizeof(*pcm));
        s->input_head = (s->input_head + samples) % FFT_SIZE;
        const unsigned filled = s->input_count + samples;
        s->input_count = filled < FFT_SIZE ? filled : FFT_SIZE;
        const unsigned added = s->input_new + samples;
        s->input_new = added < FFT_SIZE ? added : FFT_SIZE;
        s->stamp = now;
    }
    xSemaphoreGive(s_mutex);
}

static void analyse(spectrum_state_t *s, uint8_t bins[SPECTRUM_BINS])
{
    for (unsigned i = 0; i < FFT_SIZE; ++i) {
        s->time_domain[i] = (float)s->snapshot[i] * s->hann[i];
    }
    kiss_fftr(s->fft, s->time_domain, s->frequency_domain);
    for (unsigned i = 0; i < SPECTRUM_BINS; ++i) {
        const unsigned bin = i + SPECTRUM_FIRST_BIN;
        const kiss_fft_cpx v = s->frequency_domain[bin];
        /* Hann coherent gain and signed PCM full scale. A real transform's
         * Nyquist bin has no negative-frequency partner to double. */
        const float scale = (bin == FFT_SIZE / 2 ? 1.0f : 2.0f) /
            (32768.0f * (FFT_SIZE - 1) * 0.5f);
        const float power = (v.r * v.r + v.i * v.i) * scale * scale;
        const float db = power > 0.0f ? 10.0f * log10f(power) : -120.0f;
        const float intensity = (db + 78.0f) * (255.0f / 62.0f);
        bins[i] = intensity <= 0 ? 0 : intensity >= 255 ? 255 : (uint8_t)(intensity + 0.5f);
    }
}

static void spectrum_worker(void *arg)
{
    spectrum_state_t *s = arg;
    uint8_t bins[SPECTRUM_BINS];
    s->fft = kiss_fftr_alloc(FFT_SIZE, 0, s->fft_memory, &s->fft_bytes);
    for (unsigned i = 0; i < FFT_SIZE; ++i) {
        s->hann[i] = 0.5f - 0.5f * cosf(6.28318530718f * i / (FFT_SIZE - 1));
    }
    for (;;) {
        const uint32_t now = now_ms();
        vTaskDelay(pdMS_TO_TICKS(leased(now) ? 20 : 200));
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        if (!leased(now_ms())) {
            atomic_store_explicit(&s_lease_until, 0, memory_order_relaxed);
            use_path(s, SPECTRUM_AUTO);
            s->selected = SPECTRUM_AUTO;
            xSemaphoreGive(s_mutex);
            continue;
        }
        choose_path(s, now_ms());
        if (!s->fft || s->input_count < FFT_SIZE || s->input_new < HOP_SAMPLES) {
            xSemaphoreGive(s_mutex);
            continue;
        }
        const unsigned head = s->input_head;
        memcpy(s->snapshot, s->input + head, (FFT_SIZE - head) * sizeof(s->input[0]));
        memcpy(s->snapshot + FFT_SIZE - head, s->input, head * sizeof(s->input[0]));
        /* Keep any block overshoot so the long-run spacing is 2000 samples. */
        s->input_new -= HOP_SAMPLES;
        const uint32_t revision = s->input_revision;
        const uint32_t stamp = s->stamp;
        xSemaphoreGive(s_mutex);

        analyse(s, bins);

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        if (s->input_revision == revision && leased(now_ms())) {
            spectrum_row_t *row = &s->rows[s->row_head];
            row->sequence = ++s->sequence;
            row->stamp = stamp;
            memcpy(row->bins, bins, sizeof(bins));
            s->row_head = (s->row_head + 1) % HISTORY_ROWS;
            if (s->row_count < HISTORY_ROWS) { ++s->row_count; }
        }
        xSemaphoreGive(s_mutex);
    }
}

void audio_spectrum_init(void)
{
    s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_storage);
}

static bool start_locked(void)
{
    if (s_state) { return true; }
    spectrum_state_t *s = heap_caps_calloc(1, sizeof(*s), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s) { return false; }
    kiss_fftr_alloc(FFT_SIZE, 0, NULL, &s->fft_bytes);
    s->fft_memory = heap_caps_malloc(s->fft_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s->fft_memory) {
        s->generation = esp_random();
        if (xTaskCreatePinnedToCoreWithCaps(spectrum_worker, "audio_spectrum", 5120, s, 1,
                                          NULL, 0, MALLOC_CAP_SPIRAM) == pdPASS) {
            s_state = s;
            return true;
        }
    }
    heap_caps_free(s->fft_memory);
    heap_caps_free(s);
    return false;
}

static void le32(uint8_t *out, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) { out[i] = (uint8_t)(value >> (i * 8)); }
}

size_t audio_spectrum_read(uint32_t after, bool select, audio_spectrum_path_t path,
                           uint8_t out[SPECTRUM_REPLY_BYTES])
{
    if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(40)) != pdTRUE) { return 0; }
    if (!start_locked()) { xSemaphoreGive(s_mutex); return 0; }
    const uint32_t now = now_ms();
    atomic_store_explicit(&s_lease_until, now + LEASE_MS, memory_order_relaxed);
    spectrum_state_t *s = s_state;
    if (select && path < SPECTRUM_PATH_COUNT) { s->selected = path; }
    choose_path(s, now);
    uint32_t active = 0;
    for (audio_spectrum_path_t p = 1; p < SPECTRUM_PATH_COUNT; ++p) {
        if (path_active(s, p, now)) { active |= 1u << p; }
    }
    memcpy(out, "ASP\x01", 4);
    le32(out + 4, s->generation);
    le32(out + 8, active);
    le32(out + 12, now);
    out[16] = (uint8_t)s->path;
    out[18] = (uint8_t)(SPECTRUM_BINS & 255);
    out[19] = (uint8_t)(SPECTRUM_BINS >> 8);
    size_t len = SPECTRUM_HEADER_BYTES;
    unsigned rows = 0;
    const unsigned limit = s->row_count < 2 ? s->row_count : 2;
    for (unsigned i = limit; i > 0; --i) {
        const unsigned index = (s->row_head + HISTORY_ROWS - i) % HISTORY_ROWS;
        const spectrum_row_t *row = &s->rows[index];
        if (after && (int32_t)(row->sequence - after) <= 0) { continue; }
        le32(out + len, row->sequence);
        le32(out + len + 4, row->stamp);
        memcpy(out + len + 8, row->bins, SPECTRUM_BINS);
        len += SPECTRUM_ROW_BYTES;
        ++rows;
    }
    out[17] = (uint8_t)rows;
    xSemaphoreGive(s_mutex);
    return len;
}
