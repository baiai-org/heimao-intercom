#include "audio_spectrum.h"
#include "audio_i2s.h"

#include <stdatomic.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

enum {
    PCM_CAPACITY = 24000,
    JOIN_SAMPLES = 2560,
    LEASE_MS = 2400,
    ACTIVE_MS = 180,
    SEGMENT_CAPACITY = 32,
    TAP_SLOT_COUNT = 64,
    TAP_SLOT_SAMPLES = 640,
    REPLY_OVERRUN = 1,
    REPLY_GAP = 2,
    REPLY_QUEUE_OVERFLOW = 4,
    REPLY_BRIDGE = 8,
    GAP_NONE = 0,
    GAP_CAPTURE_DROPPED = 1,
    GAP_TIME_PAUSE = 2,
    GAP_TRIMMED = 3,
};
_Static_assert(AUDIO_SAMPLE_RATE_HZ == 16000 && SPECTRUM_PATH_COUNT < 32,
               "Update the PCM protocol when the audio format changes");

typedef struct {
    uint32_t first, end, stamp;
    uint32_t gap_first, gap_end, gap_samples, gap_reason;
} spectrum_segment_t;

typedef struct {
    atomic_uint ready;
    uint32_t path, session, stamp;
    uint16_t samples;
    int16_t pcm[TAP_SLOT_SAMPLES];
} spectrum_tap_t;

typedef struct {
    uint32_t path_time[SPECTRUM_PATH_COUNT];
    audio_spectrum_path_t path, selected, bridge_path;
    uint32_t boot, generation, next, stamp, dropped, pauses, renewals;
    uint32_t bridge_session, bridge_stamp, bridge_direction;
    unsigned head, segment_head, segment_count;
    spectrum_segment_t segments[SEGMENT_CAPACITY];
    int16_t pcm[PCM_CAPACITY];
} spectrum_state_t;

static StaticSemaphore_t s_mutex_storage;
static SemaphoreHandle_t s_mutex;
static spectrum_state_t *s_state;
static spectrum_tap_t *s_taps;
static atomic_uint s_tap_head, s_tap_tail;
static atomic_uint s_lease_until, s_missed[SPECTRUM_PATH_COUNT], s_overflow;

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

static uint32_t bridge_direction(audio_spectrum_path_t path)
{
    switch (path) {
    case SPECTRUM_RF_P2P:
    case SPECTRUM_RF_MQTT:
        return 1;
    case SPECTRUM_P2P_RF:
    case SPECTRUM_MQTT_RF:
        return 2;
    default:
        return 0;
    }
}

static void reset_stream(spectrum_state_t *s)
{
    if (++s->generation == 0) { ++s->generation; }
    s->segment_head = s->segment_count = s->next = 0;
}

/* Gaps occupy logical time in the ring without writing fabricated samples.
 * Each reply covers at most one uninterrupted segment. */
