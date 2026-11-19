#pragma once
#include <stddef.h>
#include <stdint.h>

/* Workspace must be max_align_t-aligned. The returned state lives inside it.
 * Save its used bytes at the same address to restore the pristine state. */
void *orl_speex_init(void *workspace, size_t capacity, int frame_samples,
                     int suppression_db, size_t *used);
void orl_speex_run(void *state, int16_t *pcm);
