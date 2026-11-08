/* Browser PCM16 LE, mono, 16 kHz. RX/TX rings live in PSRAM.
 * PTT is explicit and independent of PCM amplitude. A page session and a
 * monotonic transmission ID isolate delayed HTTP/WS traffic. See sdr.h. */
#include "sdr.h"
#include "audio_i2s.h"   /* AUDIO_SAMPLE_RATE_HZ / AUDIO_PKT_SAMPLES */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"   /* xTaskCreateWithCaps */

#include "ptt.h"
#include "voice.h"
#include "netcom.h"

static const char *TAG = "sdr";

#define SDR_PKT_SAMPLES     AUDIO_PKT_SAMPLES /* one 20 ms stream packet */
#define SDR_PKT_MS          AUDIO_PKT_MS
#define SDR_RX_RING_SAMPLES (AUDIO_SAMPLE_RATE_HZ * 3)
#define SDR_TX_RING_SAMPLES 8192
#define SDR_TX_STALL_MS     2000    /* silence after the first PCM block */
#define SDR_NO_PCM_GRACE_MS 3000    /* PTT keyed but no PCM yet (mic/ACK slow) */
#define SDR_PTT_HOLD_MAX_MS 30000
#define SDR_WS_STALE_MS     6000
/* A release must not hold the carrier longer than this.  A custom tail can be
 * up to 5 s, so the old 1.2 s cap cut .198's 1.35 s clip mid-tone and left
 * tail_timeout; the cap is now a safety net, not the tone length. */
#define SDR_RELEASE_MAX_US  6000000
#define SDR_RELEASE_NOTONE_US 600000
/* A session with no live client at all (no WebSocket, no HTTP poll, no
 * control traffic) for this long is orphaned: its listener process is gone
 * and only it could release the engine.  Heartbeats and polls refresh
 * s_client_us, so an active listener is never mistaken for an orphan. */
#define SDR_NO_CLIENT_STALE_MS 30000

static volatile bool s_on;
static bool s_ptt_hold;
static bool s_releasing;
static bool s_tail_started;
static bool s_ws_dead;
/* Latched for one browser transmission.  A mode/config change made while the
 * button is held must not move an already-running stream between RF and net. */
static bool s_net_tx;
static bool s_rf_tx;
static int64_t s_release_started_us;
static int64_t s_ptt_started_us, s_pcm_us, s_client_us, s_ws_last_us;
static uint32_t s_session, s_client, s_tx_id;
static const char *s_end_reason = "idle";
/* Short-press diagnosis: remember how the last over ended. */
static bool s_pcm_ever;
static char s_last_reason[24] = "none";
static uint32_t s_last_tx_ms;
static uint32_t s_last_rel_ms;
static bool s_last_had_pcm;
static bool s_last_tail;
/* Counters for the short-press regression: total keyed releases, releases
 * that played the tail, and the two no-tail causes we care about. */
static uint32_t s_rel_total, s_rel_tail, s_rel_notail;
static uint32_t s_rel_pcm_timeout, s_rel_force;
static uint32_t s_rel_tail_timeout, s_rel_released_notail;
static uint32_t s_begin_busy, s_owner_mismatch;
/* PTT request counters: prove the rapid-press lockup is gone. */
static uint32_t s_off_stale, s_off_reject, s_on_reject;
static bool          s_inited;

static int16_t *s_rx_ring;
static int16_t *s_tx_ring;
static volatile size_t s_rx_head, s_rx_tail;
static volatile size_t s_tx_head, s_tx_tail;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
/* Whether the current monitor block is allowed to enter the browser RX ring.
 * A close edge drops already queued board-side audio so a stale hiss cannot be
 * sent after the module squelch has closed. */
static bool s_rx_gate_known;
static bool s_rx_gate_open;

static httpd_handle_t s_server;
static volatile int   s_ws_fd = -1;

static TaskHandle_t s_tx_task;
static TaskHandle_t s_tone_task;
static StaticSemaphore_t s_control_storage, s_key_storage;
static SemaphoreHandle_t s_control, s_key;
static bool s_monitor_borrowed;

static volatile uint32_t s_rx_pkts;    /* whole stream packets delivered */
static volatile uint32_t s_rx_part;    /* < packet samples carried over */
static volatile uint64_t s_rx_smp;     /* exact samples delivered */
static volatile uint32_t s_tx_pkts;
static volatile uint32_t s_tx_dry;
static volatile uint32_t s_tx_samples;
static volatile uint32_t s_ws_frames;   /* audio frames from the page */
static volatile uint32_t s_ws_bytes;
static volatile uint32_t s_rx_drop;     /* samples thrown away because the RX
                                         * ring was full (a slow/stalled link
                                         * that the handler could not drain) */
static volatile uint32_t s_ws_send_err; /* frames the socket refused */
/* Browser TX level diagnostics: maxima for the current SDR session. */
static volatile uint32_t s_tx_raw_peak;
static volatile uint32_t s_tx_processed_peak;
static volatile uint32_t s_tx_processed_clip;

/* ------------------------------------------------------------------ rings */

