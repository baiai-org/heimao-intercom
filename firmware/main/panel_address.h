/* Canonical panel address: a configured mDNS name discovers this board's IP.
 * This helper never uses a client-supplied address as the redirect target.
 */
#pragma once

#include <stdbool.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* A dotted-quad LAN address that is safe to put into a Location header. */
static inline bool panel_ip_address_ok(const char *ip)
{
    unsigned a, b, c, d;
    char extra;
    if (!ip || strlen(ip) > 15 || strspn(ip, "0123456789.") != strlen(ip)) {
        return false;
    }
    if (sscanf(ip, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4) { return false; }
    return a != 0 && a != 127 && a < 224 && b <= 255 && c <= 255 && d <= 255;
}

/* Canonical HTTPS entry for the panel: always this board's own address, never
 * a client-supplied host, so a plain-HTTP visit cannot bounce a client to an
 * attacker-chosen name. */
static inline bool panel_https_location(const char *ip, char *out, size_t cap)
{
    if (!out || !cap) { return false; }
    out[0] = '\0';
    if (!panel_ip_address_ok(ip)) { return false; }
    /* Copied by length, not printed: a short caller buffer must fail here,
     * and a constant ip/cap pair must not trip -Wformat-truncation. */
    const size_t len = strlen(ip);
    if (cap < 8 + len + 2) { return false; }
    memcpy(out, "https://", 8);
    memcpy(out + 8, ip, len);
    out[8 + len] = '/';
    out[9 + len] = '\0';
    return true;
}

static inline bool panel_address_location(const char *host, const char *name,
                                          const char *ip, bool secure,
                                          char *out, size_t cap)
{
    if (!out || !cap) { return false; }
    out[0] = '\0';
    if (!host || !name || !ip || !name[0] || strlen(name) > 63) { return false; }

    /* Only our exact configured .local name, with an optional standard port.
     * IP literals, provisioning addresses and reverse-proxy hosts stay intact.
     */
    const char *port = strchr(host, ':');
    if (port && strcmp(port, secure ? ":443" : ":80")) { return false; }
    size_t len = port ? (size_t)(port - host) : strlen(host);
    if (len && host[len - 1] == '.') { --len; }
    char expected[70];
    const int n = snprintf(expected, sizeof(expected), "%s.local", name);
    if (n <= 0 || (size_t)n != len) { return false; }
    for (size_t i = 0; i < len; ++i) {
        if (tolower((unsigned char)host[i]) != tolower((unsigned char)expected[i])) {
            return false;
        }
    }

    if (!panel_ip_address_ok(ip)) { return false; }
    const int size = snprintf(out, cap, "%s://%s/", secure ? "https" : "http", ip);
    if (size < 0 || (size_t)size >= cap) { out[0] = '\0'; return false; }
    return true;
}
