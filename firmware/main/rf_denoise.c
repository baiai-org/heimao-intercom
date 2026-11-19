#include "rf_denoise.h"
#include "orl_speex.h"

#include <stdatomic.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"

enum {
    FRAME_SAMPLES = 160,
    SUPPRESSION_DB = -6,
    SOLO_TARGET_US = 4500,
    SOLO_BURST_US = 4000,
    SOLO_LIMIT_US = 8000,
    SHARED_TARGET_US = 2600,
    SHARED_BURST_US = 2500,
    SHARED_LIMIT_US = 6000,
    BYPASS_FADE_SAMPLES = FRAME_SAMPLES / 2,
    ARENA_BYTES = 24 * 1024,
    PLAY_DELAY = 2 * FRAME_SAMPLES,
    PLAY_CAPACITY = PLAY_DELAY + RF_DENOISE_MAX_BLOCK,
};
enum { PSRAM_CAPS = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT };

typedef struct {
    void *arena;
    void *initial;
    void *processor;
    size_t used;
    int16_t input[FRAME_SAMPLES];
    int16_t previous[FRAME_SAMPLES];
    int16_t output[FRAME_SAMPLES];
    int16_t playback[];
} workspace_t;

typedef struct {
    _Atomic(workspace_t *) prepared;
    workspace_t *work;
    bool bypass;
    bool wet_emitted;
    bool shared_budget;
    uint32_t work_debt_us;
    size_t input_n;
    size_t previous_n;
    size_t head;
    size_t queued;
    size_t lead;
    atomic_int state;
    atomic_uint frames;
    atomic_uint max_us;
    atomic_uint overruns;
} stream_t;

static stream_t s_streams[RF_DENOISE_ROUTE_COUNT];

static bool has_playback(rf_denoise_route_t route)
{
    return route != RF_DENOISE_BRIDGE && route != RF_DENOISE_NET_TX;
}

static size_t workspace_size(rf_denoise_route_t route)
{
    return sizeof(workspace_t) + (has_playback(route) ? PLAY_CAPACITY * sizeof(int16_t) : 0);
}

void rf_denoise_prepare(rf_denoise_route_t route)
{
    stream_t *stream = &s_streams[route];
    if (atomic_load(&stream->prepared)) { return; }
    workspace_t *work = heap_caps_calloc(1, workspace_size(route), PSRAM_CAPS);
    if (!work) { return; }
    work->arena = heap_caps_aligned_alloc(_Alignof(max_align_t), ARENA_BYTES, PSRAM_CAPS);
    if (work->arena) {
        work->processor = orl_speex_init(work->arena, ARENA_BYTES, FRAME_SAMPLES,
                                        SUPPRESSION_DB, &work->used);
    }
    if (work->processor) { work->initial = heap_caps_malloc(work->used, PSRAM_CAPS); }
    if (!work->initial) {
        heap_caps_free(work->arena);
        heap_caps_free(work);
        return;
    }
    memcpy(work->initial, work->arena, work->used);
    atomic_store(&stream->prepared, work);
}

bool rf_denoise_start(rf_denoise_route_t route, bool shared_budget)
{
    stream_t *stream = &s_streams[route];
    stream->work = atomic_load(&stream->prepared);
    stream->bypass = !stream->work;
    stream->wet_emitted = false;
    stream->shared_budget = shared_budget;
    stream->work_debt_us = 0;
    stream->input_n = stream->previous_n = stream->head = stream->queued = 0;
    stream->lead = PLAY_DELAY;
    if (stream->work) {
        memcpy(stream->work->arena, stream->work->initial, stream->work->used);
    }
    atomic_store(&stream->state, stream->work ? RF_DENOISE_PROCESSING : RF_DENOISE_MEMORY_BYPASS);
    return stream->work != NULL;
}

