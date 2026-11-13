/* Receive ordering survives jitter-buffer drain/reset. No RTOS dependencies.
 * Epoch is an optional, backwards-compatible 16-bit talk marker. Keep recently
 * retired epochs so a delayed previous over cannot replace the current one.
 * This is bounded reordering protection, not authentication/replay security. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define NET_RX_FLAG_EPOCH 0x20u
#define NET_RX_RETIRED 16
typedef struct {
    uint16_t epoch, latest, retired[NET_RX_RETIRED];
    uint16_t probe_seq;
    uint8_t retired_next, probe_count;
    bool seen, ptt, audio;
    int64_t last_ms, probe_ms;
} net_rx_order_t;
enum { NET_RX_DROP, NET_RX_AUDIO, NET_RX_FORWARD, NET_RX_NEW };

/* Call on a tentative copy; commit ONLY after the audio queue accepts it.
 * AUDIO fills an unplayed hole without changing PTT or its freshness clock. */
static inline int net_rx_order_accept(net_rx_order_t *s, uint16_t epoch,
                                     uint16_t seq, bool ptt, int64_t now)
{
    bool fresh = !s->seen || epoch != s->epoch;
    if (s->seen && epoch != s->epoch) {
        /* Once an epoch-aware source is seen, unmarked old frames cannot
         * downgrade it. A deliberate legacy downgrade requires reconnect. */
        if (!epoch && s->epoch) { return NET_RX_DROP; }
        for (unsigned i = 0; i < NET_RX_RETIRED; ++i) {
            if (epoch && s->retired[i] == epoch) { return NET_RX_DROP; }
        }
    }
    if (!fresh) {
        /* Marked senders MUST advance epoch for a new over. Even a greater
         * sequence inside a closed epoch cannot revive it. Repeated legacy
         * OFFs likewise must not enqueue fresh silence after release. */
        if (!s->ptt && (epoch || !ptt)) { return NET_RX_DROP; }
        const int16_t delta = (int16_t)(seq - s->latest);
        if (delta <= 0) {
            /* A legacy sender has no reboot identity. After genuine silence,
             * require three sequential low counters before accepting restart.
             * One delayed old ON/OFF never changes an established over. */
            if (!epoch && now - s->last_ms > 2000 && seq < 16 && ptt) {
                if (s->probe_count && seq == (uint16_t)(s->probe_seq + 1) &&
                    now - s->probe_ms <= 200) { ++s->probe_count; }
                else { s->probe_count = 1; }
                s->probe_seq = seq; s->probe_ms = now;
                if (s->probe_count >= 3) { fresh = true; }
                else { return NET_RX_DROP; }
            } else {
                return delta < 0 && delta > -64 && s->ptt && ptt
                    ? NET_RX_AUDIO : NET_RX_DROP;
            }
        }
    }
    const bool start = fresh || (!s->ptt && ptt) || now - s->last_ms > 500;
    if (fresh) {
        if (s->seen && s->epoch && epoch != s->epoch) {
            s->retired[s->retired_next++ % NET_RX_RETIRED] = s->epoch;
        }
        s->epoch = epoch; s->seen = true;
    }
    if (start) { s->audio = false; }
    s->latest = seq; s->ptt = ptt; s->audio |= ptt;
    s->last_ms = now; s->probe_count = 0;
    return start ? NET_RX_NEW : NET_RX_FORWARD;
}

static inline bool net_rx_order_keyed(const net_rx_order_t *s, int64_t now,
                                     int timeout_ms)
{
    return s->seen && s->audio && s->ptt && now - s->last_ms <= timeout_ms;
}

/* Seed once per engine start, advance once per outgoing over. Old receivers
 * still use bit0/sequence/audio and ignore this optional metadata. */
static inline uint16_t net_tx_epoch(uint16_t *epoch, bool *open, bool ptt)
{
    if (ptt && !*open) { if (++*epoch == 0) { ++*epoch; } }
    *open = ptt;
    return *epoch;
}
