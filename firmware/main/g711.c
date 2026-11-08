/*
 * g711 -- ITU-T G.711 mu-law companding for the intercom net payload.
 *
 * The base audio chain runs 16-bit PCM at 8 kHz internally.  The network
 * payload (UDP / MQTT) carries mu-law: one byte per sample, 8 KB/s per
 * device, phone-band quality -- the codec the public switched network has
 * used for fifty years.
 *
 * The encoder follows the classic Sun Microsystems / sox implementation
 * (also the basis of CPython's audioop module); the decoder is the standard
 * 256-entry reconstruction table, 16-bit output.
 *
 * Scale: input is full-range 16-bit PCM (the talk path is soft-limited well
 * before 0 dBFS); decode output is 16-bit PCM in the same range.
 */
#include "g711.h"

#define G711_BIAS   0x84
#define G711_CLIP   32635

static const int16_t seg_uend[8] = {
    0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF, 0x1FFF
};

/* 16-bit reconstruction of every mu-law code (index = code). */
static const int16_t ulaw2linear16[256] = {
    -32124,  -31100,  -30076,  -29052,  -28028,  -27004,  -25980,
    -24956,  -23932,  -22908,  -21884,  -20860,  -19836,  -18812,
    -17788,  -16764,  -15996,  -15484,  -14972,  -14460,  -13948,
    -13436,  -12924,  -12412,  -11900,  -11388,  -10876,  -10364,
    -9852,   -9340,   -8828,   -8316,   -7932,   -7676,   -7420,
    -7164,   -6908,   -6652,   -6396,   -6140,   -5884,   -5628,
    -5372,   -5116,   -4860,   -4604,   -4348,   -4092,   -3900,
    -3772,   -3644,   -3516,   -3388,   -3260,   -3132,   -3004,
    -2876,   -2748,   -2620,   -2492,   -2364,   -2236,   -2108,
    -1980,   -1884,   -1820,   -1756,   -1692,   -1628,   -1564,
    -1500,   -1436,   -1372,   -1308,   -1244,   -1180,   -1116,
    -1052,   -988,    -924,    -876,    -844,    -812,    -780,
    -748,    -716,    -684,    -652,    -620,    -588,    -556,
    -524,    -492,    -460,    -428,    -396,    -372,    -356,
    -340,    -324,    -308,    -292,    -276,    -260,    -244,
    -228,    -212,    -196,    -180,    -164,    -148,    -132,
    -120,    -112,    -104,    -96,     -88,     -80,     -72,
    -64,     -56,     -48,     -40,     -32,     -24,     -16,
    -8,      0,       32124,   31100,   30076,   29052,   28028,
    27004,   25980,   24956,   23932,   22908,   21884,   20860,
    19836,   18812,   17788,   16764,   15996,   15484,   14972,
    14460,   13948,   13436,   12924,   12412,   11900,   11388,
    10876,   10364,   9852,    9340,    8828,    8316,    7932,
    7676,    7420,    7164,    6908,    6652,    6396,    6140,
    5884,    5628,    5372,    5116,    4860,    4604,    4348,
    4092,    3900,    3772,    3644,    3516,    3388,    3260,
    3132,    3004,    2876,    2748,    2620,    2492,    2364,
    2236,    2108,    1980,    1884,    1820,    1756,    1692,
    1628,    1564,    1500,    1436,    1372,    1308,    1244,
    1180,    1116,    1052,    988,     924,     876,     844,
    812,     780,     748,     716,     684,     652,     620,
    588,     556,     524,     492,     460,     428,     396,
    372,     356,     340,     324,     308,     292,     276,
    260,     244,     228,     212,     196,     180,     164,
    148,     132,     120,     112,     104,     96,      88,
    80,      72,      64,      56,      48,      40,      32,
    24,      16,      8,       0
};

/*
 * Find the segment: the first table entry that is >= val.
 * seg_uend[] is ascending.
 */
static int search_uend(int16_t val)
{
    for (int i = 0; i < 8; i++) {
        if (val <= seg_uend[i]) {
            return i;
        }
    }
    return 8;
}

/* 14-bit signed input (callers shift 16-bit PCM down by 2). */
static uint8_t linear14_2_ulaw(int16_t pcm_val)
{
    int16_t v = pcm_val;
    uint8_t mask;

    if (v < 0) {
        v = -v;
        mask = 0x7F;            /* negative values complement the code */
    } else {
        mask = 0xFF;
    }
    if (v > G711_CLIP) {
        v = G711_CLIP;
    }
    v += (G711_BIAS >> 2);      /* 0x84 >> 2 = 33 */

    int seg = search_uend(v);
    if (seg >= 8) {
        return (uint8_t)(0x7F ^ mask);   /* out of range: max value */
    }
    uint8_t uval = (uint8_t)((seg << 4) | ((v >> (seg + 1)) & 0xF));
    return (uint8_t)(uval ^ mask);
}

uint8_t g711_ulaw_encode(int16_t pcm)
{
    return linear14_2_ulaw((int16_t)(pcm >> 2));
}

int16_t g711_ulaw_decode(uint8_t code)
{
    return ulaw2linear16[code & 0xFF];
}

size_t g711_ulaw_encode_buf(const int16_t *pcm, uint8_t *codes, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        codes[i] = g711_ulaw_encode(pcm[i]);
    }
    return n;
}

size_t g711_ulaw_decode_buf(const uint8_t *codes, int16_t *pcm, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        pcm[i] = g711_ulaw_decode(codes[i]);
    }
    return n;
}
