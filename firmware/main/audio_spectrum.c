#include "audio_spectrum.h"
#include "audio_i2s.h"

#include <stdatomic.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

enum { PCM_CAPACITY = 24000, JOIN_SAMPLES = 2560, LEASE_MS = 1000, ACTIVE_MS = 180 };
_Static_assert(AUDIO_SAMPLE_RATE_HZ == 16000 && SPECTRUM_PATH_COUNT < 32,
               "Update the PCM protocol when the audio format changes");

typedef struct {
    uint32_t path_time[SPECTRUM_PATH_COUNT];
    audio_spectrum_path_t path, selected;
    uint32_t boot, generation, next, stamp;
    unsigned head, count;
    int16_t pcm[PCM_CAPACITY];
} spectrum_state_t;

static StaticSemaphore_t s_mutex_storage;
static SemaphoreHandle_t s_mutex;
static spectrum_state_t *s_state;
static atomic_uint s_lease_until, s_missed, s_contention;

/* A reader pins the physical span only while it copies into its HTTP
 * response. Route changes keep the write index moving around the ring. */
typedef struct {
    unsigned index, count;
    atomic_bool active;
} spectrum_pin_t;
static spectrum_pin_t s_pins[2];

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

static void reset_stream(spectrum_state_t *s)
{
    if (++s->generation == 0) { ++s->generation; }
    s->count = s->next = 0;
}

static bool pinned(unsigned head, unsigned count)
{
    for (unsigned i = 0; i < sizeof(s_pins) / sizeof(s_pins[0]); ++i) {
        const spectrum_pin_t *pin = &s_pins[i];
        if (!atomic_load_explicit(&pin->active, memory_order_acquire)) { continue; }
        const unsigned ahead = (pin->index + PCM_CAPACITY - head) % PCM_CAPACITY;
        const unsigned within = (head + PCM_CAPACITY - pin->index) % PCM_CAPACITY;
        if (ahead < count || within < pin->count) { return true; }
    }
    return false;
}

static void use_path(spectrum_state_t *s, audio_spectrum_path_t path)
{
    if (s->path != path) {
        s->path = path;
        reset_stream(s);
    }
}

void audio_spectrum_tap(audio_spectrum_path_t path, const int16_t *pcm, size_t samples)
{
    if (path <= SPECTRUM_AUTO || path >= SPECTRUM_PATH_COUNT || !pcm || !samples) { return; }
    const uint32_t now = now_ms();
    if (!leased(now)) { return; }
    const unsigned bit = 1u << path;
    if (xSemaphoreTake(s_mutex, 0) != pdTRUE) {
        atomic_fetch_or_explicit(&s_missed, bit, memory_order_relaxed);
        atomic_fetch_add_explicit(&s_contention, 1, memory_order_relaxed);
        return;
    }
    spectrum_state_t *s = s_state;
    const uint32_t gap = now - s->path_time[path];
    s->path_time[path] = now ? now : 1;
    if (s->selected == path || (!path_active(s, s->selected, now) &&
        (!path_active(s, s->path, now) || priority(path) > priority(s->path)))) {
        use_path(s, path);
    }
    const bool missed = atomic_fetch_and_explicit(&s_missed, ~bit, memory_order_relaxed) & bit;
    if (s->path == path) {
        if (samples > PCM_CAPACITY) {
            pcm += samples - PCM_CAPACITY;
            samples = PCM_CAPACITY;
            reset_stream(s);
        }
        if (s->count && (gap > ACTIVE_MS || missed || samples > UINT32_MAX - s->next)) {
            reset_stream(s);
        }
        if (pinned(s->head, samples)) {
            atomic_fetch_or_explicit(&s_missed, bit, memory_order_relaxed);
            atomic_fetch_add_explicit(&s_contention, 1, memory_order_relaxed);
            xSemaphoreGive(s_mutex);
            return;
        }
        const size_t room = PCM_CAPACITY - s->head;
        const size_t first = samples < room ? samples : room;
        memcpy(s->pcm + s->head, pcm, first * sizeof(*pcm));
        memcpy(s->pcm, pcm + first, (samples - first) * sizeof(*pcm));
        s->head = (s->head + samples) % PCM_CAPACITY;
        const unsigned filled = s->count + samples;
        s->count = filled < PCM_CAPACITY ? filled : PCM_CAPACITY;
        s->next += samples;
        s->stamp = now;
    }
    xSemaphoreGive(s_mutex);
}

