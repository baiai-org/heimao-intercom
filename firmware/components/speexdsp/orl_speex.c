#include "orl_speex.h"
#include "speex/speex_preprocess.h"

#include <setjmp.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static atomic_uint s_init_lock;
static uint8_t *s_workspace;
static size_t s_capacity;
static size_t s_used;
static jmp_buf s_no_space;

void *orl_speex_alloc(int size)
{
    /* The selected KISS FFT sizes use only radix 2/4/5 at run time.
     * All allocations for those sizes occur inside the constructor. */
    if (!s_workspace) { abort(); }
    const size_t alignment = _Alignof(max_align_t);
    const size_t start = (s_used + alignment - 1) & ~(alignment - 1);
    if (size <= 0 || start > s_capacity || (size_t)size > s_capacity - start) {
        /* Upstream dereferences allocation results in nested constructors.
         * Unwind the whole initializer; the caller owns the single arena. */
        longjmp(s_no_space, 1);
    }
    void *memory = s_workspace + start;
    memset(memory, 0, (size_t)size);
    s_used = start + (size_t)size;
    return memory;
}

void *orl_speex_init(void *workspace, size_t capacity, int frame_samples,
                     int suppression_db, size_t *used)
{
    *used = 0;
    if (!workspace || (uintptr_t)workspace % _Alignof(max_align_t) != 0 ||
        (frame_samples != 160 && frame_samples != 256 && frame_samples != 320)) {
        return NULL;
    }
    unsigned idle = 0;
    if (!atomic_compare_exchange_strong_explicit(&s_init_lock, &idle, 1,
            memory_order_acquire, memory_order_relaxed)) {
        return NULL;
    }
    s_workspace = workspace;
    s_capacity = capacity;
    s_used = 0;
    SpeexPreprocessState *state = NULL;
    if (setjmp(s_no_space) == 0) {
        state = speex_preprocess_state_init(frame_samples, 16000);
        spx_int32_t suppression = suppression_db;
        speex_preprocess_ctl(state, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &suppression);
        *used = s_used;
    } else {
        state = NULL;
    }
    s_workspace = NULL;
    atomic_store_explicit(&s_init_lock, 0, memory_order_release);
    return state;
}

void orl_speex_run(void *state, int16_t *pcm)
{
    (void)speex_preprocess_run(state, pcm);
}
