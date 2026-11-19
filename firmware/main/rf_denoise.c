#include "rf_denoise.h"
#include "orl_speex.h"

#include <stdatomic.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"

#ifndef RF_DENOISE_HOST_TEST
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#endif

enum {
    FRAME_SAMPLES = 160,
    FADE_SAMPLES = FRAME_SAMPLES / 2,
    ARENA_BYTES = 24 * 1024,
    DRY_CAPACITY = RF_DENOISE_CAPTURE_LEAD_SAMPLES + RF_DENOISE_MAX_BLOCK,
    MAILBOX_CAPACITY = 8,
    READY_TO_RESUME = 3,
};
static const int suppression_db[] = {
    [RF_DENOISE_LEVEL_LIGHT] = -6,
    [RF_DENOISE_LEVEL_STANDARD] = -15,
    [RF_DENOISE_LEVEL_STRONG] = -24,
    [RF_DENOISE_LEVEL_EXTREME] = -36,
};
enum { PSRAM_CAPS = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT };
_Static_assert(RF_DENOISE_CAPTURE_LEAD_SAMPLES >= RF_DENOISE_NETWORK_LEAD_SAMPLES &&
               RF_DENOISE_NETWORK_LEAD_SAMPLES > 2 * FRAME_SAMPLES,
               "playback leads must fit the dry buffer and allow two analysis frames");

typedef struct {
    uint32_t generation;
    uint32_t frame;
    int64_t queued_us;
    bool flush;
    uint8_t level;
    int16_t pcm[FRAME_SAMPLES];
} job_t;

typedef struct {
    uint32_t generation;
    uint32_t frame;
    int16_t pcm[FRAME_SAMPLES];
} result_t;

typedef struct {
    void *arena, *initial, *processor;
    size_t used;
    job_t jobs[MAILBOX_CAPACITY];
    result_t results[MAILBOX_CAPACITY];
    int16_t dry[DRY_CAPACITY];
    int16_t input[FRAME_SAMPLES], selected[FRAME_SAMPLES];
} workspace_t;

typedef struct {
    _Atomic(workspace_t *) prepared;
    /* Single-producer/single-consumer queues. The audio owner writes job_head
     * and result_tail; the worker writes job_tail and result_head. */
    atomic_uint job_head, job_tail, result_head, result_tail;
    atomic_uint generation, needed_frame;

    /* Audio owner only. */
    workspace_t *work;
    uint32_t session, submitted, playing;
    size_t input_n, dry_head, dry_n, frame_offset, play_delay, lead;
    unsigned ready_run, fade_left;
    bool finished, use_wet, recovering;
    int32_t last_correction, fade_from;

    /* Worker only. */
    uint32_t worker_session, worker_frame;
    bool has_overlap;

    atomic_int state, level;
    atomic_uint frames, max_us, wait_max_us, overruns;
    atomic_uint input_frames, wet_frames, dry_frames, queue_full, recoveries;
} stream_t;

static stream_t s_streams[RF_DENOISE_ROUTE_COUNT];

static bool has_dac(rf_denoise_route_t route)
{
    return route != RF_DENOISE_BRIDGE && route != RF_DENOISE_NET_TX &&
           route != RF_DENOISE_RELAY_BRIDGE && route != RF_DENOISE_RELAY_TX;
}

static unsigned playback_delay(rf_denoise_route_t route)
{
    switch (route) {
    case RF_DENOISE_P2P_SPEAKER:
    case RF_DENOISE_P2P_RF:
    case RF_DENOISE_RELAY_SPEAKER:
    case RF_DENOISE_RELAY_RF:
        return RF_DENOISE_NETWORK_LEAD_SAMPLES;
    default:
        return RF_DENOISE_CAPTURE_LEAD_SAMPLES;
    }
}

