/*
 * G.722 (64 kbit/s, mode 1) -- public domain, see g722.c for the notice.
 *
 * One call handles a whole frame: 348 samples in -> 174 bytes out at 12 kHz.
 * At a 12 kHz sample rate the codec's quadrature-mirror split lands on 3 kHz,
 * so the 6-bit sub-band carries exactly the voice the radio transmits and the
 * 2-bit sub-band only the (mostly empty) 3-6 kHz region.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    G722_DEFAULT = 0x0000,
    G722_SAMPLE_RATE_8000 = 0x0001,
    G722_PACKED = 0x0002
};

typedef struct g722_encode_state G722_ENC_CTX;
typedef struct g722_decode_state G722_DEC_CTX;

/* rate: 64000 (8 bits per sample pair), 56000 or 48000.  options: 0. */
G722_ENC_CTX *g722_encoder_new(int rate, int options);
int g722_encoder_destroy(G722_ENC_CTX *s);
/* amp: len PCM samples (len must be even).  Returns bytes written. */
int g722_encode(G722_ENC_CTX *s, const int16_t amp[], int len, uint8_t g722_data[]);

G722_DEC_CTX *g722_decoder_new(int rate, int options);
int g722_decoder_destroy(G722_DEC_CTX *s);
/* len: bytes in.  Returns samples written (2 per byte). */
int g722_decode(G722_DEC_CTX *s, const uint8_t g722_data[], int len, int16_t amp[]);

#ifdef __cplusplus
}
#endif
