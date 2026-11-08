/*
 * LAN control panel -- OpenRadioLink-v0.3
 *
 * A small HTTP server exposing the radio over the local network:
 *
 *   GET /              the panel (self-contained HTML, embedded in the image)
 *   GET /api/status    JSON snapshot of everything worth watching
 *   GET /api/set?k=&v= change one setting
 *   GET /api/talk?on=1|0  key / unkey the transmitter
 *
 * A WiFi link is not required: if the station never comes up the rest of the
 * firmware behaves exactly as before, and the server simply has no address to
 * be reached on.
 */
#pragma once

#include "esp_err.h"

/* Starts the HTTP server.  Safe to call when WiFi is down -- the sockets exist
 * but have no address until the station connects. */
esp_err_t net_http_start(void);
