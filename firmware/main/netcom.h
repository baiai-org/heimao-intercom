/*
 * netcom -- network intercom mode manager (OpenRadioLink-Net).
 *
 * The radio has three working modes:
 *
 *   NETCOM_MODE_LOCAL   0   local simulated intercom: TX/RX through the SA818
 *                           (the base firmware behaviour, unchanged)
 *   NETCOM_MODE_NETP2P  1   public-internet direct: STUN hole punching over
 *                           plain UDP, G.711 audio point-to-point (netp2pnet)
 *   NETCOM_MODE_RELAY   2   network relay: MQTT via an EMQX broker, FMO-style
 *                           (publish on the TX topic, subscribe the RX topic,
 *                           the broker is the "repeater")
 *
 * The old mode 1 (ESP-NOW direct P2P, room-grouped, no router) was removed on
 * 2026-10-16: it duplicated the public-internet mode for a use case the radios
 * already cover on the air, and it owned the ESP-NOW radio.  The remaining
 * modes were renumbered so the public mode sits at 1 and the relay at 2; NVS
 * values are migrated on load (see netcom_cfg_load).
 *
 * This module owns the mode configuration (NVS-persisted), hot-switches the
 * active transport engine, and exposes a uniform status interface.  The audio
 * front end (mic -> EQ) and back end (decode -> EQ -> amp) are shared; only
 * the middle changes.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NETCOM_MODE_LOCAL   0
#define NETCOM_MODE_NETP2P  1   /* public-internet STUN hole-punch UDP direct */
#define NETCOM_MODE_RELAY   2

/* Highest valid mode number; anything above is invalid. */
#define NETCOM_MODE_MAX     NETCOM_MODE_RELAY

/*
 * Version of the mode numbering, persisted next to the mode itself (NVS
 * "ncmodev").  Absent means the value predates the 2026-10-16 renumbering and
 * has to be translated; see netcom_cfg_load().
 */
#define NETCOM_MODE_NUMBERING 2

#define NETCOM_ROOM_MIN     1000
#define NETCOM_ROOM_MAX     9999

#define NETCOM_PSK_LEN      13   /* pre-shared key length (see netcom_psk) */
#define NETCOM_BROKER_LEN   32   /* host or host:port, NUL included */

/*
 * MQTT credentials for the relay (EMQX) mode.  The broker was anonymous-only
 * until now, which meant a public deployment had to rely on a firewall or a
 * VPN; these fields are what let it use the broker's own auth (EMQX
 * authentication, or a password file on any MQTT 3.1.1 broker).
 */
#define NETCOM_USER_LEN     32   /* NUL included */
#define NETCOM_PASS_LEN     48   /* NUL included */

typedef struct {
    uint8_t  mode;               /* NETCOM_MODE_*                        */
    uint16_t room;               /* 1000..9999 (NET-P2P group)           */
    uint8_t  psk[NETCOM_PSK_LEN];/* KNOCK handshake key (netp2pnet)      */
    bool     psk_set;
    char     broker[NETCOM_BROKER_LEN]; /* "host" or "host:port"         */
    uint16_t broker_port;        /* parsed port, default 1883            */
    char     user[NETCOM_USER_LEN];  /* MQTT username, "" = anonymous    */
    char     pass[NETCOM_PASS_LEN];  /* MQTT password, "" = none         */
    bool     creds_set;          /* user[0] != 0                         */
    /*
     * Network talk may also key the local radio and put the same microphone
     * audio on air (repeater-style), instead of only sending it to the room.
     * Off by default: in the network modes the radio normally stays in RX.
     */
    bool     rf_tx;
} netcom_cfg_t;

/* Engine vtable: one implementation per network mode. */
typedef struct netcom_engine netcom_engine_t;
struct netcom_engine {
    const char *name;
    /* start/stop run from the app task context; keep them fast. */
    esp_err_t (*start)(const netcom_cfg_t *cfg);
    esp_err_t (*stop)(void);
    /* Stage 2: audio hooks (PCM 8 kHz 16-bit, mono). */
    void      (*tx_sample)(const int16_t *pcm, size_t n, bool ptt);
    /*
     * Optional: the transmission is over -- flush whatever partial packet is
     * buffered and send one final frame with the PTT flag cleared, so the far
     * end can start releasing its own RF/forwarding the moment our tail tone
     * ends instead of waiting for a stream-idle timeout.  NULL is fine.
     */
    void      (*tx_end)(void);
    void      (*rx_sample)(int16_t *pcm, size_t *n);   /* 0 if none ready */
    /* Live stats for the panel: messages/peers + a one-word state. */
    void      (*stats)(uint32_t *tx_msgs, uint32_t *rx_msgs,
                       int *peers, const char **state);
    /*
     * Optional: the room changed while this engine is running.  No engine needs
     * it today -- the NET-P2P header and the relay topic are built per frame /
     * per connect from netcom_room() -- but the seam stays for an engine that
     * has to re-subscribe or re-announce.  NULL is fine.
     */
    void      (*on_room_change)(uint16_t room);
    /*
     * Optional: one-line diagnostics (e.g. mqtt retry progress) appended to
     * the panel status while the engine is running.  NULL is fine.
     */
    void      (*diag)(char *out, size_t n);
    /*
     * Optional: inject a test tone into the receive jitter buffer as if a
     * peer had sent it.  Single-board self-test of the playback path and the
     * analog bridge without a second device.  NULL is fine.
     */
    void      (*test_tone)(int ms);
};

void netcom_cfg_defaults(netcom_cfg_t *cfg);

