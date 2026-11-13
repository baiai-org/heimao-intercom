#include "monitor_rx.h"
#include <string.h>

void monitor_rx_init(monitor_rx_t *m, int16_t *ring, size_t capacity)
{
    memset(m, 0, sizeof(*m)); m->ring = ring; m->capacity = capacity;
}
void monitor_rx_clear(monitor_rx_t *m)
{
    m->tail = m->head; m->source = MONITOR_RX_NONE; m->network_until_us = 0;
}
void monitor_rx_mode(monitor_rx_t *m, uint8_t mode)
{
    if (m->mode != mode) { monitor_rx_clear(m); m->mode = mode; }
}
void monitor_rx_local_tx(monitor_rx_t *m, bool on)
{
    if (m->local_tx != on) { monitor_rx_clear(m); m->local_tx = on; }
}
static bool usable(const monitor_rx_t *m, const int16_t *pcm, size_t n)
{
    return !m->local_tx && m->ring && pcm && n && n <= m->capacity;
}
static void put(monitor_rx_t *m, monitor_rx_source_t source, const int16_t *pcm, size_t n)
{
    if (m->source != source) { m->tail = m->head; m->source = source; }
    const size_t used = m->head - m->tail;
    if (used + n > m->capacity) {
        const size_t drop = used + n - m->capacity;
        m->tail += drop; m->drops += (uint32_t)drop;
    }
    for (size_t i = 0; i < n; ++i) { m->ring[m->head++ % m->capacity] = pcm[i]; }
}
void monitor_rx_rf(monitor_rx_t *m, const int16_t *pcm, size_t n, bool enabled, int64_t now)
{
    if (!enabled) {
        /* A closed RF squelch/guard owns only the RF source. Never flush NET. */
        if (m->source == MONITOR_RX_RF) { monitor_rx_clear(m); }
        return;
    }
    if (!usable(m, pcm, n) || now < m->network_until_us) { return; }
    put(m, MONITOR_RX_RF, pcm, n); m->rf_samples += (uint32_t)n;
}
void monitor_rx_network(monitor_rx_t *m, uint8_t mode, const int16_t *pcm, size_t n, int64_t now)
{
    if (!usable(m, pcm, n) || (mode != 1 && mode != 2) || m->mode != mode) { return; }
    /* Priority across short playout gaps, without delaying or buffering the
     * network copy. The queue can drain its final samples after this expires. */
    m->network_until_us = now + 250000;
    put(m, MONITOR_RX_NETWORK, pcm, n); m->network_samples += (uint32_t)n;
}
size_t monitor_rx_take(monitor_rx_t *m, int16_t *out, size_t n)
{
    if (!out || !m->ring) { return 0; }
    const size_t used = m->head - m->tail;
    if (n > used) { n = used; }
    for (size_t i = 0; i < n; ++i) { out[i] = m->ring[m->tail++ % m->capacity]; }
    return n;
}
