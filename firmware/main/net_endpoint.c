#include "net_endpoint.h"

#include <stdio.h>
#include <string.h>

#include "lwip/inet.h"
#include "lwip/sockets.h"

static bool ascii_space(char ch)
{
    return ch == ' ' || (ch >= '\t' && ch <= '\r');
}

static bool dns_name(char *host, size_t len)
{
    /* Canonicalize DNS names for stable equality and lwIP's .local lookup. */
    if (len > 0 && host[len - 1] == '.') { host[--len] = '\0'; }
    if (!len || len >= NET_HOST_SIZE) { return false; }
    size_t label = 0;
    bool nonnumeric = false;
    for (size_t i = 0; i < len; ++i) {
        char c = host[i];
        if (c >= 'A' && c <= 'Z') { c = host[i] = (char)(c + ('a' - 'A')); }
        if (c == '.') {
            if (!label || host[i - 1] == '-') { return false; }
            label = 0;
            continue;
        }
        const bool alpha = c >= 'a' && c <= 'z';
        const bool digit = c >= '0' && c <= '9';
        if (!alpha && !digit && c != '-') { return false; }
        if (!label && c == '-') { return false; }
        if (++label > 63) { return false; }
        nonnumeric |= !digit;
    }
    return nonnumeric && label && host[len - 1] != '-';
}

bool net_endpoint_parse(const char *text, uint16_t default_port, net_endpoint_t *out)
{
    if (!text || !out || !default_port) { return false; }
    while (ascii_space(*text)) { ++text; }
    const char *end = text + strlen(text);
    while (end > text && ascii_space(end[-1])) { --end; }
    if (end == text) { return false; }

    const bool bracketed = *text == '[';
    const char *host_end = end;
    const char *port_text = NULL;
    if (bracketed) {
        ++text;
        host_end = memchr(text, ']', (size_t)(end - text));
        if (!host_end) { return false; }
        if (host_end + 1 < end) {
            if (host_end[1] != ':') { return false; }
            port_text = host_end + 2;
        }
    } else {
        const char *colon = memchr(text, ':', (size_t)(end - text));
        if (colon && !memchr(colon + 1, ':', (size_t)(end - colon - 1))) {
            host_end = colon;
            port_text = colon + 1;
        }
    }
    const size_t len = (size_t)(host_end - text);
    if (!len || len > NET_HOST_SIZE ||
        (len == NET_HOST_SIZE && text[len - 1] != '.')) { return false; }
    net_endpoint_t parsed = { .port = default_port };
    const size_t copy = len == NET_HOST_SIZE ? len - 1 : len;
    memcpy(parsed.host, text, copy);
    if (port_text) {
        unsigned port = 0;
        for (const char *p = port_text; p < end; ++p) {
            if (*p < '0' || *p > '9') { return false; }
            port = port * 10U + (unsigned)(*p - '0');
            if (port > UINT16_MAX) { return false; }
        }
        if (!port) { return false; }
        parsed.port = (uint16_t)port;
    }

    struct in6_addr a6;
    struct in_addr a4;
    if (inet_pton(AF_INET6, parsed.host, &a6) == 1) {
        parsed.kind = NET_HOST_IPV6;
    } else if (bracketed) {
        return false;
    } else if (inet_pton(AF_INET, parsed.host, &a4) == 1) {
        parsed.kind = NET_HOST_IPV4;
    } else if (!dns_name(parsed.host, copy)) {
        return false;
    }
    *out = parsed;
    return true;
}

void net_endpoint_format(const net_endpoint_t *endpoint, char *out, size_t size)
{
    snprintf(out, size, endpoint->kind == NET_HOST_IPV6 ? "[%s]:%u" : "%s:%u",
             endpoint->host, (unsigned)endpoint->port);
}
