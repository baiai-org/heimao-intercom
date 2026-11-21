/*
 * bridge -- analog <-> network bridge (OpenRadioLink-Net).
 *
 * In P2P or relay mode the bridge turns the board into a two-way intercom
 * between the air and the network:
 *
 *   RF -> net : the SA818's receive audio (squelch-gated) is forwarded into
 *               the active room, so network members can hear what is on the
 *               channel;
 *   net -> RF : room audio is re-transmitted on the air -- the PTT pad is
 *               keyed with the remote PTT and buffered playback, and the
 *               samples ride the DAC into the SA818's MIC_IN.
 *
 * The bridge works on an operator-selected preset channel. Only an explicit
 * channel selection tunes the SA818; enabling keeps the current profile. It only
 * becomes active in a network mode with the engine up, and the RF -> net
 * direction is muted while the bridge is keyed (half-duplex: no self-hearing
 * loop when the radio's own squelch opens on its own transmission).
 *
 * The monitor task is the audio source for the RF -> net tap, so the bridge
 * borrows it the same way a tail preview does (voice_monitor_borrow).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "rf_denoise.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Load the persisted selection.  Call once at boot, before the engines. */
void bridge_init(void);

/*
 * Re-evaluate the bridge state (borrow/release the monitor
 * task, keying).  Called by the setters and after intercom-mode changes;
 * safe to call whenever.
 */
void bridge_apply(void);

/* Panel/console setters: persist and re-evaluate. */
esp_err_t bridge_set_en(bool en);
esp_err_t bridge_set_chan(int index);   /* 0 = none, else 1-based channel index */
/* Internal: channel_control calls this only after a successful module ACK. */
esp_err_t bridge_channel_selected(int index, bool bind_bridge);
/* Preserve the same bound entry when the channel list is compacted; no tuning. */
void bridge_channel_deleted(int index); /* deleted 0-based slot */

bool bridge_en(void);
int  bridge_chan(void);                 /* 1-based; 0 = none */
bool bridge_active(void);               /* en + network mode + channel + engine */
bool bridge_keyed(void);                /* RF pad currently keyed by the bridge */

/*
 * RF -> net tap, called by the monitor task per output block: forwards what
 * the speaker is playing (squelch-gated radio audio) to the active engine.
 * `squelch_open` becomes the ptt flag on the wire; `enabled` is false while
 * a tail preview is consuming the blocks (a local test, not the channel).
 */
void bridge_rx_tap(const int16_t *pcm, size_t n, bool squelch_open, bool enabled);

/* End the RF -> network stream before monitor capture stops. Idempotent. */
void bridge_rx_end(void);
/* Close a suppressed receive stream without generating a new courtesy tone. */
void bridge_rx_inhibit(void);

/* Pause the denoised RF stream while changing the active P2P room. */
void bridge_rx_room_change(bool changing);
/* Network selectors use their NETCOM_MODE values for r11-95 compatibility. */
enum {
    DENOISE_SETTING_SPEAKER = 3,
    DENOISE_SETTING_HANDSET_TX = 4,
    DENOISE_SETTING_COUNT = 10,
    DENOISE_LEVEL_BITS = 3,
};
typedef enum {
    DENOISE_NET_TX,
    DENOISE_NET_SPEAKER,
    DENOISE_NET_RF,
} denoise_network_path_t;
#define DENOISE_CONFIG_MASK 0x7feu
#define DENOISE_LEVELS_MAX ((1u << (DENOISE_LEVEL_BITS * DENOISE_SETTING_COUNT)) - 1u)
typedef struct {
    uint32_t levels;
    rf_denoise_level_t selection_level;
} bridge_denoise_config_t;

uint8_t bridge_denoise_network_setting(uint8_t mode, denoise_network_path_t path);
uint32_t bridge_denoise_levels(void);
bridge_denoise_config_t bridge_denoise_config(void);
rf_denoise_level_t bridge_denoise_level(uint32_t levels, uint8_t setting);
uint32_t bridge_denoise_flags(uint32_t levels);
rf_denoise_route_t bridge_denoise_route(uint8_t setting);
esp_err_t bridge_denoise_save_selection(rf_denoise_level_t level, uint32_t paths,
                                        bridge_denoise_config_t expected);
/* The mask uses setting IDs as bit positions. A packed level uses three bits
 * starting at (setting - 1) * 3, with zero for all unmasked settings. */
esp_err_t bridge_denoise_update_levels(uint8_t mode, uint32_t mask, uint32_t expected, uint32_t values);
/* Legacy Boolean API. Enabling an off route selects Standard. */
esp_err_t bridge_denoise_update(uint8_t mode, uint32_t mask, uint32_t expected, uint32_t values);
esp_err_t bridge_denoise_set(uint8_t setting, bool enabled, bool expected);
void bridge_denoise_status(uint8_t mode, uint32_t levels, uint8_t *active_mode, rf_denoise_stats_t *stats);

/* Read-only stream-edge diagnostics; timestamps are local uptime in ms. */
typedef struct {
    bool open;
    uint32_t starts;
    uint32_t ends;
    uint32_t captured_samples, submitted_samples, rejected_samples;
    int64_t start_ms;
    int64_t end_ms;
} bridge_rx_stats_t;
void bridge_rx_stats(bridge_rx_stats_t *out);

/* Called from netcom when the intercom mode changes (and at boot). */
void bridge_mode_changed(void);

#ifdef __cplusplus
}
#endif
