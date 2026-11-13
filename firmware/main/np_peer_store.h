/* Portable/versioned peer configuration; legacy v1/v2 keep their byte order. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define NP_PEER_LIMIT 16
#define NP_PEER_STORE_BYTES (4 + NP_PEER_LIMIT * 27)
typedef struct { char name[8]; uint8_t family, address[16]; uint16_t port; } np_peer_record_t;
bool np_peer_store_decode(const uint8_t *data, size_t len, unsigned version,
                          np_peer_record_t *records, size_t *count);
size_t np_peer_store_encode(const np_peer_record_t *records, size_t count, uint8_t *out, size_t cap);