void audio_spectrum_init(void)
{
    s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_storage);
}

static bool start_locked(void)
{
    if (s_state) { return true; }
    s_state = heap_caps_calloc(1, sizeof(*s_state), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_state) { return false; }
    s_state->boot = esp_random();
    s_state->generation = 1;
    return true;
}

static void le32(uint8_t *out, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) { out[i] = (uint8_t)(value >> (i * 8)); }
}

size_t audio_spectrum_read(uint32_t after, uint32_t generation, bool select, audio_spectrum_path_t path,
                           uint8_t out[SPECTRUM_REPLY_BYTES])
{
    if (!s_mutex || xSemaphoreTake(s_mutex, pdMS_TO_TICKS(5)) != pdTRUE) { return 0; }
    if (!start_locked()) { xSemaphoreGive(s_mutex); return 0; }
    const uint32_t now = now_ms();
    spectrum_state_t *s = s_state;
    if (!leased(now)) {
        use_path(s, SPECTRUM_AUTO);
        memset(s->path_time, 0, sizeof(s->path_time));
        s->selected = SPECTRUM_AUTO;
    }
    atomic_store_explicit(&s_lease_until, now + LEASE_MS, memory_order_relaxed);
    if (select && path < SPECTRUM_PATH_COUNT) {
        s->selected = path;
        if (path_active(s, path, now)) { use_path(s, path); }
    }

    uint32_t active = 0;
    for (audio_spectrum_path_t p = 1; p < SPECTRUM_PATH_COUNT; ++p) {
        if (path_active(s, p, now)) { active |= 1u << p; }
    }
    /* Allow a delayed reader to drain the end of a short transmission. */
    if (!active && s->path && now - s->stamp > LEASE_MS) { use_path(s, SPECTRUM_AUTO); }

    const uint32_t oldest = s->next - s->count;
    uint32_t first = after;
    if (generation != s->generation || after > s->next || after < oldest) {
        first = s->count > JOIN_SAMPLES ? s->next - JOIN_SAMPLES : oldest;
    }
    unsigned count = s->next - first;
    if (count > SPECTRUM_MAX_SAMPLES) { count = SPECTRUM_MAX_SAMPLES; }
    const unsigned index = (s->head + PCM_CAPACITY - (s->next - first)) % PCM_CAPACITY;
    spectrum_pin_t *pin = NULL;
    if (count) {
        for (unsigned i = 0; i < sizeof(s_pins) / sizeof(s_pins[0]); ++i) {
            if (!atomic_load_explicit(&s_pins[i].active, memory_order_acquire)) {
                pin = &s_pins[i];
                pin->index = index;
                pin->count = count;
                atomic_store_explicit(&pin->active, true, memory_order_release);
                break;
            }
        }
        if (!pin) { xSemaphoreGive(s_mutex); return 0; }
    }
    const uint32_t header[] = { s->boot, s->generation, active, now, s->stamp, first,
        s->next, count, s->path, atomic_load_explicit(&s_contention, memory_order_relaxed) };
    xSemaphoreGive(s_mutex);

    memcpy(out, "APC\x01", 4);
    _Static_assert(4 + sizeof(header) == SPECTRUM_HEADER_BYTES, "PCM header size");
    for (unsigned i = 0; i < sizeof(header) / sizeof(header[0]); ++i) { le32(out + 4 * (i + 1), header[i]); }
    if (pin) {
        const size_t run = count < PCM_CAPACITY - index ? count : PCM_CAPACITY - index;
        /* ESP32-S3 stores the signed 16-bit samples in wire byte order. */
        memcpy(out + SPECTRUM_HEADER_BYTES, s->pcm + index, run * sizeof(int16_t));
        memcpy(out + SPECTRUM_HEADER_BYTES + run * sizeof(int16_t), s->pcm, (count - run) * sizeof(int16_t));
        atomic_store_explicit(&pin->active, false, memory_order_release);
    }
    return SPECTRUM_HEADER_BYTES + count * sizeof(int16_t);
}
