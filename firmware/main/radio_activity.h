#pragma once
#include <stddef.h>
#include <stdint.h>

#define RADIO_ACTIVITY_JSON_SIZE 512

/* Initialise once before HTTP/HTTPS start. Snapshots are read-only: no NVS,
 * meter drains, audio-ring access, socket writes, or control-lease changes. */
void radio_activity_init(void);
/* Writes one complete JSON snapshot; returns a signature of its state fields
 * (excluding timestamp). Caller sends it only from its owning HTTPD task. */
uint32_t radio_activity_json(char *buf, size_t n);
