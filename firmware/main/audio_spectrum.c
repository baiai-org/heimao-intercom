#include "audio_spectrum.h"
#include "audio_i2s.h"

#include <stdatomic.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

enum { PCM_CAPACITY = 24000, JOIN_SAMPLES = 2560, LEASE_MS = 2400, ACTIVE_MS = 180,
       SEGMENT_CAPACITY = 32, REPLY_OVERRUN = 1, REPLY_GAP = 2 };
_Static_assert(AUDIO_SAMPLE_RATE_HZ == 16000 && SPECTRUM_PATH_COUNT < 32,
               "Update the PCM protocol when the audio format changes");

typedef struct {
    uint32_t first, end, stamp;
} spectrum_segment_t;

typedef struct {
    uint32_t path_time[SPECTRUM_PATH_COUNT];
    audio_spectrum_path_t path, selected;
    uint32_t boot, generation, next, stamp, dropped, pauses, renewals;
    unsigned head, segment_head, segment_count;
    spectrum_segment_t segments[SEGMENT_CAPACITY];
    int16_t pcm[PCM_CAPACITY];
} spectrum_state_t;

static StaticSemaphore_t s_mutex_storage;
static SemaphoreHandle_t s_mutex;
static spectrum_state_t *s_state;
static atomic_uint s_lease_until, s_missed[SPECTRUM_PATH_COUNT], s_contention;

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
    s->segment_head = s->segment_count = s->next = 0;
}

/* Gaps occupy logical time in the ring without writing fabricated samples.
 * Each reply covers at most one uninterrupted segment. */
static spectrum_segment_t *append_segment(spectrum_state_t *s, uint32_t gap)
{
    if (gap || !s->segment_count) {
        s->next += gap;
        s->head = (s->head + gap % PCM_CAPACITY) % PCM_CAPACITY;
        s->segments[s->segment_head] = (spectrum_segment_t){.first = s->next};
        s->segment_head = (s->segment_head + 1) % SEGMENT_CAPACITY;
        if (s->segment_count < SEGMENT_CAPACITY) { ++s->segment_count; }
    }
    return &s->segments[(s->segment_head + SEGMENT_CAPACITY - 1) % SEGMENT_CAPACITY];
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
    if (xSemaphoreTake(s_mutex, 0) != pdTRUE) {
        atomic_fetch_add_explicit(&s_missed[path], samples, memory_order_relaxed);
        atomic_fetch_add_explicit(&s_contention, 1, memory_order_relaxed);
        return;
    }
    spectrum_state_t *s = s_state;
    s->path_time[path] = now ? now : 1;
    if (s->selected == path || (!path_active(s, s->selected, now) &&
        (!path_active(s, s->path, now) || priority(path) > priority(s->path)))) {
        use_path(s, path);
    }
    const uint32_t missed = atomic_exchange_explicit(&s_missed[path], 0, memory_order_relaxed);
    if (s->path == path) {
        uint64_t gap = s->segment_count ? missed : 0;
        const uint32_t interval = now - s->stamp;
        const bool pause = s->segment_count && interval > ACTIVE_MS;
        if (pause) {
            const uint64_t elapsed = (uint64_t)interval * (AUDIO_SAMPLE_RATE_HZ / 1000);
            if (elapsed > samples && elapsed - samples > gap) { gap = elapsed - samples; }
        }
        uint32_t trimmed = 0;
        if (samples > PCM_CAPACITY) {
            trimmed = samples - PCM_CAPACITY;
            pcm += trimmed;
            samples = PCM_CAPACITY;
            gap += trimmed;
        }
        if ((uint64_t)s->next + gap + samples > UINT32_MAX) {
            reset_stream(s);
            gap = 0;
        }
        const unsigned next_head = gap ? (s->head + gap % PCM_CAPACITY) % PCM_CAPACITY : s->head;
        if (pinned(next_head, samples)) {
            atomic_fetch_add_explicit(&s_missed[path], missed + trimmed + samples, memory_order_relaxed);
            atomic_fetch_add_explicit(&s_contention, 1, memory_order_relaxed);
            xSemaphoreGive(s_mutex);
            return;
        }
        s->dropped += missed + trimmed;
        if (pause) { ++s->pauses; }
        spectrum_segment_t *segment = append_segment(s, (uint32_t)gap);
        const size_t room = PCM_CAPACITY - s->head;
        const size_t first = samples < room ? samples : room;
        memcpy(s->pcm + s->head, pcm, first * sizeof(*pcm));
        memcpy(s->pcm, pcm + first, (samples - first) * sizeof(*pcm));
        s->head = (s->head + samples) % PCM_CAPACITY;
        s->next += samples;
        s->stamp = now;
        segment->end = s->next;
        segment->stamp = now;
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
    if (!leased(now) && atomic_load_explicit(&s_lease_until, memory_order_relaxed)) {
        ++s->renewals;
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
    uint32_t oldest = s->next > PCM_CAPACITY ? s->next - PCM_CAPACITY : 0;
    unsigned segment_index = (s->segment_head + SEGMENT_CAPACITY - s->segment_count) % SEGMENT_CAPACITY;
    if (s->segment_count && oldest < s->segments[segment_index].first) {
        oldest = s->segments[segment_index].first;
    }
    uint32_t first = after;
    uint32_t flags = 0;
    if (generation != s->generation || after > s->next || after < oldest) {
        if (generation == s->generation && after < oldest) { flags |= REPLY_OVERRUN; }
        first = s->next - oldest > JOIN_SAMPLES ? s->next - JOIN_SAMPLES : oldest;
    }
    uint32_t end = s->next, stamp = s->stamp;
    unsigned count = 0;
    for (unsigned i = 0; i < s->segment_count && first < s->next; ++i) {
        const spectrum_segment_t *segment = &s->segments[segment_index];
        segment_index = (segment_index + 1) % SEGMENT_CAPACITY;
        if (segment->end <= first) { continue; }
        if (first < segment->first) { first = segment->first; flags |= REPLY_GAP; }
        end = segment->end;
        stamp = segment->stamp;
        count = end - first;
        break;
    }
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
    const uint32_t header[] = { s->boot, s->generation, active, now, stamp, first,
        s->next, count, s->path, atomic_load_explicit(&s_contention, memory_order_relaxed),
        end, flags, s->dropped, s->pauses, s->renewals };
    xSemaphoreGive(s_mutex);

    memcpy(out, "APC\x02", 4);
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
