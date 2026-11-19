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

typedef enum {
    RF_DENOISE_BRIDGE,
    RF_DENOISE_SPEAKER,
    RF_DENOISE_HANDSET_TX,
    RF_DENOISE_NET_TX,
    RF_DENOISE_P2P_SPEAKER,
    RF_DENOISE_P2P_RF,
    RF_DENOISE_RELAY_SPEAKER,
    RF_DENOISE_RELAY_RF,
    RF_DENOISE_ROUTE_COUNT,
} rf_denoise_route_t;

#define RF_DENOISE_MAX_BLOCK 320u

/* Boot/control task only, serialized by the caller. The workspace is retained
 * for subsequent comparisons and published once. */
void rf_denoise_prepare(rf_denoise_route_t route);
/* One audio task owns each route. An unprepared route emits unmodified PCM. */
/* shared_budget when two denoisers may run in the same audio task per block. */
bool rf_denoise_start(rf_denoise_route_t route, bool shared_budget);
void rf_denoise_stop(rf_denoise_route_t route);
bool rf_denoise_push(rf_denoise_route_t route, const int16_t *pcm, size_t n,
                     rf_denoise_emit_t emit, void *context);
bool rf_denoise_finish(rf_denoise_route_t route, rf_denoise_emit_t emit, void *context);
void rf_denoise_stats(rf_denoise_route_t route, rf_denoise_stats_t *out);

/* Local DAC routes use fixed-size playback, including a 20 ms startup lead.
 * Call finish once, then take the remaining PCM before releasing playback. */
bool rf_denoise_play_block(rf_denoise_route_t route, int16_t *pcm, size_t n);
bool rf_denoise_play_finish(rf_denoise_route_t route);
size_t rf_denoise_play_take(rf_denoise_route_t route, int16_t *pcm, size_t n);
size_t rf_denoise_play_pending(rf_denoise_route_t route);