void sdr_init(void)
{
    if (!s_control) {
        s_control = xSemaphoreCreateRecursiveMutexStatic(&s_control_storage);
        s_key = xSemaphoreCreateMutexStatic(&s_key_storage);
    }
    if (s_inited) {
        return;
    }
    s_rx_ring = heap_caps_malloc(SDR_RX_RING_SAMPLES * sizeof(int16_t),
                                 MALLOC_CAP_SPIRAM);
    s_tx_ring = heap_caps_malloc(SDR_TX_RING_SAMPLES * sizeof(int16_t),
                                 MALLOC_CAP_SPIRAM);
    if (!s_rx_ring || !s_tx_ring) {
        ESP_LOGE(TAG, "audio rings: out of PSRAM");
        heap_caps_free(s_rx_ring);
        heap_caps_free(s_tx_ring);
        s_rx_ring = s_tx_ring = NULL;
        return;
    }
    s_rx_head = s_rx_tail = 0;
    s_tx_head = s_tx_tail = 0;
    s_session = esp_random();
    if (!s_session) { s_session = 1; }
    s_inited = true;
    ESP_LOGI(TAG, "rings: rx %u / tx %u samples in PSRAM",
             (unsigned)SDR_RX_RING_SAMPLES, (unsigned)SDR_TX_RING_SAMPLES);
}

/* Append to a ring; when it is full the OLDEST audio is dropped so the
 * backlog (and therefore the latency) stays bounded. */
static void ring_put(int16_t *ring, size_t cap, volatile size_t *head,
                     volatile size_t *tail, const int16_t *pcm, size_t n,
                     volatile uint32_t *drop_ctr)
{
    if (!ring || !pcm || n == 0 || n > cap) {
        return;
    }
    portENTER_CRITICAL(&s_mux);
    size_t used = *head - *tail;
    if (used + n > cap) {
        const size_t drop = used + n - cap;
        *tail += drop;              /* drop oldest */
        if (drop_ctr) {
            *drop_ctr += (uint32_t)drop;
        }
    }
    for (size_t i = 0; i < n; ++i) {
        ring[(*head) % cap] = pcm[i];
        (*head)++;
    }
    portEXIT_CRITICAL(&s_mux);
}

/* Take up to n samples; returns how many were copied (0 when empty). */
static size_t ring_take(int16_t *ring, size_t cap, volatile size_t *head,
                        volatile size_t *tail, int16_t *out, size_t n)
{
    if (!ring || !out || n == 0) {
        return 0;
    }
    portENTER_CRITICAL(&s_mux);
    size_t used = *head - *tail;
    if (used > n) {
        used = n;
    }
    for (size_t i = 0; i < used; ++i) {
        out[i] = ring[(*tail) % cap];
        (*tail)++;
    }
    portEXIT_CRITICAL(&s_mux);
    return used;
}

/* ------------------------------------------------------------- rx (board) */

void sdr_rx_push(const int16_t *pcm, size_t n, bool enabled)
{
    if (!s_on || !enabled) {
        if (s_on && !enabled) {
            portENTER_CRITICAL(&s_mux);
            if (!s_rx_gate_known || s_rx_gate_open) {
                s_rx_tail = s_rx_head;
            }
            s_rx_gate_known = true;
            s_rx_gate_open = false;
            portEXIT_CRITICAL(&s_mux);
        }
        return;
    }
    portENTER_CRITICAL(&s_mux);
    s_rx_gate_known = true;
    s_rx_gate_open = true;
    portEXIT_CRITICAL(&s_mux);
    ring_put(s_rx_ring, SDR_RX_RING_SAMPLES, &s_rx_head, &s_rx_tail, pcm, n,
             &s_rx_drop);
}

size_t sdr_rx_take(int16_t *out, size_t n)
{
    return ring_take(s_rx_ring, SDR_RX_RING_SAMPLES, &s_rx_head, &s_rx_tail,
                     out, n);
}

void sdr_note_rx_sent(size_t samples)
{
    s_rx_smp += (uint64_t)samples;
    s_rx_part += (uint32_t)samples;
    s_rx_pkts += s_rx_part / SDR_PKT_SAMPLES;
    s_rx_part %= SDR_PKT_SAMPLES;
}

void sdr_note_ws_frame(size_t bytes)
{
    ++s_ws_frames;
    s_ws_bytes += (uint32_t)bytes;
}

void sdr_note_ws_send_err(void)
{
    ++s_ws_send_err;
}

/* -------------------------------------------------------------- tx (board) */

/* All session/key/TX-ring writes are serialized by s_key. It is never
 * acquired by MONITOR, and is not held while waiting for worker exit. */
static bool session_locked(uint32_t sid)
{
    return s_on && sid != 0 && sid == s_session;
}

static size_t tx_level_locked(void)
{
    size_t n;
    portENTER_CRITICAL(&s_mux);
    n = s_tx_head - s_tx_tail;
    portEXIT_CRITICAL(&s_mux);
    return n;
}

