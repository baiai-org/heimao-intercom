/* Source-aware phone/browser monitor. Caller serializes all operations. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef enum { MONITOR_RX_NONE, MONITOR_RX_RF, MONITOR_RX_NETWORK } monitor_rx_source_t;
typedef struct {
    int16_t *ring;
    size_t capacity, head, tail;
    uint8_t mode;
    bool local_tx;
    monitor_rx_source_t source;
    int64_t network_until_us;
    uint32_t drops, rf_samples, network_samples;
} monitor_rx_t;
void monitor_rx_init(monitor_rx_t *m, int16_t *ring, size_t capacity);
void monitor_rx_clear(monitor_rx_t *m);
void monitor_rx_mode(monitor_rx_t *m, uint8_t mode);
void monitor_rx_local_tx(monitor_rx_t *m, bool on);
void monitor_rx_rf(monitor_rx_t *m, const int16_t *pcm, size_t n, bool enabled, int64_t now);
void monitor_rx_network(monitor_rx_t *m, uint8_t mode, const int16_t *pcm, size_t n, int64_t now);
size_t monitor_rx_take(monitor_rx_t *m, int16_t *out, size_t n);