static void publish_result(stream_t *s, workspace_t *w, const job_t *job)
{
    if (job->generation != atomic_load(&s->generation) ||
        job->frame - 1 < atomic_load(&s->needed_frame)) { return; }
    const unsigned head = atomic_load_explicit(&s->result_head, memory_order_relaxed);
    if (head - atomic_load_explicit(&s->result_tail, memory_order_acquire) == MAILBOX_CAPACITY) { return; }
    result_t *result = &w->results[head % MAILBOX_CAPACITY];
    result->generation = job->generation;
    result->frame = job->frame - 1;
    memcpy(result->pcm, job->pcm, sizeof(result->pcm));
    atomic_store_explicit(&s->result_head, head + 1, memory_order_release);
}

static bool service_job(void)
{
    stream_t *selected = NULL;
    workspace_t *work = NULL;
    int64_t earliest = INT64_MAX;
    unsigned tail = 0;
    for (unsigned i = 0; i < RF_DENOISE_ROUTE_COUNT; ++i) {
        stream_t *s = &s_streams[i];
        const unsigned next = atomic_load_explicit(&s->job_tail, memory_order_relaxed);
        if (next == atomic_load_explicit(&s->job_head, memory_order_acquire)) { continue; }
        workspace_t *w = atomic_load(&s->prepared);
        const job_t *job = &w->jobs[next % MAILBOX_CAPACITY];
        /* A result for frame N needs the first two 10 ms input frames
         * starting at N. Only the remaining lead is available to the worker. */
        const unsigned slack_samples = playback_delay(i) - 2 * FRAME_SAMPLES;
        const int64_t deadline = job->queued_us + slack_samples * 1000000 / 16000;
        if (deadline < earliest) {
            selected = s;
            work = w;
            earliest = deadline;
            tail = next;
        }
    }
    if (!selected) { return false; }

    stream_t *s = selected;
    job_t job = work->jobs[tail % MAILBOX_CAPACITY];
    atomic_store_explicit(&s->job_tail, tail + 1, memory_order_release);
    if (job.generation != atomic_load(&s->generation) ||
        job.frame < atomic_load(&s->needed_frame)) { return true; }
    if (s->worker_session != job.generation || s->worker_frame != job.frame) {
        memcpy(work->arena, work->initial, work->used);
        orl_speex_set_suppression(work->processor, suppression_db[job.level]);
        s->worker_session = job.generation;
        s->has_overlap = false;
    }
    s->worker_frame = job.frame + 1;
    if (job.flush && !s->has_overlap) { return true; }
    const int64_t start = esp_timer_get_time();
    const uint32_t waited = start > job.queued_us ? (uint32_t)(start - job.queued_us) : 0;
    orl_speex_run(work->processor, job.pcm);
    const uint32_t elapsed = (uint32_t)(esp_timer_get_time() - start);
    if (job.generation == atomic_load(&s->generation)) {
        atomic_fetch_add(&s->frames, 1);
        if (elapsed > atomic_load(&s->max_us)) { atomic_store(&s->max_us, elapsed); }
        if (waited > atomic_load(&s->wait_max_us)) { atomic_store(&s->wait_max_us, waited); }
        if (s->has_overlap && job.frame != 0) { publish_result(s, work, &job); }
    }
    s->has_overlap = !job.flush;
    return true;
}

#ifdef RF_DENOISE_HOST_TEST
bool rf_denoise_test_service(void) { return service_job(); }
extern void rf_denoise_test_notify(void);
static bool prepare_worker(void) { return true; }
static void wake_worker(void) { rf_denoise_test_notify(); }
#else
static TaskHandle_t s_worker;

static void denoise_worker(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (service_job()) { }
    }
}

static bool prepare_worker(void)
{
    /* Control tasks serialize preparation, and no active route exists until
     * this handle and its workspace have been published. DSP never writes flash. */
    return s_worker || xTaskCreatePinnedToCoreWithCaps(denoise_worker, "denoise", 4096,
        NULL, 6, &s_worker, tskNO_AFFINITY, PSRAM_CAPS) == pdPASS;
}

static void wake_worker(void) { xTaskNotifyGive(s_worker); }
#endif

