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

/* Read-only stream-edge diagnostics; timestamps are local uptime in ms. */
typedef struct {
    bool open;
    uint32_t starts;
    uint32_t ends;
    int64_t start_ms;
    int64_t end_ms;
} bridge_rx_stats_t;
void bridge_rx_stats(bridge_rx_stats_t *out);

/* Called from netcom when the intercom mode changes (and at boot). */
void bridge_mode_changed(void);

#ifdef __cplusplus
}
#endif
