/* Browser audio: PCM16 LE mono at AUDIO_SAMPLE_RATE_HZ (16 kHz).
 * A page opens /api/sdr?on=1&client=<nonce> and receives sid. All subsequent
 * requests carry sid. Each press uses a strictly increasing tx ID; release
 * and PCM carry the same ID. WS PCM starts with LE uint32 sid, uint32 tx,
 * followed by at most 2040 PCM bytes. Text control is ptt1:sid:tx:request
 * or ptt0:sid:tx:request. Replies/status include sid, tx, owner and reason.
 * No transport or audio thread sends asynchronously into an HTTPD socket. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Allocate the audio rings (PSRAM); call once at boot. */
void sdr_init(void);

/* Enable/disable the web-audio mode (persists nothing; the panel owns the
 * switch).  Enabling borrows the monitor task, disabling releases it and
 * unkeys the bridge domain if we were transmitting. */
esp_err_t sdr_set_enabled(bool on);
bool      sdr_enabled(void);

/* WebSocket session bookkeeping, called from the /ws/sdr handler.
 *
 * Only ONE browser can listen at a time: the radio audio comes from a single
 * ring, so a second listener would silently get half of every sentence.  The
 * attach call therefore refuses a second client, and only takes over once the
 * attached fd has been silent long enough to be considered gone (half-open
 * socket, crashed tab) -- otherwise a dead session would lock the stream out
 * until reboot.
 */
bool sdr_ws_attach(httpd_handle_t server, int fd, uint32_t sid);
bool sdr_ws_owner(httpd_handle_t server, int fd, uint32_t sid);
void sdr_ws_detach(httpd_handle_t server, int fd, uint32_t sid);
/* Refresh the attached client's liveness; called for every frame it sends. */
bool sdr_client_open(uint32_t client);
/* force=true may abort a live transmission (explicit stop/OTA); a normal close
 * cancels a pending tail but refuses while the PTT is still held. */
bool sdr_client_close(uint32_t sid, bool force);
uint32_t sdr_current_session(void);
bool sdr_session_valid(uint32_t sid);
void sdr_state_json(char *buf, size_t n, bool accepted, uint32_t request);
/* True while a live WebSocket listener owns the stream (the HTTP poller uses
 * this to stay out of the way). */
bool sdr_ws_client_attached(void);

/* Browser microphone audio (raw little-endian PCM16 payload). */
bool sdr_tx_push_bytes(uint32_t sid, uint32_t tx, const uint8_t *bytes, size_t len);

/* RF-audio tap, called from the monitor task once per output block.
 * `enabled` is false for audio that must not leave the board (module squelch
 * closed, a previewed tail tone, or the audio we ourselves injected). A close
 * edge flushes queued RF samples only, leaving remote network audio intact. */
void sdr_rx_push(const int16_t *pcm, size_t n, bool enabled);
/* Remote decoded PCM before local RF processing. RF mute cannot close NET. */
void sdr_rx_network_push(uint8_t mode, const int16_t *pcm, size_t n);
/* Called by the mode manager before starting/stopping network engines. */
void sdr_rx_network_mode(uint8_t mode);

/*
 * Drain up to `n` samples of radio audio for the WebSocket handler.  The
 * handler sends these from its own (httpd) task in reply to each browser
 * frame: the server has one task per listener, and sending from another task
 * used to write WebSocket frames into whatever new session reused the old
 * socket descriptor once a client vanished -- which corrupted the TLS stream
 * of an unrelated request ("WRONG_VERSION_NUMBER", empty bodies).
 */
size_t sdr_rx_take(int16_t *out, size_t n);

/*
 * Explicit PTT from the page: "hold to talk" keys the radio on the press and
 * releases it on the release, exactly like the hardware button.  The old
 * design inferred the key from the microphone level, which chopped the first
 * syllable off every sentence and dropped the carrier during pauses -- the
 * far end heard a stuttering, broken transmission.
 */
bool sdr_ptt_request(uint32_t sid, uint32_t tx, bool on);

/* Key the radio and transmit `freq_hz` for ~3 s, generated on the board
 * (no network involved): the listening test that isolates the local audio
 * chain from the browser transport. */
void sdr_play_test_tone(int freq_hz);

/*
 * Count radio-audio handed to the browser.  `samples` is what this call took
 * from the ring: a take can be shorter than a packet, so counting calls would
 * overstate the packet total (the panel shows it as "RX 包").
 */
void sdr_note_rx_sent(size_t samples);
/* Count an audio frame received from the page (diagnostics). */
void sdr_note_ws_frame(size_t bytes);
/*
 * Count a radio-audio frame the handler could NOT hand to the socket (the WS
 * send returned an error, typically the TLS/TCP send buffer being full on a
 * slow link).  Those samples are lost on the board, not on the wire, so this
 * counter is what separates "the link is slow" from "the board dropped it".
 */
void sdr_note_ws_send_err(void);

/* "\"sdr\":{...}" fragment for /api/status (no leading comma). */
void sdr_status_json(char *buf, size_t n);

#ifdef __cplusplus
}
#endif
