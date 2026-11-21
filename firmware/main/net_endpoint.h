#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A DNS name without the optional root dot is at most 253 bytes. */
#define NET_HOST_SIZE 254
#define NET_ENDPOINT_SIZE (NET_HOST_SIZE + 8)

typedef enum {
    NET_HOST_DNS = 0,
    NET_HOST_IPV4 = 4,
    NET_HOST_IPV6 = 6,
} net_host_kind_t;

typedef struct {
    char host[NET_HOST_SIZE];
    uint16_t port;
    net_host_kind_t kind;
} net_endpoint_t;

bool net_endpoint_parse(const char *text, uint16_t default_port, net_endpoint_t *out);
void net_endpoint_format(const net_endpoint_t *endpoint, char *out, size_t size);
