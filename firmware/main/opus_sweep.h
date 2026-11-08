#pragma once

#include <stddef.h>
#include <stdint.h>

/* Encode/decode a synthetic voiced signal across frame sizes, bitrates and
 * complexity settings, and report segmental SNR for each.  Runs in its own
 * PSRAM-stacked task (Opus needs the stack). */
const char *opus_sweep_task(char *out, size_t n, uint32_t timeout_ms);
