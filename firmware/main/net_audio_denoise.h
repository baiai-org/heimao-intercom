#pragma once

#include "rf_denoise.h"

typedef struct {
    uint64_t sender_epoch;
    uint16_t first_seq;
    bool present;
    bool feeding;
    bool previous_completed;
} net_audio_source_t;

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
    return dn->pending && net_audio_source_changed(dn, next) && (next.previous_completed || dn->finished);
}

bool net_audio_denoise_pending(net_audio_denoise_t *dn, int64_t now);
bool net_audio_denoise_render(net_audio_denoise_t *dn, uint8_t mode,
                              net_audio_source_t source, bool have, bool output_ready,
                              int16_t *block, int64_t now);
void net_audio_denoise_reset(net_audio_denoise_t *dn);
