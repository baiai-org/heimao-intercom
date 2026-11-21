#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include "netcom.h"

#define NET_TX_PCM_CLOSE_SLOTS 2u
/* A paced courtesy tone waits behind congestion so a new over can follow. */
#define NET_TX_TAIL_BACKLOG_FRAMES 4u

/* Producers hold the engine's accumulator lock. The consumer only frees
 * slots, so a preflight accepts the whole input or leaves the FIFO untouched. */
static inline bool net_tx_pcm_fits(size_t slots, size_t block, size_t partial,
                                   size_t samples, bool boundary)
{
    const size_t flush = boundary && partial;
    const size_t full = ((flush ? 0 : partial) + samples) / block;
    /* The current over always owns one slot for its partial PCM and one OFF. */
    return slots >= flush + full + NET_TX_PCM_CLOSE_SLOTS;
}

typedef struct {
    atomic_uint accepted_samples, rejected_samples, expired_frames;
    atomic_uint encode_errors, send_errors;
} net_tx_pcm_counters_t;

static inline void net_tx_pcm_counters_read(const net_tx_pcm_counters_t *c, netcom_tx_audio_stats_t *out)
{
    *out = (netcom_tx_audio_stats_t){
        .accepted_samples = atomic_load(&c->accepted_samples),
        .rejected_samples = atomic_load(&c->rejected_samples),
        .expired_frames = atomic_load(&c->expired_frames),
        .encode_errors = atomic_load(&c->encode_errors),
        .send_errors = atomic_load(&c->send_errors),
    };
}
