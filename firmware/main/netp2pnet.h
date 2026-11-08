/*
 * netp2pnet -- NET-P2P engine (netcom mode 1): public-internet UDP direct via
 * STUN hole punching.  See netp2pnet.c for the protocol, and
 * docs/NETP2P-PUBNET-PLAN.md (v2) for the design.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register the NET-P2P engine; must run before netcom_init(). */
void netp2pnet_init(void);

/*
 * Append the "\"np2p\":{...}" JSON fragment (no surrounding commas) to buf,
 * NUL-terminated.  Emitted in every /api/status so the panel can show the
 * public address while any mode is active.
 */
void netp2pnet_status_json(char *buf, size_t n);

/* Peer table (panel / CLI).  ip is a dotted string; name up to 7 chars. */
esp_err_t netp2pnet_peer_add(const char *name, const char *ip, uint16_t port);
esp_err_t netp2pnet_peer_del(int index);
esp_err_t netp2pnet_peer_knock(int index);     /* force a fresh KNOCK */
int      netp2pnet_peer_count(void);
/* Fills the row for one peer; state is a static string.  -1 = no such peer. */
int netp2pnet_peer_info(int index, char *name, size_t name_len,
                        char *ip, size_t ip_len, uint16_t *port,
                        const char **state, int *rtt_ms,
                        uint32_t *rx, uint32_t *tx, uint32_t *drop);

/* Hub group-chat forwarder (device acts as repeater).  Persisted. */
esp_err_t netp2pnet_hub_set(bool on);
bool      netp2pnet_hub_on(void);

/*
 * Override the primary STUN server with "host:port"; an empty string
 * restores the built-in default list.  Persisted.
 */
esp_err_t netp2pnet_stun_set(const char *host_port);
const char *netp2pnet_stun_str(void);

/* Trigger an immediate STUN probe (console `np2p stun`). */
void netp2pnet_stun_now(void);

/* Human-readable multi-line summary for the serial console. */
void netp2pnet_console(char *out, size_t n);

/* Restore factory defaults: peers cleared, hub off, STUN list default. */
void netp2pnet_factory_reset(void);

#ifdef __cplusplus
}
#endif
