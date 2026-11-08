/*
 * g711 -- ITU-T G.711 mu-law companding for the intercom net payload.
 *
 * The base audio chain runs 16-bit PCM at 8 kHz internally.  The network
 * payload (UDP / MQTT) carries mu-law: one byte per sample, 8 KB/s per
 * device, phone-band quality -- the same codec the public switched network
 * has used for fifty years, which keeps the encoder/decoder to a few lines
 * with no tables.
 *
 * Scale: input samples are assumed to be full-range 16-bit PCM (the talk
 * path is already soft-limited well before 0 dBFS).  Decode output is
 * 16-bit PCM in the same range.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Encode one 16-bit PCM sample to a mu-law code (0..255). */
uint8_t g711_ulaw_encode(int16_t pcm);

/* Decode one mu-law code (0..255) to a 16-bit PCM sample. */
int16_t g711_ulaw_decode(uint8_t code);

/*
 * Encode/decode a whole buffer in place-safe fashion.  `n` is the number of
 * samples.  Returns n.  (src and dst may be the same buffer only when the
 * directions do not alias -- they are different types in practice.)
 */
size_t g711_ulaw_encode_buf(const int16_t *pcm, uint8_t *codes, size_t n);
size_t g711_ulaw_decode_buf(const uint8_t *codes, int16_t *pcm, size_t n);

#ifdef __cplusplus
}
#endif