/* Forced release used by safety paths (owner loss, disconnect, watchdog and
 * disabling SDR).  A normal browser OFF takes the asynchronous tail path
 * below so the radio remains keyed until the tone is transmitted. */
static void release_locked(const char *reason)
{
    /* Keep the last over's shape for the short-press diagnosis: a release
     * with had_pcm=false and tail=false is the "carrier but no tone" case. */
    const int64_t now_us = esp_timer_get_time();
    const bool was_tx = s_ptt_started_us != 0;
    s_last_tx_ms = s_ptt_started_us
        ? (uint32_t)((now_us - s_ptt_started_us) / 1000) : 0;
    s_last_rel_ms = (uint32_t)(now_us / 1000);
    s_last_had_pcm = s_pcm_ever;
    s_last_tail = s_tail_started;
    strlcpy(s_last_reason, reason ? reason : "?", sizeof(s_last_reason));
    if (was_tx) {
        ++s_rel_total;
        if (s_tail_started) { ++s_rel_tail; } else { ++s_rel_notail; }
        if (reason && !strcmp(reason, "pcm_timeout")) { ++s_rel_pcm_timeout; }
        if (reason && !strcmp(reason, "force_close")) { ++s_rel_force; }
        if (reason && !strcmp(reason, "tail_timeout")) { ++s_rel_tail_timeout; }
        if (reason && !strcmp(reason, "released") && !s_tail_started) {
            ++s_rel_released_notail;
        }
    }
    s_ptt_started_us = 0;
    /* Flush the network accumulator before dropping the SDR owner.  This is
     * also used by safety/disconnect paths, so a peer can never retain the
     * browser PTT until its timeout. */
    if (s_net_tx) {
        netcom_tx_end();
    }
    /* Browser network-only TX temporarily disables physical RF keying.  The
     * setting is restored before the owner is released so the next source
     * starts from the normal receive-safe default. */
    if (s_net_tx && !s_rf_tx) {
        ptt_set_rf_keying(true);
    }
    /* Release the PTT owner even if s_tx_id was left ahead of it by a
     * rejected ON: ptt_release() matches the token exactly. */
    uint32_t end_token = s_tx_id;
    if (!ptt_owned(PTT_OWNER_SDR, end_token)) {
        const uint32_t owner_token = ptt_owner_token();
        if (ptt_owner() == PTT_OWNER_SDR && owner_token != 0) {
            end_token = owner_token;
            ++s_owner_mismatch;
        }
    }
    voice_sdr_end(end_token);
    s_ptt_hold = false;
    s_releasing = false;
    s_tail_started = false;
    s_net_tx = false;
    s_rf_tx = false;
    s_end_reason = reason;
    portENTER_CRITICAL(&s_mux);
    s_tx_tail = s_tx_head;
    portEXIT_CRITICAL(&s_mux);
}

static void request_release_locked(const char *reason)
{
    if (!s_ptt_hold || s_releasing) {
        return;
    }
    /* Always enter the short release window: the client's last PCM (its
     * courtesy tone, or the end of the sentence) is still in the ring and
     * must reach the modulator.  With tail=0 the window ends as soon as the
     * ring is empty (bounded by SDR_RELEASE_NOTONE_US); with a board tail it
     * also covers the tone (SDR_RELEASE_MAX_US). */
    s_ptt_hold = false;
    s_releasing = true;
    s_tail_started = false;
    s_release_started_us = esp_timer_get_time();
    s_end_reason = reason;
}

static void finish_release_locked(const char *reason)
{
    release_locked(reason);
}

bool sdr_session_valid(uint32_t sid)
{
    if (!s_key) { return false; }
    xSemaphoreTake(s_key, portMAX_DELAY);
    const bool ok = session_locked(sid);
    if (ok) { s_client_us = esp_timer_get_time(); }
    xSemaphoreGive(s_key);
    return ok;
}

