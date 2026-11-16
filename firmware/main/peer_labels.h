#pragma once
#include "np_access.h"
#include "esp_err.h"
#define PEER_LABEL_BYTES 96
esp_err_t peer_labels_init(void);
bool peer_labels_ready(void);
uint32_t peer_labels_revision(void);
void peer_labels_get(const np_access_record_t *ref, char out[PEER_LABEL_BYTES + 1]);
/* The caller supplies all current peers so departed labels can be reclaimed. */
esp_err_t peer_labels_set(const np_access_record_t *ref, const char *label, uint32_t expected,
                          const np_access_record_t *active, size_t count);
/* Bind an address-only label before the transport adopts a NAT port/address. */
void peer_labels_bind(const np_access_record_t *old_ref, const np_access_record_t *new_ref);
esp_err_t peer_labels_flush(void);
esp_err_t peer_labels_reset(void);