static spectrum_segment_t *append_segment(spectrum_state_t *s, uint32_t gap, uint32_t reason)
{
    if (gap || !s->segment_count) {
        const uint32_t gap_first = s->next;
        s->next += gap;
        s->head = (s->head + gap % PCM_CAPACITY) % PCM_CAPACITY;
        s->segments[s->segment_head] = (spectrum_segment_t){
            .first = s->next,
            .gap_first = gap_first,
            .gap_end = s->next,
            .gap_samples = gap,
            .gap_reason = gap ? reason : GAP_NONE,
        };
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

static void touch_bridge(spectrum_state_t *s, audio_spectrum_path_t path, uint32_t session,
                         uint32_t stamp)
{
    const uint32_t direction = bridge_direction(path);
    if (!session || !direction) { return; }
    if (s->bridge_session != session || s->bridge_direction != direction) {
        reset_stream(s);
        s->bridge_session = session;
        s->bridge_direction = direction;
        s->bridge_path = path;
        s->bridge_stamp = stamp;
        return;
    }
    s->bridge_path = path;
    s->bridge_stamp = stamp;
}

static audio_spectrum_path_t choose_path(spectrum_state_t *s, audio_spectrum_path_t path,
                                         uint32_t now)
{
    if (path_active(s, s->selected, now)) { return s->selected; }
    if (s->bridge_session && s->bridge_direction &&
        now - s->bridge_stamp <= ACTIVE_MS &&
        path_active(s, s->bridge_path, now)) {
        return s->bridge_path;
    }
    if (!path_active(s, s->path, now) || priority(path) > priority(s->path)) { return path; }
    return s->path;
}

static void enqueue_block(audio_spectrum_path_t path, uint32_t session,
                          uint32_t stamp, const int16_t *pcm, size_t samples)
{
    unsigned head = 0;
    bool reserved = false;
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        head = atomic_load_explicit(&s_tap_head, memory_order_relaxed);
        const unsigned tail = atomic_load_explicit(&s_tap_tail, memory_order_acquire);
        if (head - tail >= TAP_SLOT_COUNT) { break; }
        if (atomic_compare_exchange_weak_explicit(&s_tap_head, &head, head + 1,
                                                  memory_order_acq_rel, memory_order_relaxed)) {
            reserved = true;
            break;
        }
    }
    if (!reserved) {
        atomic_fetch_add_explicit(&s_missed[path], samples, memory_order_relaxed);
        atomic_fetch_add_explicit(&s_overflow, 1, memory_order_relaxed);
        return;
    }
    spectrum_tap_t *slot = &s_taps[head % TAP_SLOT_COUNT];
    slot->path = path;
    slot->session = session;
    slot->stamp = stamp;
    slot->samples = (uint16_t)samples;
    memcpy(slot->pcm, pcm, samples * sizeof(*pcm));
    atomic_store_explicit(&slot->ready, head + 1, memory_order_release);
}

void audio_spectrum_tap_session(audio_spectrum_path_t path, uint32_t session,
                                const int16_t *pcm, size_t samples)
{
    if (path <= SPECTRUM_AUTO || path >= SPECTRUM_PATH_COUNT || !pcm || !samples) { return; }
    const uint32_t now = now_ms();
    if (!leased(now)) { return; }
    while (samples) {
        const size_t take = samples < TAP_SLOT_SAMPLES ? samples : TAP_SLOT_SAMPLES;
        enqueue_block(path, session, now ? now : 1, pcm, take);
        pcm += take;
        samples -= take;
    }
}

void audio_spectrum_tap(audio_spectrum_path_t path, const int16_t *pcm, size_t samples)
{
    audio_spectrum_tap_session(path, 0, pcm, samples);
}

void audio_spectrum_init(void)
{
    s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_storage);
}