bool sdr_ptt_request(uint32_t sid, uint32_t tx, bool on)
{
    if (!s_key || !tx) { return false; }
    xSemaphoreTake(s_key, portMAX_DELAY);
    bool ok = session_locked(sid);
    if (ok) {
        s_client_us = esp_timer_get_time();
        if (!on && s_ptt_hold && tx >= s_tx_id) {
            /* The client owns this session; an OFF (even with a tx that raced
             * ahead of the board's accepted one) is authoritative.  Without
             * this, rapid re-presses left the carrier keyed until the 30 s
             * hold limit because both ON and OFF were rejected on the tx
             * mismatch. */
            if (tx != s_tx_id) { ++s_off_stale; }
            request_release_locked("tail_pending");
            xSemaphoreGive(s_key);
            return true;
        }
        if (on && tx == s_tx_id && s_ptt_hold) {
            /* Retry is idempotent: it does not extend the hard hold limit. */
        } else if (tx <= s_tx_id) {
            ok = !on && tx == s_tx_id;
            if (!ok && !on) { ++s_off_reject; }
            if (ok && s_ptt_hold) { request_release_locked("tail_pending"); }
        } else {
            /* OFF can arrive before ON after a cancelled request. Remember
             * its ID so the delayed ON can never key the transmitter. */
            if (s_ptt_hold) {
                ok = false;
                if (on) { ++s_on_reject; } else { ++s_off_reject; }
            } else {
                if (!on) { s_tx_id = tx; release_locked("cancelled"); }
                else if (!voice_sdr_begin(tx)) {
                    /* The previous owner (tail/release) is still alive.  Do
                     * NOT advance s_tx_id: a later release uses s_tx_id to
                     * ptt_release(), and a token the board never accepted
                     * leaves the real owner keyed forever. */
                    ++s_begin_busy;
                    s_end_reason = "busy"; ok = false;
                } else {
                    const bool network = netcom_mode() != NETCOM_MODE_LOCAL;
                    const bool rf = !network || netcom_rf_tx();
                    bool route_ok = !network || netcom_tx_ready();
                    if (route_ok) {
                        /* The ordinary TALK path uses the same policy: a
                         * network-only talk is a logical TX window and leaves
                         * the local SA818 in receive. */
                        s_net_tx = network;
                        s_rf_tx = rf;
                        ptt_set_rf_keying(rf);
                        if (rf) {
                            route_ok = ptt_source_key(PTT_OWNER_SDR, tx, true);
                        }
                    }
                    if (!route_ok) {
                        if (s_net_tx && !s_rf_tx) { ptt_set_rf_keying(true); }
                        s_net_tx = false;
                        s_rf_tx = false;
                        voice_sdr_end(tx);
                        s_end_reason = network ? "network_not_ready" : "radio_not_ready";
                        ok = false;
                    } else {
                        portENTER_CRITICAL(&s_mux);
                        s_tx_tail = s_tx_head;
                        portEXIT_CRITICAL(&s_mux);
                        s_tx_id = tx;
                        s_ptt_hold = true;
                        s_ptt_started_us = s_pcm_us = esp_timer_get_time();
                        s_pcm_ever = false;
                        s_end_reason = "transmitting";
                    }
                }
            }
        }
    }
    xSemaphoreGive(s_key);
    return ok;
}

bool sdr_tx_push_bytes(uint32_t sid, uint32_t tx, const uint8_t *bytes, size_t len)
{
    if (!s_key || !bytes || len < 2 || (len & 1)) { return false; }
    xSemaphoreTake(s_key, portMAX_DELAY);
    if (!session_locked(sid) || !s_ptt_hold || tx != s_tx_id ||
        !ptt_owned(PTT_OWNER_SDR, tx)) {
        xSemaphoreGive(s_key); return false;
    }
    /* Digital silence counts as arriving PCM. No signal-level gate. */
    s_pcm_us = s_client_us = esp_timer_get_time();
    s_pcm_ever = true;
    int16_t tmp[512];
    for (size_t done = 0; done < len;) {
        size_t n = (len - done) / 2;
        if (n > 512) { n = 512; }
        for (size_t i = 0; i < n; ++i) {
            const uint8_t *p = bytes + done + 2 * i;
            tmp[i] = (int16_t)(uint16_t)(p[0] | (uint16_t)p[1] << 8);
        }
        ring_put(s_tx_ring, SDR_TX_RING_SAMPLES, &s_tx_head, &s_tx_tail, tmp, n, NULL);
        done += 2 * n;
    }
    xSemaphoreGive(s_key);
    return true;
}

static int sdr_pcm_peak(const int16_t *pcm, size_t n)
{
    int peak = 0;
    if (!pcm) {
        return 0;
    }
    for (size_t i = 0; i < n; ++i) {
        const int v = pcm[i] < 0 ? -(int)pcm[i] : (int)pcm[i];
        if (v > peak) {
            peak = v;
        }
    }
    return peak;
}

/* One browser block fans out to the active destinations only after it has
 * received the same fixed MIC EQ as ordinary TALK, but through the network TX
 * gain (ntxgain) rather than the handset's txgain.  The local RF consumer is
 * explicitly told that this buffer is already processed, so the monitor does not
 * apply the MIC EQ or network->RF bgtx a second time. */
static void sdr_route_tx_pcm(int16_t *pcm, size_t n)
{
    if (!pcm || n == 0) {
        return;
    }
    const int raw_peak = sdr_pcm_peak(pcm, n);
    const int processed_peak = voice_apply_net_tx_processing(pcm, n);
    if ((uint32_t)raw_peak > s_tx_raw_peak) {
        s_tx_raw_peak = (uint32_t)raw_peak;
    }
    if ((uint32_t)processed_peak > s_tx_processed_peak) {
        s_tx_processed_peak = (uint32_t)processed_peak;
    }
    for (size_t i = 0; i < n; ++i) {
        if (pcm[i] == INT16_MAX || pcm[i] == INT16_MIN) {
            ++s_tx_processed_clip;
        }
    }
    if (s_net_tx) {
        netcom_tx_sample(pcm, n, true);
    }
    if (!s_net_tx || s_rf_tx) {
        voice_sdr_inject_processed(s_tx_id, pcm, n);
    }
}