void rf_denoise_prepare(rf_denoise_route_t route)
{
    stream_t *s = &s_streams[route];
    if (atomic_load(&s->prepared)) { return; }
    workspace_t *w = heap_caps_calloc(1, sizeof(*w), PSRAM_CAPS);
    if (!w) { return; }
    w->arena = heap_caps_aligned_alloc(_Alignof(max_align_t), ARENA_BYTES, PSRAM_CAPS);
    if (w->arena) {
        w->processor = orl_speex_init(w->arena, ARENA_BYTES, FRAME_SAMPLES,
                                      suppression_db[RF_DENOISE_LEVEL_STANDARD], &w->used);
    }
    if (w->processor) { w->initial = heap_caps_malloc(w->used, PSRAM_CAPS); }
    if (!w->initial || !prepare_worker()) {
        heap_caps_free(w->initial);
        heap_caps_free(w->arena);
        heap_caps_free(w);
        return;
    }
    memcpy(w->initial, w->arena, w->used);
    atomic_store(&s->prepared, w);
}

bool rf_denoise_start(rf_denoise_route_t route, rf_denoise_level_t level)
{
    if (level < RF_DENOISE_LEVEL_LIGHT || level > RF_DENOISE_LEVEL_EXTREME) { return false; }
    stream_t *s = &s_streams[route];
    s->work = atomic_load(&s->prepared);
    s->session = atomic_fetch_add(&s->generation, 1) + 1;
    s->submitted = s->playing = 0;
    s->input_n = s->dry_head = s->dry_n = s->frame_offset = 0;
    s->play_delay = playback_delay(route);
    s->lead = has_dac(route) ? s->play_delay : 0;
    s->finished = s->use_wet = s->recovering = false;
    s->ready_run = READY_TO_RESUME;
    s->fade_left = 0;
    s->last_correction = 0;
    atomic_store(&s->needed_frame, 0);
    atomic_store(&s->result_tail, atomic_load(&s->result_head));
    atomic_store(&s->input_frames, 0);
    atomic_store(&s->wet_frames, 0);
    atomic_store(&s->dry_frames, 0);
    atomic_store(&s->queue_full, 0);
    atomic_store(&s->recoveries, 0);
    atomic_store(&s->level, level);
    atomic_store(&s->state, s->work ? RF_DENOISE_PROCESSING : RF_DENOISE_MEMORY_BYPASS);
    return s->work != NULL;
}

void rf_denoise_stop(rf_denoise_route_t route)
{
    stream_t *s = &s_streams[route];
    rf_denoise_invalidate(route);
    s->work = NULL;
    s->dry_n = s->lead = 0;
    atomic_store(&s->state, RF_DENOISE_OFF);
}

void rf_denoise_invalidate(rf_denoise_route_t route)
{
    atomic_fetch_add(&s_streams[route].generation, 1);
}

static void enqueue(stream_t *s, bool flush)
{
    workspace_t *w = s->work;
    const uint32_t frame = s->submitted++;
    if (!flush) { atomic_fetch_add(&s->input_frames, 1); }
    const unsigned head = atomic_load_explicit(&s->job_head, memory_order_relaxed);
    if (head - atomic_load_explicit(&s->job_tail, memory_order_acquire) == MAILBOX_CAPACITY) {
        atomic_fetch_add(&s->queue_full, 1);
        return;
    }
    job_t *job = &w->jobs[head % MAILBOX_CAPACITY];
    job->generation = s->session;
    job->frame = frame;
    job->queued_us = esp_timer_get_time();
    job->flush = flush;
    job->level = atomic_load_explicit(&s->level, memory_order_relaxed);
    memcpy(job->pcm, w->input, s->input_n * sizeof(*job->pcm));
    memset(job->pcm + s->input_n, 0, (FRAME_SAMPLES - s->input_n) * sizeof(*job->pcm));
    atomic_store_explicit(&s->job_head, head + 1, memory_order_release);
    wake_worker();
}