/* NVS-backed load/save ("orl" namespace).  Load falls back to defaults. */
esp_err_t netcom_cfg_load(netcom_cfg_t *cfg);
esp_err_t netcom_cfg_save(const netcom_cfg_t *cfg);

/*
 * Load the config and start the engine for the current mode.  Called once at
 * boot, before the web panel starts.  Failing engines never block boot: the
 * radio keeps working in whatever the engine supports.
 */
esp_err_t netcom_init(void);

/*
 * Hot-switch the mode: persist, stop the current engine, start the new one.
 * The SA818 radio and the web panel keep running in every mode.
 */
esp_err_t netcom_set_mode(uint8_t mode);
esp_err_t netcom_set_room(uint16_t room);
esp_err_t netcom_set_broker(const char *host_port);   /* "host" or "host:port" */

/*
 * MQTT username/password for the relay engine.  Passing "" for the username
 * clears the credentials (back to an anonymous connection).  Persisted in NVS
 * and applied by restarting the relay engine when it is running, so the
 * operator sees the result of a wrong password in the panel diagnostics
 * immediately instead of after the next mode switch.  Must be called from a
 * task with an internal-RAM stack (it writes NVS) -- the HTTP handler is.
 */
esp_err_t netcom_set_credentials(const char *user, const char *pass);
/* Live credentials; both point into the config, never NULL ("" when unset). */
void netcom_credentials(const char **user, const char **pass, bool *set);


/* Current state accessors. */
uint8_t   netcom_mode(void);
uint16_t  netcom_room(void);
const char *netcom_broker_str(void);                  /* "host:port" */
uint16_t  netcom_broker_port(void);
/*
 * The 13-byte PSK (NVS ncpsk).  *set is true only if the operator configured
 * one; otherwise out is unmodified.  netp2pnet uses it as the KNOCK handshake
 * key (falling back to its own built-in default when unset).
 */
void      netcom_psk(uint8_t *out, bool *set);
const char *netcom_mode_name(uint8_t mode);           /* "local" | "netp2p" | "relay" */

/*
 * Route shaped TX audio (8 kHz, 16-bit mono) to the active engine.  In a
 * network mode the TALK task calls this instead of feeding the SA818's MIC_IN.
 * No-op in local mode or while no TX hook is registered.
 */
void netcom_tx_sample(const int16_t *pcm, size_t n, bool ptt);

/*
 * Flush the active engine's partial TX packet and send a final frame with the
 * PTT flag cleared.  Called once after the end-of-transmission tail tone so the
 * far end stops forwarding immediately.  No-op in local mode / without a hook.
 */
void netcom_tx_end(void);

/* Read-only end-marker timing. Times use each board's uptime, not wall time.
 * rx_carrier is the playback task's requested carrier, even with RF disabled. */
typedef struct {
    uint32_t tx_end_requests, tx_end_sent, tx_end_failed;
    uint32_t rx_ends, rx_timeouts;
    int64_t tx_end_ms, tx_sent_ms, tx_delay_ms;
    int64_t rx_end_ms, rx_release_ms;
    bool rx_ptt, rx_carrier;
} netcom_ptt_diag_t;
void netcom_ptt_diag_get(netcom_ptt_diag_t *out);
/* Engine instrumentation only; these functions never change PTT or audio. */
void netcom_note_end_sent(int64_t requested_ms, bool ok);
void netcom_note_rx_ptt(bool ptt, bool timeout);
void netcom_note_rx_carrier(bool carrier);

/* True while a network engine (NET-P2P / relay) is started.  Note this does NOT
 * mean audio can be carried: the relay starts before the broker accepts us. */
bool netcom_engine_running(void);

/* True when audio fed to the engine would actually go out: mode LOCAL, or the
 * relay is connected, or (for NET-P2P) at least one peer is linked.  Talk paths
 * must check this instead of netcom_engine_running(). */
bool netcom_tx_ready(void);

/* Why netcom_tx_ready() is false, in words a panel can show. */
const char *netcom_tx_ready_hint(void);

/* Network talk also keys the local radio (same mic audio on air). */
bool netcom_rf_tx(void);
esp_err_t netcom_set_rf_tx(bool on);

/*
 * Self-test: feed a 1 kHz tone into the active engine's jitter buffer as if a
 * peer had sent it (see the engine vtable's test_tone seam).  No-op in local
 * mode or while the engine has no test hook.
 */
void netcom_test_tone(int ms);

/*
 * Test hook: push `ms` of a locally generated 1 kHz tone through the ACTIVE
 * engine's TX path (ptt = true) and finish with a tx_end() -- byte for byte
 * what the TALK task would send, but without keying the SA818.  Lets one board
 * verify a whole network link (KNOCK handshake, jitter buffer, far-end
 * playback) against a Python peer or a second board without going on air.
 * Runs on its own short-lived task, so callers (HTTP handler, console) return
 * immediately.  ESP_ERR_INVALID_STATE while no engine runs / a test is already
 * in flight, ESP_ERR_INVALID_ARG outside 1..10000 ms.
 */

/* Engine stats (zeros + "idle" in local mode / while an engine is absent). */
/* Relay TX queue drops since boot (0 in NET-P2P). */
uint32_t netcom_tx_qdrop(void);

void netcom_stats(uint32_t *tx_msgs, uint32_t *rx_msgs, int *peers, const char **state);

/* One-line engine diagnostics ("" when the active engine has none). */
void netcom_diag(char *out, size_t n);

/*
 * Register (or replace) the engine for a network mode.  Called by netp2pnet.c /
 * netrelay.c at boot; a NULL engine removes the seam (stub behaviour).
 */
esp_err_t netcom_register_engine(uint8_t mode, const netcom_engine_t *engine);

#ifdef __cplusplus
}
#endif