void rf_denoise_stop(rf_denoise_route_t route)
{
    stream_t *stream = &s_streams[route];
    stream->work = NULL;
    stream->bypass = true;
    stream->wet_emitted = false;
    stream->input_n = stream->previous_n = stream->head = stream->queued = stream->lead = 0;
    atomic_store(&stream->state, RF_DENOISE_OFF);
}

static bool run_frame(stream_t *stream)
{
    const int64_t start = esp_timer_get_time();
    orl_speex_run(stream->work->processor, stream->work->output);
    const uint32_t us = (uint32_t)(esp_timer_get_time() - start);
    atomic_fetch_add(&stream->frames, 1);
    if (us > atomic_load(&stream->max_us)) { atomic_store(&stream->max_us, us); }
    const uint32_t target = stream->shared_budget ? SHARED_TARGET_US : SOLO_TARGET_US;
    const uint32_t burst = stream->shared_budget ? SHARED_BURST_US : SOLO_BURST_US;
    const uint32_t limit = stream->shared_budget ? SHARED_LIMIT_US : SOLO_LIMIT_US;
    /* Faster 10 ms frames repay prior excess. The remaining debt bounds
     * repeated overload, while the absolute limit bounds isolated stalls. */
    if (us <= limit) {
        const uint32_t work = stream->work_debt_us + us;
        stream->work_debt_us = work > target ? work - target : 0;
        if (stream->work_debt_us <= burst) { return true; }
    }
    atomic_fetch_add(&stream->overruns, 1);
    atomic_store(&stream->state, RF_DENOISE_TIME_BYPASS);
    stream->bypass = true;
    return false;
}

static bool emit_previous(stream_t *stream, size_t count, bool on_time,
                          rf_denoise_emit_t emit, void *context)
{
    if (!count) { return true; }
    workspace_t *work = stream->work;
    const int16_t *pcm = work->output;
    if (!on_time) {
        if (stream->wet_emitted && count > 1) {
            // The completed output and the saved raw frame describe the same samples.
            const int last = (int)(count < BYPASS_FADE_SAMPLES ? count : BYPASS_FADE_SAMPLES) - 1;
            for (int i = 1; i < last; ++i) {
                const int32_t mixed = (last - i) * (int32_t)work->output[i] +
                                      i * (int32_t)work->previous[i];
                work->output[i] = (int16_t)(mixed / last);
            }
            memcpy(work->output + last, work->previous + last, (count - last) * sizeof(*pcm));
        } else {
            pcm = work->previous;
        }
    }
    if (!emit(pcm, count, context)) { return false; }
    stream->wet_emitted = on_time;
    return true;
}

static bool submit_frame(stream_t *stream, rf_denoise_emit_t emit, void *context)
{
    workspace_t *work = stream->work;
    memcpy(work->output, work->input, stream->input_n * sizeof(*work->input));
    memset(work->output + stream->input_n, 0, (FRAME_SAMPLES - stream->input_n) * sizeof(*work->output));
    const bool on_time = run_frame(stream);

    /* The overlap/add result aligns with the previous input frame. */
    if (!emit_previous(stream, stream->previous_n, on_time, emit, context)) { return false; }
    if (!on_time) {
        const size_t count = stream->input_n;
        stream->input_n = stream->previous_n = 0;
        return emit(work->input, count, context);
    }

    memcpy(work->previous, work->input, stream->input_n * sizeof(*work->input));
    stream->previous_n = stream->input_n;
    stream->input_n = 0;
    return true;
}

bool rf_denoise_push(rf_denoise_route_t route, const int16_t *pcm, size_t n,
                     rf_denoise_emit_t emit, void *context)
{
    stream_t *stream = &s_streams[route];
    while (!stream->bypass && n) {
        const size_t room = FRAME_SAMPLES - stream->input_n;
        const size_t count = n < room ? n : room;
        memcpy(stream->work->input + stream->input_n, pcm, count * sizeof(*pcm));
        pcm += count;
        n -= count;
        stream->input_n += count;
        if (stream->input_n == FRAME_SAMPLES && !submit_frame(stream, emit, context)) { return false; }
    }
    return !n || emit(pcm, n, context);
}

