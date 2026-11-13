/* Post-RF-TX receive inhibit, shared by P2P and MQTT analog bridges. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define RX_GUARD_MAX_MS 60000
#define RX_GUARD_NETWORK 1u
#define RX_GUARD_SDR     2u
#define RX_GUARD_TALK    4u
#define RX_GUARD_OUT_NET 1u
#define RX_GUARD_OUT_SDR 2u
#define RX_GUARD_OUT_SPK 4u
#define RX_GUARD_ALL     7u

typedef struct {
    uint32_t delay_ms;
    uint8_t enabled, sources, outputs, reserved;
} rx_guard_config_t;
typedef struct {
    rx_guard_config_t p2p, relay;
    uint32_t remaining_ms;
    uint8_t blocked;             /* output mask, zero outside the guard */
} rx_guard_status_t;

void rx_guard_init(void);
/* Only an active analog bridge in mode 1 or 2 can arm the guard. Changing
 * context cancels the old deadline; a clock value never crosses mode changes. */
void rx_guard_context(uint8_t mode, bool active);
/* Called ONLY at the physical PTT output, including normal/forced/watchdog
 * releases. Never acquire a PTT/voice/bridge mutex from inside this module. */
void rx_guard_rf_state(bool keyed, uint8_t sources);
uint8_t rx_guard_blocked(void);
/* For an audio block, compare against its capture/processing START time too:
 * a block begun during the guard must not leak out after the deadline. */
uint8_t rx_guard_blocked_at(int64_t block_start_us);
void rx_guard_status(rx_guard_status_t *out);
/* Atomic per-mode save. Format: enabled,delay_ms,source_mask,output_mask.
 * Strict unsigned decimal integers only. New values affect the next release;
 * disabling cancels an in-progress guard immediately. */
esp_err_t rx_guard_configure(uint8_t mode, const char *value);