static void sdr_tx_step(void)
{
    int16_t pcm[SDR_PKT_SAMPLES];
    xSemaphoreTake(s_key, portMAX_DELAY);
    const int64_t now = esp_timer_get_time();
    if (s_ptt_hold) {
        const char *reason = NULL;
        if (!ptt_owned(PTT_OWNER_SDR, s_tx_id)) { reason = "owner_lost"; }
        else if (now - s_ptt_started_us >= (int64_t)SDR_PTT_HOLD_MAX_MS * 1000) { reason = "hold_limit"; }
        else if (!s_pcm_ever) {
            /* The mic may take a moment to open (and the client waits for the
             * ON ACK before it starts capture).  Do not unkey a tap just
             * because no PCM arrived yet. */
            if (now - s_ptt_started_us >= (int64_t)SDR_NO_PCM_GRACE_MS * 1000) {
                reason = "pcm_timeout";
            }
        } else if (now - s_pcm_us >= (int64_t)SDR_TX_STALL_MS * 1000) {
            reason = "pcm_timeout";
        }
        if (reason) {
            /* A normal end of speech (mic silent / user stopped) must still
             * get the courtesy tone.  Only safety reasons unkey immediately. */
            if (!strcmp(reason, "pcm_timeout")) {
                request_release_locked(reason);
            } else {
                release_locked(reason);
            }
        } else if (!ptt_owned(PTT_OWNER_SDR, s_tx_id) ||
                 (s_rf_tx && !ptt_source_key(PTT_OWNER_SDR, s_tx_id, true))) {
            release_locked(s_rf_tx ? "radio_not_ready" : "owner_lost");
        }
        else if (voice_bridge_level() <= 7000) {
            const size_t n = ring_take(s_tx_ring, SDR_TX_RING_SAMPLES,
                                       &s_tx_head, &s_tx_tail, pcm, SDR_PKT_SAMPLES);
            if (n) {
                sdr_route_tx_pcm(pcm, n);
                ++s_tx_pkts; s_tx_samples += (uint32_t)n;
            } else { ++s_tx_dry; }
        }
    } else if (s_releasing) {
        const int64_t release_cap_us = voice_sdr_tail_configured()
            ? ((int64_t)voice_sdr_tail_ms() + 600) * 1000 : SDR_RELEASE_NOTONE_US;
        const int64_t cap = release_cap_us < SDR_RELEASE_MAX_US
            ? release_cap_us : SDR_RELEASE_MAX_US;
        if (!ptt_owned(PTT_OWNER_SDR, s_tx_id)) {
            finish_release_locked("owner_lost");
        } else if (now - s_release_started_us >= cap) {
            finish_release_locked("tail_timeout");
        } else if (!ptt_owned(PTT_OWNER_SDR, s_tx_id) ||
                   (s_rf_tx && !ptt_source_key(PTT_OWNER_SDR, s_tx_id, true))) {
            finish_release_locked(s_rf_tx ? "radio_not_ready" : "owner_lost");
        } else {
            if (voice_bridge_level() <= 7000) {
                const size_t n = ring_take(s_tx_ring, SDR_TX_RING_SAMPLES,
                                           &s_tx_head, &s_tx_tail, pcm,
                                           SDR_PKT_SAMPLES);
                if (n) {
                    sdr_route_tx_pcm(pcm, n);
                    ++s_tx_pkts; s_tx_samples += (uint32_t)n;
                }
            }
            if (!s_tail_started && tx_level_locked() == 0) {
                if (voice_sdr_tail_start(s_tx_id)) {
                    s_tail_started = true;
                    s_end_reason = "tail";
                } else {
                    finish_release_locked("released");
                }
            } else if (s_tail_started &&
                       voice_sdr_tail_done() == s_tx_id) {
                finish_release_locked("released");
            }
        }
    }
    if (s_ws_fd >= 0 && now - s_ws_last_us >= (int64_t)SDR_WS_STALE_MS * 1000) {
        s_ws_fd = -1;
        if (s_ptt_hold || s_releasing) { release_locked("ws_timeout"); }
    }
    /* An orphaned session: no live WebSocket, and no client of any kind
     * (WebSocket heartbeat, HTTP poll, PTT, or control traffic) has touched
     * it for one no-client window.  The listener process is gone and only it
     * could release the engine, so shut the engine down -- otherwise tail
     * uploads and OTA stay blocked by a listener that no longer exists.
     * The off path takes s_key, so sdr_tx_task runs it after this step
     * returns. */
    if (!s_ptt_hold && !s_releasing && s_ws_fd < 0 &&
        now - s_client_us >= (int64_t)SDR_NO_CLIENT_STALE_MS * 1000) {
        release_locked("ws_dead");
        s_ws_dead = true;
    }
    xSemaphoreGive(s_key);
}

