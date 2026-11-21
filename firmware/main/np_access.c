#include "np_access.h"
#include <stdio.h>
#include <string.h>

/* UI/storage identity for a configured hostname and port. Packet admission
 * continues to use the authenticated peer identity and transport address. */
uint64_t np_access_dns_key(const char *host, uint16_t port)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (; *host; ++host) { hash = (hash ^ (uint8_t)*host) * UINT64_C(1099511628211); }
    hash = (hash ^ (uint8_t)(port >> 8)) * UINT64_C(1099511628211);
    hash = (hash ^ (uint8_t)port) * UINT64_C(1099511628211);
    return hash ? hash : 1;
}
bool np_access_has_mac(const uint8_t mac[6])
{
    if (!mac) { return false; }
    for (int i = 0; i < 6; ++i) { if (mac[i]) { return true; } }
    return false;
}
bool np_access_match(const np_access_record_t *r, const uint8_t mac[6],
                     uint8_t family, const uint8_t address[16], uint16_t port)
{
    /* A known identity survives NAT/address changes. Never widen to a shared IP. */
    if (np_access_has_mac(r->mac)) {
        return np_access_has_mac(mac) && memcmp(r->mac, mac, 6) == 0;
    }
    return address && port && r->peer.port == port && r->peer.family == family &&
        memcmp(r->peer.address, address, family == 4 ? 4 : 16) == 0;
}
void np_access_id(const np_access_record_t *r, char *out, size_t n)
{
    if (!n) { return; }
    if (r->dns_key) {
        snprintf(out, n, "d-%016llx", (unsigned long long)r->dns_key);
    } else if (np_access_has_mac(r->mac)) {
        snprintf(out, n, "m-%02x%02x%02x%02x%02x%02x", r->mac[0], r->mac[1],
                 r->mac[2], r->mac[3], r->mac[4], r->mac[5]);
    } else {
        size_t off = (size_t)snprintf(out, n, "a-%u-", r->peer.family);
        for (int i = 0; i < (r->peer.family == 4 ? 4 : 16) && off + 2 < n; ++i) {
            off += (size_t)snprintf(out + off, n - off, "%02x", r->peer.address[i]);
        }
        if (off < n) { snprintf(out + off, n - off, "-%u", r->peer.port); }
    }
}
static bool valid(const np_access_record_t *r, bool denied)
{
    if (!r->peer.name[0] || !memchr(r->peer.name, 0, 8)) { return false; }
    if (r->dns_key) { return true; }
    if (denied && np_access_has_mac(r->mac)) { return true; } /* Indirect Hub origin. */
    if (!r->peer.port || (r->peer.family != 4 && r->peer.family != 6)) { return false; }
    for (int i = 0; i < (r->peer.family == 4 ? 4 : 16); ++i) {
        if (r->peer.address[i]) { return true; }
    }
    return false;
}
size_t np_access_encode(const np_access_state_t *s, uint8_t *out, size_t cap)
{
    if (!s || !out || s->peer_count > NP_PEER_LIMIT || s->denied_count > NP_DENY_LIMIT) { return 0; }
    const size_t count = s->peer_count + s->denied_count;
    bool v2 = false;
    size_t host_bytes = 0;
    for (size_t i = 0; i < s->peer_count; ++i) {
        if (!s->peers[i].dns_key) { continue; }
        const np_access_host_t *host = &s->hosts[i];
        const char *end = memchr(host->host, 0, NET_HOST_SIZE);
        if (!end || end == host->host || !host->port ||
            np_access_dns_key(host->host, host->port) != s->peers[i].dns_key) { return 0; }
        host_bytes += 3 + (size_t)(end - host->host);
        v2 = true;
    }
    for (size_t i = 0; i < s->denied_count; ++i) { v2 |= s->denied[i].dns_key != 0; }
    const size_t width = v2 ? 41 : 33;
    const size_t base = 4 + count * width;
    const size_t size = base + host_bytes;
    if (size > cap) { return 0; }
    memset(out, 0, size); out[0] = v2 ? 2 : 1; out[1] = (uint8_t)s->peer_count;
    out[2] = (uint8_t)s->denied_count; out[3] = (uint8_t)width;
    size_t offset = base;
    for (size_t i = 0; i < count; ++i) {
        const np_access_record_t *r = i < s->peer_count ? &s->peers[i] : &s->denied[i - s->peer_count];
        if (!valid(r, i >= s->peer_count)) { return 0; }
        uint8_t *p = out + 4 + i * width;
        memcpy(p, r->peer.name, 8); p[8] = r->peer.family; memcpy(p + 9, r->peer.address, 16);
        p[25] = (uint8_t)(r->peer.port >> 8); p[26] = (uint8_t)r->peer.port;
        memcpy(p + 27, r->mac, 6);
        if (v2) {
            for (unsigned b = 0; b < 8; ++b) { p[33 + b] = (uint8_t)(r->dns_key >> (8 * b)); }
        }
        if (r->dns_key && i < s->peer_count) {
            const np_access_host_t *host = &s->hosts[i];
            const size_t len = strlen(host->host);
            out[offset++] = (uint8_t)len;
            out[offset++] = (uint8_t)(host->port >> 8);
            out[offset++] = (uint8_t)host->port;
            memcpy(out + offset, host->host, len);
            offset += len;
        }
    }
    return size;
}
bool np_access_decode(const uint8_t *data, size_t n, np_access_state_t *s)
{
    if (!data || !s || n < 4 || (data[0] != 1 && data[0] != 2) ||
        data[1] > NP_PEER_LIMIT || data[2] > NP_DENY_LIMIT) { return false; }
    const bool v2 = data[0] == 2;
    const size_t width = v2 ? 41 : 33;
    const size_t count = (size_t)data[1] + data[2];
    size_t offset = 4 + count * width;
    if (data[3] != width || offset > n) { return false; }
    memset(s, 0, sizeof(*s)); s->peer_count = data[1]; s->denied_count = data[2];
    for (size_t i = 0; i < count; ++i) {
        np_access_record_t *r = i < s->peer_count ? &s->peers[i] : &s->denied[i - s->peer_count];
        const uint8_t *p = data + 4 + i * width;
        memcpy(r->peer.name, p, 8); r->peer.family = p[8]; memcpy(r->peer.address, p + 9, 16);
        r->peer.port = (uint16_t)((p[25] << 8) | p[26]); memcpy(r->mac, p + 27, 6);
        if (v2) {
            for (unsigned b = 0; b < 8; ++b) { r->dns_key |= (uint64_t)p[33 + b] << (8 * b); }
        }
        if (!valid(r, i >= s->peer_count)) { return false; }
        if (r->dns_key && i < s->peer_count) {
            if (n - offset < 3) { return false; }
            const size_t len = data[offset++];
            np_access_host_t *host = &s->hosts[i];
            host->port = (uint16_t)((data[offset] << 8) | data[offset + 1]);
            offset += 2;
            if (!host->port || !len || len >= NET_HOST_SIZE || n - offset < len ||
                memchr(data + offset, 0, len)) { return false; }
            memcpy(host->host, data + offset, len);
            if (np_access_dns_key(host->host, host->port) != r->dns_key) { return false; }
            offset += len;
        }
    }
    return offset == n;
}
