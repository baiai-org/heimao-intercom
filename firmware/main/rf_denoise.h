#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    RF_DENOISE_OFF,
    RF_DENOISE_ARMED,
    RF_DENOISE_PROCESSING,
    RF_DENOISE_MEMORY_BYPASS,
    RF_DENOISE_TIME_BYPASS,
} rf_denoise_state_t;

typedef struct {
    uint32_t frames;
    uint32_t max_us;
    uint32_t overruns;
    uint32_t workspace_bytes;
    rf_denoise_state_t state;
} rf_denoise_stats_t;

typedef bool (*rf_denoise_emit_t)(const int16_t *pcm, size_t n, void *context);

/* Boot/control task only, serialized by the caller. The workspace is retained
 * for subsequent comparisons and published once. */
void rf_denoise_prepare(void);
/* Monitor task only. Emits exactly the valid sample count on normal finish. */
void rf_denoise_start(void);
bool rf_denoise_push(const int16_t *pcm, size_t n, rf_denoise_emit_t emit, void *context);
bool rf_denoise_finish(rf_denoise_emit_t emit, void *context);
void rf_denoise_stats(rf_denoise_stats_t *out);
