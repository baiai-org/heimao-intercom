/*
 * opus_link -- the Opus codec as this firmware uses it.
 *
 * One configuration for both network paths (P2P/UDP and the MQTT relay):
 *
 *   sample rate 12000 Hz   the existing audio pipeline; Opus codes 12 kHz
 *                          natively, so no resampling anywhere
 *   frame       40 ms      the relay is sensitive to message rate, and 25
 *                          packets/s carries the same audio the old 29 ms /
 *                          348-sample framing did
 *   bitrate     24 kbit/s  CBR: 120 bytes per frame, 3.0 KB/s of payload
 *   application VOIP       speech-optimised; the RF channel only carries
 *                          ~3 kHz, so this is transparent for our source
 *   complexity  2          balance on a 240 MHz core without an FPU
 *   VBR         off        constant packet size keeps the framing and the
 *                          jitter buffer simple, and the link budget predictable
 *   DTX / FEC   off        DTX would fight the bridge's own carrier discipline;
 *                          in-band FEC costs ~30% and only pays off once we see
 *                          real loss
 *
 * The codec state and Opus' 120 KB pseudostack are allocated from PSRAM by the
 * micro-opus component (see its Kconfig), so internal RAM is unaffected.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define OPUS_LINK_RATE_HZ    16000
#define OPUS_LINK_FRAME_MS   20
#define OPUS_LINK_SAMPLES    (OPUS_LINK_RATE_HZ * OPUS_LINK_FRAME_MS / 1000)  /* 320 */
#define OPUS_LINK_BITRATE    32000
/* CBR 24 kbit/s x 40 ms = 120 bytes; leave room for the encoder overshooting. */
#define OPUS_LINK_MAX_BYTES  256

typedef struct opus_link_enc opus_link_enc_t;
typedef struct opus_link_dec opus_link_dec_t;

/* Returns NULL if the encoder could not be created (logs why). */
opus_link_enc_t *opus_link_enc_new(void);
void opus_link_enc_free(opus_link_enc_t *enc);
/* pcm: OPUS_LINK_SAMPLES mono samples.  Returns bytes written, or -1. */
int opus_link_encode(opus_link_enc_t *enc, const int16_t *pcm,
                     uint8_t *out, size_t cap);

opus_link_dec_t *opus_link_dec_new(void);
void opus_link_dec_free(opus_link_dec_t *dec);
/* data/len from the wire; pcm must hold OPUS_LINK_SAMPLES.  Returns samples
 * produced (OPUS_LINK_SAMPLES), or -1.  len == 0 asks the decoder to conceal a
 * lost frame, which is why the relay can ride out a dropped packet. */
int opus_link_decode(opus_link_dec_t *dec, const uint8_t *data, size_t len,
                     int16_t *pcm);

/* Encode+decode a synthetic voice-like signal and report the cost.
 * Writes a human-readable line into out; returns it. */
const char *opus_link_bench(char *out, size_t n);
/* Same benchmark, run in a PSRAM-stacked task (Opus needs the stack). */
const char *opus_link_bench_task(char *out, size_t n, uint32_t timeout_ms);