static void append_pcm(stream_t *s, const int16_t *pcm, size_t n)
{
    workspace_t *w = s->work;
    const size_t tail = (s->dry_head + s->dry_n) % DRY_CAPACITY;
    const size_t first = n < DRY_CAPACITY - tail ? n : DRY_CAPACITY - tail;
    memcpy(w->dry + tail, pcm, first * sizeof(*pcm));
    memcpy(w->dry, pcm + first, (n - first) * sizeof(*pcm));
    s->dry_n += n;
    while (n) {
        const size_t room = FRAME_SAMPLES - s->input_n;
        const size_t count = n < room ? n : room;
        memcpy(w->input + s->input_n, pcm, count * sizeof(*pcm));
        s->input_n += count;
        pcm += count;
        n -= count;
        if (s->input_n == FRAME_SAMPLES) {
            enqueue(s, false);
            s->input_n = 0;
        }
    }
}

static bool take_result(stream_t *s)
{
    unsigned tail = atomic_load_explicit(&s->result_tail, memory_order_relaxed);
    const unsigned head = atomic_load_explicit(&s->result_head, memory_order_acquire);
    bool found = false;
    while (tail != head) {
        const result_t *result = &s->work->results[tail % MAILBOX_CAPACITY];
        if (result->generation == s->session) {
            if (result->frame > s->playing) { break; }
            if (result->frame == s->playing) {
                memcpy(s->work->selected, result->pcm, sizeof(result->pcm));
                found = true;
            }
        }
        ++tail;
        if (found) { break; }
    }
    atomic_store_explicit(&s->result_tail, tail, memory_order_release);
    return found;
}

static void select_frame(stream_t *s)
{
    const bool ready = take_result(s);
    atomic_store(&s->needed_frame, s->playing + 1);
    if (!ready) {
        s->ready_run = 0;
        s->recovering = true;
        atomic_fetch_add(&s->overruns, 1);
    } else if (s->ready_run < READY_TO_RESUME) {
        ++s->ready_run;
    }
    const bool wet = ready && s->ready_run == READY_TO_RESUME;
    if (wet != s->use_wet) {
        s->fade_left = FADE_SAMPLES;
        s->fade_from = s->last_correction;
        s->use_wet = wet;
    }
    if (wet && s->recovering) {
        atomic_fetch_add(&s->recoveries, 1);
        s->recovering = false;
    }
    atomic_fetch_add(wet ? &s->wet_frames : &s->dry_frames, 1);
    atomic_store(&s->state, wet ? RF_DENOISE_PROCESSING : RF_DENOISE_TIME_BYPASS);
}

static void take_pcm(stream_t *s, int16_t *pcm, size_t n)
{
    workspace_t *w = s->work;
    while (n) {
        if (!s->frame_offset) { select_frame(s); }
        size_t count = FRAME_SAMPLES - s->frame_offset;
        if (count > n) { count = n; }
        if (count > DRY_CAPACITY - s->dry_head) { count = DRY_CAPACITY - s->dry_head; }
        for (size_t i = 0; i < count; ++i) {
            const int32_t dry = w->dry[s->dry_head + i];
            int32_t correction = s->use_wet ? w->selected[s->frame_offset + i] - dry : 0;
            if (s->fade_left) {
                const int32_t old_weight = (int32_t)s->fade_left--;
                correction = (s->fade_from * old_weight +
                    correction * (FADE_SAMPLES - old_weight)) / FADE_SAMPLES;
            }
            int32_t value = dry + correction;
            if (value > INT16_MAX) { value = INT16_MAX; }
            if (value < INT16_MIN) { value = INT16_MIN; }
            pcm[i] = (int16_t)value;
            s->last_correction = value - dry;
        }
        s->dry_head = (s->dry_head + count) % DRY_CAPACITY;
        s->dry_n -= count;
        s->frame_offset += count;
        if (s->frame_offset == FRAME_SAMPLES) {
            s->frame_offset = 0;
            ++s->playing;
        }
        pcm += count;
        n -= count;
    }
}