static bool start_locked(void)
{
    if (s_state) { return true; }
    s_state = heap_caps_calloc(1, sizeof(*s_state), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_taps = heap_caps_calloc(TAP_SLOT_COUNT, sizeof(*s_taps), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_state || !s_taps) { return false; }
    s_state->boot = esp_random();
    s_state->generation = 1;
    atomic_store_explicit(&s_tap_head, 0, memory_order_relaxed);
    atomic_store_explicit(&s_tap_tail, 0, memory_order_relaxed);
    atomic_store_explicit(&s_overflow, 0, memory_order_relaxed);
    for (unsigned i = 0; i < TAP_SLOT_COUNT; ++i) {
        atomic_init(&s_taps[i].ready, 0);
    }
    return true;
}

static void consume_tap_locked(spectrum_state_t *s, const spectrum_tap_t *tap, uint32_t now)
{
    const audio_spectrum_path_t path = (audio_spectrum_path_t)tap->path;
    size_t samples = tap->samples;
    const int16_t *pcm = tap->pcm;
    touch_bridge(s, path, tap->session, tap->stamp);
    s->path_time[path] = tap->stamp;
    use_path(s, choose_path(s, path, now));
    const uint32_t missed = atomic_exchange_explicit(&s_missed[path], 0, memory_order_relaxed);
    if (s->path != path) { return; }

    uint32_t gap = s->segment_count ? missed : 0;
    uint32_t reason = gap ? GAP_CAPTURE_DROPPED : GAP_NONE;
    /* A poll drains a burst of captured blocks.  Compare each block with the
     * previously consumed block, not with the end of that burst. */
    const uint32_t interval = tap->stamp - s->stamp;
    const bool pause = s->segment_count && interval > ACTIVE_MS;
    if (pause) {
        const uint64_t elapsed = (uint64_t)interval * (AUDIO_SAMPLE_RATE_HZ / 1000);
        if (elapsed > samples && elapsed - samples > gap) {
            gap = (uint32_t)(elapsed - samples);
            reason = GAP_TIME_PAUSE;
        }
    }
    uint32_t trimmed = 0;
    if (samples > PCM_CAPACITY) {
        trimmed = samples - PCM_CAPACITY;
        pcm += trimmed;
        samples = PCM_CAPACITY;
        gap += trimmed;
        reason = GAP_TRIMMED;
    }
    if ((uint64_t)s->next + gap + samples > UINT32_MAX) {
        reset_stream(s);
        gap = 0;
        reason = GAP_NONE;
    }
    const unsigned next_head = gap ? (s->head + gap % PCM_CAPACITY) % PCM_CAPACITY : s->head;
    if (pinned(next_head, samples)) {
        atomic_fetch_add_explicit(&s_missed[path], missed + trimmed + samples, memory_order_relaxed);
        atomic_fetch_add_explicit(&s_overflow, 1, memory_order_relaxed);
        return;
    }
    s->dropped += missed + trimmed;
    if (pause) { ++s->pauses; }
    spectrum_segment_t *segment = append_segment(s, gap, reason);
    const size_t room = PCM_CAPACITY - s->head;
    const size_t first = samples < room ? samples : room;
    memcpy(s->pcm + s->head, pcm, first * sizeof(*pcm));
    memcpy(s->pcm, pcm + first, (samples - first) * sizeof(*pcm));
    s->head = (s->head + samples) % PCM_CAPACITY;
    s->next += samples;
    s->stamp = tap->stamp;
    segment->end = s->next;
    segment->stamp = tap->stamp;
}

/* Draining is deliberately serialized only with other HTTP readers. Audio
 * producers never take this mutex, so response work cannot create a visible
 * PCM gap. */
static void drain_taps_locked(spectrum_state_t *s, uint32_t now)
{
    unsigned tail = atomic_load_explicit(&s_tap_tail, memory_order_relaxed);
    const unsigned head = atomic_load_explicit(&s_tap_head, memory_order_acquire);
    while (tail != head) {
        spectrum_tap_t *slot = &s_taps[tail % TAP_SLOT_COUNT];
        if (atomic_load_explicit(&slot->ready, memory_order_acquire) != tail + 1) { break; }
        consume_tap_locked(s, slot, now);
        atomic_store_explicit(&slot->ready, 0, memory_order_release);
        ++tail;
        atomic_store_explicit(&s_tap_tail, tail, memory_order_release);
    }
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
    drain_taps_locked(s, now);

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
    const bool bridge_active = s->bridge_session && s->bridge_direction &&
        now - s->bridge_stamp <= ACTIVE_MS;
    uint32_t flags = bridge_active ? REPLY_BRIDGE : 0;
    if (generation != s->generation || after > s->next || after < oldest) {
        if (generation == s->generation && after < oldest) { flags |= REPLY_OVERRUN; }
        first = s->next - oldest > JOIN_SAMPLES ? s->next - JOIN_SAMPLES : oldest;
    }
    uint32_t end = s->next, stamp = s->stamp;
    uint32_t gap_first = 0, gap_end = 0, gap_samples = 0, gap_reason = GAP_NONE;
    unsigned count = 0;
    for (unsigned i = 0; i < s->segment_count && first < s->next; ++i) {
        const spectrum_segment_t *segment = &s->segments[segment_index];
        segment_index = (segment_index + 1) % SEGMENT_CAPACITY;
        if (segment->end <= first) { continue; }
        if (first < segment->first) { first = segment->first; flags |= REPLY_GAP; }
        if (segment->gap_samples) {
            gap_first = segment->gap_first;
            gap_end = segment->gap_end;
            gap_samples = segment->gap_samples;
            gap_reason = segment->gap_reason;
        }
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
    const uint32_t header[] = {
        s->boot, s->generation, active, now, stamp, first, s->next, count, s->path,
        atomic_load_explicit(&s_overflow, memory_order_relaxed), end, flags, s->dropped,
        s->pauses, s->renewals, s->bridge_session, gap_first, gap_end, gap_samples,
        gap_reason, s->bridge_direction, 0, 0,
    };
    xSemaphoreGive(s_mutex);

    memcpy(out, "APC\x03", 4);
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
