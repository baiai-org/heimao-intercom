/*
 * WiFi station link -- OpenRadioLink-v0.3
 *
 * Joins the operator's LAN so the web panel can be reached from a browser.
 * Credentials live in NVS and are set once over the serial console:
 *
 *     x wifi <ssid> <password>
 *     x wifi            (show the current state)
 *     x wifi off        (forget the network)
 *
 * No credentials are compiled in: a board that ships with a hardcoded password
 * is a board whose password is in the repository forever.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Start the worker. With no eligible saved networks, open the provisioning AP.
 * Existing radio and network-voice business does not depend on an App session. */
esp_err_t net_wifi_start(void);

/* Schedule a debug-only Wi-Fi radio stop for this boot.  Credentials are not
 * changed; a reboot starts Wi-Fi normally again. */
esp_err_t net_wifi_debug_radio_off_schedule(void);

/* Store credentials and (re)connect.  Passing an empty ssid clears them.
 * `host` is the mDNS name the panel is published as (without ".local");
 * NULL or empty keeps the current name (default "orl").  With several boards
 * on one LAN each needs its own name or the .local address is ambiguous. */
esp_err_t net_wifi_set_credentials(const char *ssid, const char *password,
                                   const char *host);
esp_err_t net_wifi_clear(void);

/* Save first, ACK the HTTP request, then schedule connect. NULL password keeps
 * an existing secret. IDs remain stable and JSON never returns credentials. */
esp_err_t net_wifi_profile_save(uint32_t *id, const char *ssid, const char *password,
                               bool auto_join, int priority, const char *host);
esp_err_t net_wifi_profile_connect(uint32_t id, unsigned delay_ms);
esp_err_t net_wifi_profile_can_connect(uint32_t id);
esp_err_t net_wifi_profile_forget(uint32_t id);
esp_err_t net_wifi_profiles_json(char *out, size_t len); /* at least 7800 bytes */

/*
 * Provisioning access point.
 *
 * Started automatically when no credentials are stored, so the network can be
 * configured from a browser instead of a serial cable -- and so that no
 * password ever has to be pasted into a terminal or a chat log.  The board
 * raises an open AP and answers on http://192.168.4.1/; submitting the form
 * stores the credentials and joins the real network.
 */
esp_err_t   net_wifi_setup_ap_start(void);
void        net_wifi_setup_ap_stop(void);
bool        net_wifi_setup_ap_active(void);
/* AP-local provisioning client hold: renew every 15 s, expires after 60 s.
 * It only pauses automatic scanning/rejoin; explicit scan/connect still work. */
esp_err_t   net_wifi_setup_session(const char *owner, bool release);
const char *net_wifi_setup_ssid(void);

/* The AP's own address, or "" when down. The AP and an associated STA never
 * coexist on this board; APSTA is used only for unassociated scanning. */
const char *net_wifi_ap_ip(void);

/*
 * Scan for access points and write a JSON array of what was found:
 *
 *   [{"ssid":"Home","rssi":-46,"auth":3,"hidden":false}, ...]
 *
 * Names are de-duplicated, strongest first: a mesh or a router with 2.4 and 5
 * GHz radios reports the same SSID several times, and a list of four identical
 * entries is worse than useless to the person picking one.
 *
 * Nonblocking: returns cached results and schedules an asynchronous refresh.
 * /api/wifi reports scan_running and scan_age_ms; poll after it completes.
 *
 * Scans may briefly disturb the single radio. Healthy STA links are never
 * scanned automatically; explicit force requires the current control lease.
 */
esp_err_t net_wifi_scan_json(char *out, size_t out_len, bool force);

bool        net_wifi_connected(void);
/* Drop accounting: total disconnects, last reason code, ms since the last one. */
void        net_wifi_disc_stats(uint32_t *count, int *reason, int64_t *age_ms);
const char *net_wifi_ip(void);
/* Preferred global/ULA/link-local IPv6 address, "" when none. */
const char *net_wifi_ip6(void);
/* JSON array of every IPv6 address on the STA netif (diagnostics). */
int net_wifi_ip6_json(char *out, size_t out_len);
/* Log the IPv6 state of the STA interface (esp_netif + lwIP). */
void net_wifi_ip6_debug(const char *why);        /* dotted quad, or "" */
const char *net_wifi_ssid(void);
const char *net_wifi_hostname(void);  /* mDNS name without ".local", e.g. "orl" */
void net_wifi_name_changed(void);
int         net_wifi_rssi_dbm(void);  /* 0 when not connected */
const char *net_wifi_state_name(void);