static bool emit_pcm(stream_t *s, size_t n, rf_denoise_emit_t emit, void *context)
{
    int16_t output[FRAME_SAMPLES];
    while (n) {
        const size_t count = n < FRAME_SAMPLES ? n : FRAME_SAMPLES;
        take_pcm(s, output, count);
        if (!emit(output, count, context)) { return false; }
        n -= count;
    }
    return true;
}

bool rf_denoise_push(rf_denoise_route_t route, const int16_t *pcm, size_t n,
                     rf_denoise_emit_t emit, void *context)
{
    stream_t *s = &s_streams[route];
    if (!s->work) { return !n || emit(pcm, n, context); }
    if (s->finished || s->session != atomic_load(&s->generation)) { return false; }
    while (n) {
        const size_t count = n < RF_DENOISE_MAX_BLOCK ? n : RF_DENOISE_MAX_BLOCK;
        append_pcm(s, pcm, count);
        if (s->dry_n > s->play_delay && !emit_pcm(s, s->dry_n - s->play_delay, emit, context)) {
            return false;
        }
        pcm += count;
        n -= count;
    }
    return true;
}

static void finish_input(stream_t *s)
{
    if (s->finished) { return; }
    s->finished = true;
    if (!s->dry_n) { s->lead = 0; return; }
    if (s->input_n) {
        enqueue(s, false);
        s->input_n = 0;
    }
    if (s->submitted) { enqueue(s, true); }
}

bool rf_denoise_finish(rf_denoise_route_t route, rf_denoise_emit_t emit, void *context)
{
    stream_t *s = &s_streams[route];
    if (!s->work) { return true; }
    if (s->session != atomic_load(&s->generation)) { return false; }
    finish_input(s);
    return emit_pcm(s, s->dry_n, emit, context);
}

size_t rf_denoise_play_take(rf_denoise_route_t route, int16_t *pcm, size_t n)
{
    stream_t *s = &s_streams[route];
    if (!s->work || !has_dac(route)) { return 0; }
    const size_t lead = n < s->lead ? n : s->lead;
    memset(pcm, 0, lead * sizeof(*pcm));
    s->lead -= lead;
    n -= lead;
    const size_t count = n < s->dry_n ? n : s->dry_n;
    take_pcm(s, pcm + lead, count);
    return lead + count;
}

bool rf_denoise_play_block(rf_denoise_route_t route, int16_t *pcm, size_t n)
{
    stream_t *s = &s_streams[route];
    if (!s->work || !has_dac(route) || s->finished || n > RF_DENOISE_MAX_BLOCK ||
        n > DRY_CAPACITY - s->dry_n) { return false; }
    append_pcm(s, pcm, n);
    return rf_denoise_play_take(route, pcm, n) == n;
}

bool rf_denoise_play_finish(rf_denoise_route_t route)
{
    stream_t *s = &s_streams[route];
    if (!s->work || !has_dac(route)) { return false; }
    finish_input(s);
    return true;
}

size_t rf_denoise_play_pending(rf_denoise_route_t route)
{
    const stream_t *s = &s_streams[route];
    return s->lead + s->dry_n;
}

void rf_denoise_stats(rf_denoise_route_t route, rf_denoise_stats_t *out)
{
    const stream_t *s = &s_streams[route];
    const workspace_t *w = atomic_load(&s->prepared);
    *out = (rf_denoise_stats_t){
        .frames = atomic_load(&s->frames),
        .max_us = atomic_load(&s->max_us),
        .overruns = atomic_load(&s->overruns),
        .workspace_bytes = w ? sizeof(*w) + ARENA_BYTES + w->used : 0,
        .state = atomic_load(&s->state),
        .level = atomic_load(&s->level),
        .wait_max_us = atomic_load(&s->wait_max_us),
        .input_frames = atomic_load(&s->input_frames),
        .wet_frames = atomic_load(&s->wet_frames),
        .dry_frames = atomic_load(&s->dry_frames),
        .queue_full = atomic_load(&s->queue_full),
        .recoveries = atomic_load(&s->recoveries),
    };
}