static void sdr_tx_task(void *arg)
{
    (void)arg;
    while (s_on) {
        sdr_tx_step();
        if (s_ws_dead) {
            s_ws_dead = false;
            /* Outside s_key: the off path re-takes it, releases the monitor
             * borrow, and flips s_on. */
            sdr_set_enabled(false);
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    s_tx_task = NULL;
    vTaskDeleteWithCaps(NULL);
}

/* ------------------------------------------------------- test tone (local) */
/* An explicit operator-only diagnostic uses the same ownership/session rules. */
static void sdr_tone_task(void *arg)
{
    const int freq = (int)(intptr_t)arg;
    const uint32_t sid = s_session, tx = s_tx_id + 1;
    if (sdr_ptt_request(sid, tx, true)) {
        int16_t pcm[SDR_PKT_SAMPLES];
        double phase = 0, step = 2.0 * 3.14159265358979 * freq / AUDIO_SAMPLE_RATE_HZ;
        const int64_t start = esp_timer_get_time();
        for (int packet = 0; packet < 3000 / AUDIO_PKT_MS && s_on; ++packet) {
            for (int i = 0; i < SDR_PKT_SAMPLES; ++i) {
                pcm[i] = (int16_t)(sin(phase) * 6000);
                phase += step;
                if (phase > 2 * 3.14159265358979) { phase -= 2 * 3.14159265358979; }
            }
            if (!sdr_tx_push_bytes(sid, tx, (const uint8_t *)pcm, sizeof(pcm))) { break; }
            const int64_t due = start + (int64_t)(packet + 1) * AUDIO_PKT_MS * 1000;
            const int64_t now = esp_timer_get_time();
            if (due > now) { vTaskDelay(pdMS_TO_TICKS((due - now) / 1000)); }
        }
        vTaskDelay(pdMS_TO_TICKS(300));
        (void)sdr_ptt_request(sid, tx, false);
    }
    s_tone_task = NULL;
    vTaskDeleteWithCaps(NULL);
}

void sdr_play_test_tone(int freq_hz)
{
    if (!s_control) { return; }
    xSemaphoreTakeRecursive(s_control, portMAX_DELAY);
    xSemaphoreTake(s_key, portMAX_DELAY);
    if (s_on && !s_tone_task && !s_ptt_hold) {
        (void)xTaskCreateWithCaps(sdr_tone_task, "sdr_tone", 4096,
                                  (void *)(intptr_t)freq_hz, 5, &s_tone_task, MALLOC_CAP_SPIRAM);
    }
    xSemaphoreGive(s_key);
    xSemaphoreGiveRecursive(s_control);
}

/* --------------------------------------------------------------- websocket */
bool sdr_ws_owner(httpd_handle_t server, int fd, uint32_t sid)
{
    if (!s_key) { return false; }
    xSemaphoreTake(s_key, portMAX_DELAY);
    const bool ok = session_locked(sid) && s_server == server && s_ws_fd == fd;
    if (ok) { s_ws_last_us = s_client_us = esp_timer_get_time(); }
    xSemaphoreGive(s_key);
    return ok;
}

bool sdr_ws_client_attached(void)
{
    if (!s_key) { return false; }
    xSemaphoreTake(s_key, portMAX_DELAY);
    const bool ok = s_ws_fd >= 0 &&
        esp_timer_get_time() - s_ws_last_us < (int64_t)SDR_WS_STALE_MS * 1000;
    xSemaphoreGive(s_key);
    return ok;
}

bool sdr_ws_attach(httpd_handle_t server, int fd, uint32_t sid)
{
    if (!s_key) { return false; }
    xSemaphoreTake(s_key, portMAX_DELAY);
    const int64_t now = esp_timer_get_time();
    bool ok = session_locked(sid);
    if (ok && s_ws_fd >= 0 && (s_server != server || s_ws_fd != fd)) {
        ok = now - s_ws_last_us >= (int64_t)SDR_WS_STALE_MS * 1000;
        if (ok && s_ptt_hold) { release_locked("ws_replaced"); }
    }
    if (ok) {
        s_server = server; s_ws_fd = fd; s_ws_last_us = s_client_us = now;
        s_ws_dead = false;
        portENTER_CRITICAL(&s_mux);
        s_rx_tail = s_rx_head;
        portEXIT_CRITICAL(&s_mux);
    }
    xSemaphoreGive(s_key);
    return ok;
}

void sdr_ws_detach(httpd_handle_t server, int fd, uint32_t sid)
{
    if (!s_key) { return; }
    xSemaphoreTake(s_key, portMAX_DELAY);
    if (session_locked(sid) && s_server == server && s_ws_fd == fd) {
        s_ws_fd = -1;
        if (s_ptt_hold) { release_locked("disconnected"); }
    }
    xSemaphoreGive(s_key);
}

/* ------------------------------------------------------------------ enable */
esp_err_t sdr_set_enabled(bool on)
{
    if (!s_inited) { sdr_init(); }
    if (!s_rx_ring || !s_tx_ring) { return ESP_ERR_NO_MEM; }
    xSemaphoreTakeRecursive(s_control, portMAX_DELAY);
    if (on && s_on) { xSemaphoreGiveRecursive(s_control); return ESP_OK; }
    if (on) {
        if (s_tx_task || s_tone_task || voice_mode() == VOICE_TALK) {
            xSemaphoreGiveRecursive(s_control); return ESP_ERR_INVALID_STATE;
        }
        voice_monitor_borrow("sdr");
        if (voice_mode() != VOICE_MONITOR) {
            voice_monitor_release("sdr");
            xSemaphoreGiveRecursive(s_control); return ESP_ERR_INVALID_STATE;
        }
        s_monitor_borrowed = true;
        xSemaphoreTake(s_key, portMAX_DELAY);
        portENTER_CRITICAL(&s_mux);
        s_rx_head = s_rx_tail = s_tx_head = s_tx_tail = 0;
        portEXIT_CRITICAL(&s_mux);
        s_tx_raw_peak = 0;
        s_tx_processed_peak = 0;
        s_tx_processed_clip = 0;
        ++s_session; if (!s_session) { ++s_session; }
        s_client = s_tx_id = 0;
        s_ptt_hold = false;
        s_releasing = false;
        s_tail_started = false;
        s_net_tx = false;
        s_rf_tx = false;
        s_end_reason = "idle";
        s_ws_fd = -1; s_ws_dead = false; s_on = true;
        /* A new session must not inherit the previous session's WebSocket
         * timestamp, or the stale check would act before any client can
         * attach. */
        s_ws_last_us = s_client_us = esp_timer_get_time();
        xSemaphoreGive(s_key);
        if (xTaskCreateWithCaps(sdr_tx_task, "sdr_tx", 4096, NULL, 6, &s_tx_task,
                                MALLOC_CAP_SPIRAM) != pdPASS) {
            s_on = false; voice_monitor_release("sdr"); s_monitor_borrowed = false;
            xSemaphoreGiveRecursive(s_control); return ESP_ERR_NO_MEM;
        }
    } else {
        xSemaphoreTake(s_key, portMAX_DELAY);
        release_locked("disabled"); s_on = false; s_ws_fd = -1;
        xSemaphoreGive(s_key);
        /* The tx task can run this off path itself when an orphaned session
         * self-shuts (s_ws_dead); it must not wait on its own handle -- the
         * loop clears it after the return. */
        const TaskHandle_t self = xTaskGetCurrentTaskHandle();
        for (int w = 0; w < 200 &&
             ((s_tx_task && s_tx_task != self) || s_tone_task); ++w) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if ((s_tx_task && s_tx_task != self) || s_tone_task) {
            xSemaphoreGiveRecursive(s_control); return ESP_ERR_TIMEOUT;
        }
        if (s_monitor_borrowed) { voice_monitor_release("sdr"); s_monitor_borrowed = false; }
    }
    ESP_LOGI(TAG, "SDR %s sid=%lu", on ? "on" : "off", (unsigned long)s_session);
    xSemaphoreGiveRecursive(s_control);
    return ESP_OK;
}

bool sdr_client_open(uint32_t client)
{
    if (!client) { return false; }
    sdr_init();
    xSemaphoreTakeRecursive(s_control, portMAX_DELAY);
    xSemaphoreTake(s_key, portMAX_DELAY);
    const bool occupied = s_on && s_client != client &&
        esp_timer_get_time() - s_client_us < (int64_t)SDR_WS_STALE_MS * 1000;
    const bool replace = s_on && s_client != client;
    xSemaphoreGive(s_key);
    bool ok = !occupied;
    if (ok && replace) { ok = sdr_set_enabled(false) == ESP_OK; }
    if (ok) { ok = sdr_set_enabled(true) == ESP_OK; }
    if (ok) {
        xSemaphoreTake(s_key, portMAX_DELAY);
        s_client = client; s_client_us = esp_timer_get_time();
        xSemaphoreGive(s_key);
    }
    xSemaphoreGiveRecursive(s_control);
    return ok;
}

bool sdr_client_close(uint32_t sid, bool force)
{
    if (!s_control) { return false; }
    xSemaphoreTakeRecursive(s_control, portMAX_DELAY);
    bool mine = sdr_session_valid(sid);
    if (!mine) {
        /* A late close can carry a stale session id: the board already
         * replaced the session for a newer client, or a reboot rolled the
         * counter.  Then accept the goodbye only when nobody else uses the
         * engine -- a PTT hold or an attached socket means the current
         * listener owns it, and a stranger's goodbye must not drop a live
         * session. */
        xSemaphoreTake(s_key, portMAX_DELAY);
        mine = s_on && !s_ptt_hold && !s_releasing && s_ws_fd < 0;
        xSemaphoreGive(s_key);
    }
    bool ok = false;
    if (mine) {
        bool transmitting = false;
        xSemaphoreTake(s_key, portMAX_DELAY);
        transmitting = s_ptt_hold;
        if (s_ptt_hold && force) {
            /* An explicit force close may abort a live transmission: the
             * client asked to stop, and upload/OTA must not wait for the
             * 30 s hold limit. */
            release_locked("force_close");
            transmitting = false;
        } else if (s_releasing) {
            /* Tail playback is not a reason to refuse a close.  Cancel it and
             * drop the carrier now, then power the engine down. */
            finish_release_locked("closed");
        }
        xSemaphoreGive(s_key);
        if (!transmitting) { ok = sdr_set_enabled(false) == ESP_OK; }
    }
    xSemaphoreGiveRecursive(s_control);
    return ok;
}

bool sdr_enabled(void) { return s_on; }

uint32_t sdr_current_session(void)
{
    if (!s_key) { return 0; }
    xSemaphoreTake(s_key, portMAX_DELAY);
    const uint32_t sid = s_on ? s_session : 0;
    xSemaphoreGive(s_key);
    return sid;
}

/* ACK/state replies are sent only by the requesting HTTPD task. The browser
 * also pulls this every 500 ms, including while sending microphone PCM. */
void sdr_state_json(char *buf, size_t n, bool accepted, uint32_t request)
{
    xSemaphoreTake(s_key, portMAX_DELAY);
    snprintf(buf, n,
        "{\"ok\":%s,\"sid\":%lu,\"tx\":%lu,\"id\":%lu,\"ptt\":%s,\"owner\":\"%s\",\"reason\":\"%s\",\"on\":%s,"
        "\"rel\":%s,\"tail\":%s}",
        accepted ? "true" : "false", (unsigned long)s_session,
        (unsigned long)s_tx_id, (unsigned long)request, s_ptt_hold ? "true" : "false",
        ptt_owner_name(), s_end_reason, s_on ? "true" : "false",
        s_releasing ? "true" : "false", s_tail_started ? "true" : "false");
    xSemaphoreGive(s_key);
}

void sdr_status_json(char *buf, size_t n)
{
    if (!buf || !n || !s_key) { return; }
    xSemaphoreTake(s_key, portMAX_DELAY);
    const int64_t now = esp_timer_get_time();
    const unsigned long long pcm_age_ms = s_pcm_us ? (unsigned long long)
        ((now - s_pcm_us) / 1000) : 0ULL;
    snprintf(buf, n,
        "\"sdr\":{\"on\":%s,\"ws\":%s,\"sid\":%lu,\"txid\":%lu,\"ptt\":%s,\"owner\":\"%s\",\"reason\":\"%s\","
        "\"rel\":%s,\"tail\":%s,\"hold\":%s,\"pcm_age_ms\":%llu,\"ring\":%u,"
        "\"last_reason\":\"%s\",\"last_tx_ms\":%u,\"last_rel_ms\":%u,\"last_had_pcm\":%s,\"last_tail\":%s,\"pcm_ever\":%s,"
        "\"rel_total\":%u,\"rel_tail\":%u,\"rel_notail\":%u,\"rel_pcm_timeout\":%u,\"rel_force\":%u,"
        "\"rel_tail_timeout\":%u,\"rel_released_notail\":%u,"
        "\"off_stale\":%u,\"off_reject\":%u,\"on_reject\":%u,"
        "\"begin_busy\":%u,\"owner_mismatch\":%u,"
        "\"rx\":%u,\"rsmp\":%llu,\"tx\":%u,\"dry\":%u,\"inj\":%u,\"starve\":%u,\"smp\":%u,\"blk\":%u,\"trim\":%u,\"wf\":%u,\"wb\":%u,\"drop\":%u,\"werr\":%u,\"rawpk\":%u,\"outpk\":%u,\"clip\":%u}",
        s_on ? "true" : "false", s_ws_fd >= 0 ? "true" : "false",
        (unsigned long)s_session, (unsigned long)s_tx_id,
        (s_ptt_hold && !s_releasing) ? "true" : "false",
        ptt_owner_name(), s_end_reason,
        s_releasing ? "true" : "false", s_tail_started ? "true" : "false",
        s_ptt_hold ? "true" : "false", pcm_age_ms, (unsigned)tx_level_locked(),
        s_last_reason, (unsigned)s_last_tx_ms, (unsigned)s_last_rel_ms,
        s_last_had_pcm ? "true" : "false", s_last_tail ? "true" : "false",
        s_pcm_ever ? "true" : "false",
        (unsigned)s_rel_total, (unsigned)s_rel_tail, (unsigned)s_rel_notail,
        (unsigned)s_rel_pcm_timeout, (unsigned)s_rel_force,
        (unsigned)s_rel_tail_timeout, (unsigned)s_rel_released_notail,
        (unsigned)s_off_stale, (unsigned)s_off_reject, (unsigned)s_on_reject,
        (unsigned)s_begin_busy, (unsigned)s_owner_mismatch,
        (unsigned)s_rx_pkts, (unsigned long long)s_rx_smp, (unsigned)s_tx_pkts,
        (unsigned)s_tx_dry, (unsigned)voice_bridge_level(), (unsigned)voice_bridge_starve(),
        (unsigned)s_tx_samples, (unsigned)voice_bridge_blocks(), (unsigned)voice_bridge_trim(),
        (unsigned)s_ws_frames, (unsigned)s_ws_bytes, (unsigned)s_rx_drop, (unsigned)s_ws_send_err,
        (unsigned)s_tx_raw_peak, (unsigned)s_tx_processed_peak,
        (unsigned)s_tx_processed_clip);
    xSemaphoreGive(s_key);
}