bool rf_denoise_finish(rf_denoise_route_t route, rf_denoise_emit_t emit, void *context)
{
    stream_t *stream = &s_streams[route];
    if (!stream->bypass && stream->input_n && !submit_frame(stream, emit, context)) { return false; }
    if (stream->bypass || !stream->previous_n) { return true; }
    workspace_t *work = stream->work;
    memset(work->output, 0, sizeof(work->output));
    const bool on_time = run_frame(stream);
    const size_t count = stream->previous_n;
    stream->previous_n = 0;
    return emit_previous(stream, count, on_time, emit, context);
}

static bool queue_pcm(const int16_t *pcm, size_t n, void *context)
{
    stream_t *stream = context;
    if (n > PLAY_CAPACITY - stream->queued) { return false; }
    const size_t tail = (stream->head + stream->queued) % PLAY_CAPACITY;
    const size_t first = n < PLAY_CAPACITY - tail ? n : PLAY_CAPACITY - tail;
    memcpy(stream->work->playback + tail, pcm, first * sizeof(*pcm));
    memcpy(stream->work->playback, pcm + first, (n - first) * sizeof(*pcm));
    stream->queued += n;
    return true;
}

size_t rf_denoise_play_take(rf_denoise_route_t route, int16_t *pcm, size_t n)
{
    stream_t *stream = &s_streams[route];
    if (!stream->work || !has_playback(route)) { return 0; }
    const size_t lead = n < stream->lead ? n : stream->lead;
    memset(pcm, 0, lead * sizeof(*pcm));
    stream->lead -= lead;
    n -= lead;
    const size_t take = n < stream->queued ? n : stream->queued;
    const size_t first = take < PLAY_CAPACITY - stream->head ? take : PLAY_CAPACITY - stream->head;
    memcpy(pcm + lead, stream->work->playback + stream->head, first * sizeof(*pcm));
    memcpy(pcm + lead + first, stream->work->playback, (take - first) * sizeof(*pcm));
    stream->head = (stream->head + take) % PLAY_CAPACITY;
    stream->queued -= take;
    return lead + take;
}

bool rf_denoise_play_block(rf_denoise_route_t route, int16_t *pcm, size_t n)
{
    stream_t *stream = &s_streams[route];
    if (!stream->work || !has_playback(route) || n > RF_DENOISE_MAX_BLOCK ||
        !rf_denoise_push(route, pcm, n, queue_pcm, stream)) { return false; }
    const size_t got = rf_denoise_play_take(route, pcm, n);
    return got == n;
}

bool rf_denoise_play_finish(rf_denoise_route_t route)
{
    stream_t *stream = &s_streams[route];
    if (!stream->work || !has_playback(route)) { return false; }
    /* A short burst can end before the 20 ms lead has been rendered. Only
     * carry the lead into drain if there is actual speech waiting behind it. */
    if (!stream->input_n && !stream->previous_n && !stream->queued) { stream->lead = 0; }
    return rf_denoise_finish(route, queue_pcm, stream);
}

size_t rf_denoise_play_pending(rf_denoise_route_t route)
{
    const stream_t *stream = &s_streams[route];
    return stream->lead + stream->queued;
}

void rf_denoise_stats(rf_denoise_route_t route, rf_denoise_stats_t *out)
{
    const stream_t *stream = &s_streams[route];
    const workspace_t *work = atomic_load(&stream->prepared);
    *out = (rf_denoise_stats_t){
        .frames = atomic_load(&stream->frames),
        .max_us = atomic_load(&stream->max_us),
        .overruns = atomic_load(&stream->overruns),
        .workspace_bytes = work ? workspace_size(route) + ARENA_BYTES + work->used : 0,
        .state = atomic_load(&stream->state),
    };
}
