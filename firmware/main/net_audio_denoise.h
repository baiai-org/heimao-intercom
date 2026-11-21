#pragma once

#include "rf_denoise.h"
#include "net_rx_order.h"

typedef struct {
    uint64_t sender_epoch;
    uint16_t first_seq; /* logical PTT start, independent of jitter-buffer priming */
    bool present;
    bool feeding;
    bool previous_completed;
    bool rf_suppressed;
    bool local_tail;
} net_audio_source_t;

typedef struct {
    net_audio_source_t source;
    bool active;
    bool drain_source;
    bool local_tail;
    bool tail_attempted;
    bool armed;
} net_rf_close_t;

static inline void net_rf_close_begin(net_rf_close_t *close, net_audio_source_t source,
                                      net_rx_order_t *order, bool drain_source)
{
    if (order) { order->rf_blocked = true; }
    *close = (net_rf_close_t){
        .source = source, .active = true, .drain_source = drain_source,
        .local_tail = !order || !order->tail_seen,
    };
}

static inline bool net_rf_close_suppresses(const net_rf_close_t *close, net_audio_source_t source,
                                           const net_rx_order_t *order)
{
    if (!close->active) { return order->rf_blocked; }
    return !close->drain_source || close->source.sender_epoch != source.sender_epoch ||
        close->source.first_seq != source.first_seq;
}

bool net_rf_close_hold(net_rf_close_t *close, bool enabled, bool draining);

typedef struct {
    net_audio_source_t source;
    rf_denoise_route_t route;
    int64_t close_ms;
    int64_t last_pcm_ms;
    uint16_t room;
    bool latched;
    bool rf;
    bool requested;
    bool active;
    bool pending;
    bool finished;
} net_audio_denoise_t;

net_audio_source_t net_audio_source(const uint8_t mac[6], uint16_t epoch,
                                    uint16_t first_seq, bool feeding);

static inline bool net_audio_source_changed(const net_audio_denoise_t *dn, net_audio_source_t next)
{
    return dn->latched && next.present &&
        (dn->source.sender_epoch != next.sender_epoch || dn->source.first_seq != next.first_seq);
}

/* The new packet stays in its jitter buffer while the old decoded tail is played. */
static inline bool net_audio_denoise_defer(const net_audio_denoise_t *dn, net_audio_source_t next)
{
    return dn->pending && next.present &&
        (dn->finished || (next.previous_completed && net_audio_source_changed(dn, next)));
}

bool net_audio_denoise_pending(net_audio_denoise_t *dn, int64_t now);
static inline bool net_audio_denoise_needs_lead(const net_audio_denoise_t *dn)
{
    return dn->active && !dn->finished && rf_denoise_play_needs_lead(dn->route);
}
void net_audio_denoise_add_lead(net_audio_denoise_t *dn, const int16_t *block, int64_t now);
bool net_audio_denoise_render(net_audio_denoise_t *dn, uint8_t mode,
                              net_audio_source_t source, bool have, bool output_ready,
                              int16_t *block, int64_t now);
void net_audio_denoise_reset(net_audio_denoise_t *dn);
