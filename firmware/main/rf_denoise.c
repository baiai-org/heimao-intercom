#include "rf_denoise.h"
#include "orl_speex.h"

#include <stdatomic.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"

enum {
    FRAME_SAMPLES = 160,
    SUPPRESSION_DB = -6,
    FRAME_BUDGET_US = 2500,
    ARENA_BYTES = 24 * 1024,
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
} workspace_t;

static _Atomic(workspace_t *) s_workspace;
static workspace_t *s_stream;
static size_t s_input_n;
static size_t s_previous_n;
static atomic_int s_state;
static atomic_uint s_frames;
static atomic_uint s_max_us;
static atomic_uint s_overruns;

void rf_denoise_prepare(void)
{
    if (atomic_load(&s_workspace)) { return; }
    workspace_t *work = heap_caps_calloc(1, sizeof(*work), PSRAM_CAPS);
    if (!work) { return; }
    work->arena = heap_caps_aligned_alloc(_Alignof(max_align_t), ARENA_BYTES, PSRAM_CAPS);
    if (work->arena) {
        work->processor = orl_speex_init(work->arena, ARENA_BYTES, FRAME_SAMPLES,
                                        SUPPRESSION_DB, &work->used);
    }
    if (work->processor) {
        work->initial = heap_caps_malloc(work->used, PSRAM_CAPS);
    }
    if (!work->initial) {
        heap_caps_free(work->arena);
        heap_caps_free(work);
        return;
    }
    memcpy(work->initial, work->arena, work->used);
    atomic_store(&s_workspace, work);
}

void rf_denoise_start(void)
{
    s_stream = atomic_load(&s_workspace);
    s_input_n = s_previous_n = 0;
    if (s_stream) {
        memcpy(s_stream->arena, s_stream->initial, s_stream->used);
    }
    atomic_store(&s_state, s_stream ? RF_DENOISE_PROCESSING : RF_DENOISE_MEMORY_BYPASS);
}

static bool run_frame(workspace_t *work)
{
    const int64_t start = esp_timer_get_time();
    orl_speex_run(work->processor, work->output);
    const uint32_t us = (uint32_t)(esp_timer_get_time() - start);
    atomic_fetch_add(&s_frames, 1);
    if (us > atomic_load(&s_max_us)) { atomic_store(&s_max_us, us); }
    if (us <= FRAME_BUDGET_US) { return true; }
    atomic_fetch_add(&s_overruns, 1);
    atomic_store(&s_state, RF_DENOISE_TIME_BYPASS);
    s_stream = NULL; /* Bypass the rest of this talkspurt. */
    return false;
}

static bool submit_frame(rf_denoise_emit_t emit, void *context)
{
    workspace_t *work = s_stream;
    memcpy(work->output, work->input, s_input_n * sizeof(*work->input));
    memset(work->output + s_input_n, 0, (FRAME_SAMPLES - s_input_n) * sizeof(*work->output));
    const bool on_time = run_frame(work);

    /* The overlap/add result aligns with the previous input frame. */
    const int16_t *previous = on_time ? work->output : work->previous;
    if (s_previous_n && !emit(previous, s_previous_n, context)) { return false; }
    if (!on_time) { return emit(work->input, s_input_n, context); }

    memcpy(work->previous, work->input, s_input_n * sizeof(*work->input));
    s_previous_n = s_input_n;
    s_input_n = 0;
    return true;
}

bool rf_denoise_push(const int16_t *pcm, size_t n, rf_denoise_emit_t emit, void *context)
{
    while (s_stream && n) {
        const size_t room = FRAME_SAMPLES - s_input_n;
        const size_t count = n < room ? n : room;
        memcpy(s_stream->input + s_input_n, pcm, count * sizeof(*pcm));
        pcm += count;
        n -= count;
        s_input_n += count;
        if (s_input_n == FRAME_SAMPLES && !submit_frame(emit, context)) { return false; }
    }
    return !n || emit(pcm, n, context);
}

bool rf_denoise_finish(rf_denoise_emit_t emit, void *context)
{
    if (s_stream && s_input_n && !submit_frame(emit, context)) { return false; }
    if (!s_stream || !s_previous_n) { return true; }
    workspace_t *work = s_stream;
    memset(work->output, 0, sizeof(work->output));
    const bool on_time = run_frame(work);
    const size_t count = s_previous_n;
    s_previous_n = 0;
    return emit(on_time ? work->output : work->previous, count, context);
}

void rf_denoise_stats(rf_denoise_stats_t *out)
{
    const workspace_t *work = atomic_load(&s_workspace);
    *out = (rf_denoise_stats_t){
        .frames = atomic_load(&s_frames),
        .max_us = atomic_load(&s_max_us),
        .overruns = atomic_load(&s_overruns),
        .workspace_bytes = work ? sizeof(*work) + ARENA_BYTES + work->used : 0,
        .state = atomic_load(&s_state),
    };
}
