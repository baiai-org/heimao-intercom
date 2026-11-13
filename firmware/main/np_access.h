/* Persistent P2P admission policy. IDs are transport identities, not cryptographic credentials. */
#pragma once
#include "np_peer_store.h"
#define NP_DENY_LIMIT 64
#define NP_ACCESS_BYTES (4 + (NP_PEER_LIMIT + NP_DENY_LIMIT) * 33)
typedef struct {
    np_peer_record_t peer;
    uint8_t mac[6];
} np_access_record_t;
typedef struct {
    size_t peer_count, denied_count;
    np_access_record_t peers[NP_PEER_LIMIT], denied[NP_DENY_LIMIT];
} np_access_state_t;
bool np_access_has_mac(const uint8_t mac[6]);
bool np_access_match(const np_access_record_t *r, const uint8_t mac[6],
                     uint8_t family, const uint8_t address[16], uint16_t port);
void np_access_id(const np_access_record_t *r, char *out, size_t n);
size_t np_access_encode(const np_access_state_t *s, uint8_t *out, size_t cap);
bool np_access_decode(const uint8_t *data, size_t n, np_access_state_t *s);
