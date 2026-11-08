/*
 * voice_tail -- end-of-transmission tail tones (global, all intercom modes).
 *
 * When a TALK ends, the transmitter stays keyed for a short while and plays a
 * configured "tail tone" into the TX path, so the tone is actually transmitted
 * (local: out the SA818 to the air; network: published as ordinary audio
 * frames).  This is the classic amateur-radio courtesy signal: K-CHOO at the
 * end of a transmission means "over to you".  Icom calls the same feature a
 * "courtesy tone"; Baofeng has a "TAIL BEEP" option.
 *
 * Built-in tones are synthesized at init (no storage):
 *   - single beep (the Baofeng-style TAIL BEEP)
 *   - K-CHOO   Morse letter K  -.-.   "go ahead, any station" (the classic)
 *   - brand classics (researched from manuals / firmware, see voice_tail.c):
 *       Baofeng ROGER, Motorola MDC-1200, Icom go-ahead * / #,
 *       Quansheng UV-K5 double beep
 *
 * A custom tone can be uploaded from the web panel as a WAV file (16-bit PCM,
 * mono or stereo, any sample rate 4.8..96 kHz) or an MP3 file (MPEG-1/2
 * Layer III, mono or stereo, 8..48 kHz; ID3 tags tolerated).  It is converted
 * on the board: resampled to 8 kHz mono, amplitude-normalised, truncated to
 * 5 s, mu-law encoded (8 KB/s) and stored in the 1 MB "storage" SPIFFS
 * partition.
 *
 * Preview: any DAC owner (local monitor task, or the network engines'
 * playback task) substitutes the preview samples for its own audio while the
 * preview is armed, so the preview never fights the single-owner rule.  In
 * local mode with no voice path running, the web panel borrows the monitor
 * task for the tone's duration (settle fades skipped) so the preview is
 * always audible.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    TAIL_OFF = 0,
    TAIL_BEEP1,           /* 1: 880 Hz, 150 ms */
    TAIL_KCHOO,           /* 2: Morse K, the classic "over to you" */
    TAIL_ROGER,           /* 3: Baofeng UV-5R menu 39 "ROGER": 880 Hz beep at
                             the end of each transmission */
    TAIL_MDC,             /* 4: Motorola MDC-1200 PTT-ID: 1200-baud FFSK,
                             CCIR tones mark 1200 Hz / space 1800 Hz */
    TAIL_ICOM_STAR,       /* 5: Icom IDAS "go ahead" courtesy tone: DTMF *
                             (941 Hz) -- analog repeater implementation */
    TAIL_ICOM_HASH,       /* 6: Icom IDAS "go ahead": DTMF # (941 + 1336 Hz) */
    TAIL_K5X2,            /* 7: Quansheng UV-K5 tail-style: 880 Hz, 60 ms x2
                             (from the UV-K5 firmware beep table) */
    TAIL_CUSTOM,          /* 8: uploaded WAV/MP3, last in the panel select */
    TAIL_COUNT
};

#define TAIL_UPLOAD_MAX         (512 * 1024)  /* largest accepted upload */
#define TAIL_CUSTOM_MAX_SAMPLES (8000 * 5)    /* 5 s at 8 kHz */

/* Which communication architecture receives the configured tail.  The bits
 * intentionally match NETCOM_MODE_* without making this small audio module
 * depend on the network manager. */
#define TAIL_MODE_LOCAL   (1u << 0)   /* MODE 00 */
#define TAIL_MODE_NETP2P  (1u << 1)   /* MODE 01 */
#define TAIL_MODE_RELAY   (1u << 2)   /* MODE 02 */
#define TAIL_MODE_ALL     (TAIL_MODE_LOCAL | TAIL_MODE_NETP2P | TAIL_MODE_RELAY)

/* Mount the storage partition, load the selection.  Idempotent. */
void voice_tail_init(void);

const char *tail_name(int id);

int tail_get(void);
esp_err_t tail_set(int id);          /* persist + activate */
size_t tail_samples(int id);         /* total 8 kHz samples (0 if empty) */

/* Persisted mode mask.  A zero mask is rejected; use TAIL_OFF for a global
 * disable.  Missing/invalid NVS values load as TAIL_MODE_ALL for compatibility
 * with firmware that predates per-mode selection. */
uint8_t tail_modes_get(void);
esp_err_t tail_modes_set(int modes);
bool tail_should_play(int mode);

/*
 * Fetch up to n 8 kHz samples at absolute position pos.  Returns the number
 * written (0 once pos is at or past the end).
 */
size_t tail_fetch(int id, size_t pos, int16_t *buf, size_t n);

/* Store an uploaded WAV or MP3 (see the file header for accepted formats). */
esp_err_t tail_custom_store(const uint8_t *file, size_t len);
esp_err_t tail_custom_delete(void);
size_t tail_custom_samples(void);    /* 0 = none stored */

/* Preview: play id through whichever DAC owner is active right now. */
void tail_preview_arm(int id);       /* id 0..TAIL_COUNT-1 */
void tail_preview_cancel(void);
bool tail_preview_active(void);
/*
 * DAC-owner hook: fetch the next preview slice, or 0 when the preview is not
 * armed (or has finished, which also clears the state).
 */
size_t tail_preview_fetch(int16_t *buf, size_t n);

#ifdef __cplusplus
}
#endif
