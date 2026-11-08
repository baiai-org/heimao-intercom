/*
 * netrelay -- EMQX/MQTT relay engine (netcom mode 2).
 *
 * FMO-style relay: the broker is the repeater.  Each device publishes its
 * audio on its own topic and subscribes to every device on the broker:
 *
 *   publish  orl/tx/<device_id>             (this device's audio)
 *   subscribe orl/tx/+                      (every device publishing there)
 *
 * <device_id> is the last three MAC bytes in hex, the same value esp-mqtt
 * already uses as the default client id.  The broker is plain MQTT 3.1.1,
 * anonymous, QoS 0, no TLS on the first pass.  A message published by a
 * device also comes back to it through the wildcard subscription, so the
 * sender is filtered out by the MAC in the frame header (the topic alone
 * is not the source of truth).
 *
 * There is no room/group number here: the EMQX server is the scope.  Whoever
 * is connected to that broker and subscribes to orl/tx/+ hears the traffic,
 * which is what the mode is for.  (Removed 2026-10-16; the topic used to be
 * orl/<room>/tx/<id>.)
 *
 * The frame format is the same 14-byte header plus u-law payload the NET-P2P
 * engine uses:
 *
 *   14-byte header (magic / reserved / seq / sender MAC / flags) + 232 x u-law
 *   samples (29 ms at 8 kHz).
 *
 * The receive side is the same seq-stamped jitter buffer and playback task
 * as netp2pnet: one 29 ms packet per loop, the blocking I2S write locking the
 * task to the sample clock, and an amplifier that follows the audio level.
 */
#include "netrelay.h"
#include "radio_led.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "mqtt_client.h"

#include "audio_i2s.h"
#include "board_hw.h"
#include "bridge.h"
#include "ptt.h"
#include "sdr.h"
#include "voice_tail.h"
#include "netcom.h"
#include "voice.h"
#include "opus_link.h"

static const char *TAG = "netrelay";

/* Exactly one courtesy tone per over, and never hold the carrier forever. */
static bool s_tail_played;
static int64_t s_tail_start_ms;

#define REL_MAGIC         0x4F52
#define REL_PKT_SAMPLES   OPUS_LINK_SAMPLES   /* one Opus frame */
#define REL_AUDIO_OPUS    (OPUS_LINK_BITRATE / 8 * OPUS_LINK_FRAME_MS / 1000)  /* 120 */
#define REL_HDR_LEN       14
/* One fixed-size Opus frame follows the 14-byte header. */
#define REL_PKT_LEN       (REL_HDR_LEN + REL_AUDIO_OPUS)     /* 134 */

#define REL_JBUF_SLOTS    128     /* ~3.7 s of 29 ms packets: the public link
                                   * stalls for seconds at a time, and a
                                   * dropped packet is silence in the middle of
                                   * a word.  Capacity costs PSRAM only. */
/*
 * Playout cushion in 29 ms packets.  3 (87 ms) is too shallow for WiFi: a late
 * packet becomes a silent hole and the relayed audio is heard as chopped.
 * 12 (~350 ms) matches the P2P engine.
 */
#define REL_PLAY_LAG      6      /* 120 ms of cushion (6 x 20 ms): the operator asked
                                  * for 120 ms on both engines.  A public-broker
                                  * stream will starve more often than it did at
                                  * 320 ms -- that trade is deliberate. */
#define REL_STREAM_IDLE_MS 500    /* still > REL_PLAY_LAG so the tail plays */
#define REL_PEER_SEEN_MS  5000
#define REL_PKT_MS        20      /* one packet of audio (20 ms frames) */

/* How long a silent stream keeps the bridge keyed after the far end's flag
 * clears, and how long a missing frame may go before we assume it is gone.
 *
 * 500 ms of hang is what a real repeater's tail does: without it, a squelch
 * flicker or a single dropped MQTT packet unkeys the transmitter mid-sentence
 * and the operator hears chopping.  2 s (was 1 s) tolerates a WiFi hiccup. */
#define REL_RX_HANG_MS        250   /* counted from the last PLAYED block; the
                                     * cushion already covers the tail */
#define REL_RX_PTT_TIMEOUT_MS 2000

#pragma pack(push, 1)
typedef struct {
    uint16_t magic;
    uint16_t reserved;   /* was the room; the relay is not grouped any more, so
                          * it is written as 0 and never checked.  Kept in the
                          * layout so the header stays 14 bytes and a stray
                          * older packet is still parsed rather than misread. */
    uint16_t seq;
    uint8_t  mac[6];
    uint8_t  flags;
    uint8_t  reserved2;
} rel_header_t;
#pragma pack(pop)

_Static_assert(sizeof(rel_header_t) == REL_HDR_LEN, "header must be 14 bytes");

static const char *s_topic_root = "orl";
static uint8_t s_mac[6];
static char s_device_id[7];

static esp_mqtt_client_handle_t s_client;
/* Receive-side accounting: "the broker delivers, the board drops" needs to be
 * visible, otherwise a filter that rejects everything looks like a dead link. */
static uint32_t s_ev_total, s_ev_badlen, s_ev_badmagic, s_ev_self, s_ev_ok;

/*
 * Transmit key, taken from the PTT flag in the received frames.
 *
 * Every audio frame carries "the far end is in a transmission" (it is the
 * sender's own squelch/PTT state, not a per-packet level), and tx_end() sends
 * one final frame with the flag cleared.  Keying the local transmitter from
 * that state -- instead of from the local audio level or from packet timing --
 * is what stops the carrier blinking: the key follows the over, exactly like a
 * real PTT, and there is nothing in the middle to chatter.
 */
static volatile bool    s_rx_ptt;
static volatile int64_t s_rx_ptt_ms;
static volatile int64_t s_rx_hang_ms;      /* unkey pending since (0 = none) */
/* Timestamp of the last packet actually received (playback must not refresh
 * this: it is what tells a stalled sender from a playing one). */
static volatile int64_t s_last_pkt_ms;
static uint32_t s_starves;
static uint32_t s_primes;    /* streams primed since boot */
static uint16_t s_prime_first, s_prime_play;  /* last priming: stream start vs play head */
static uint32_t s_late;      /* arrived after the play head had passed */
static uint32_t s_plc;       /* lost frames Opus concealed */                 /* playout waits while a stream is live */
static volatile bool     s_started;
static bool     s_connected;
static char     s_uri[64];
static char     s_my_topic[32];
static char     s_sub_topic[32];
static int      s_sub_id;

/* Connection diagnostics, surfaced through /api/status while connecting. */
static int64_t  s_start_ms;
static int      s_attempts;
static char     s_last_err[48];

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

/* The device id: last three MAC bytes in hex.  Matches the derivation of
 * esp-mqtt's default client id, so broker logs and topics agree. */
static const char *device_id(void)
{
    snprintf(s_device_id, sizeof(s_device_id), "%02x%02x%02x",
             s_mac[3], s_mac[4], s_mac[5]);
    return s_device_id;
}

/* ------------------------------------------------------------- peers */

static struct {
    char    id[7];      /* 6 hex digits + NUL */
    int64_t last_ms;
} s_peers[8];

static void peer_seen(const char *id)
{
    const int64_t now = now_ms();
    for (int i = 0; i < (int)(sizeof(s_peers) / sizeof(s_peers[0])); ++i) {
        if (strcmp(s_peers[i].id, id) == 0) {
            s_peers[i].last_ms = now;
            return;
        }
    }
    int oldest = 0;
    for (int i = 1; i < (int)(sizeof(s_peers) / sizeof(s_peers[0])); ++i) {
        if (s_peers[i].last_ms < s_peers[oldest].last_ms) {
            oldest = i;
        }
    }
    snprintf(s_peers[oldest].id, sizeof(s_peers[oldest].id), "%s", id);
    s_peers[oldest].last_ms = now;
}

static int peer_count(void)
{
    const int64_t now = now_ms();
    int n = 0;
    for (int i = 0; i < (int)(sizeof(s_peers) / sizeof(s_peers[0])); ++i) {
        if (s_peers[i].id[0] && now - s_peers[i].last_ms < REL_PEER_SEEN_MS) {
            ++n;
        }
    }
    return n;
}

/* -------------------------------------------------------- receive side */

/* Audio lives in PSRAM, allocated in relay_start -- see netp2pnet.c for the
 * DRAM budget and why this is a heap allocation rather than a static
 * `.psram` section. */
static uint8_t (*s_jbuf)[REL_AUDIO_OPUS];
static uint16_t s_jbuf_seq[REL_JBUF_SLOTS];
static bool s_jbuf_valid[REL_JBUF_SLOTS];
static uint8_t s_jbuf_mac[REL_JBUF_SLOTS][6];
static SemaphoreHandle_t s_rx_mux;
static void rx_lock(void) { if (s_rx_mux) xSemaphoreTake(s_rx_mux, portMAX_DELAY); }
static void rx_unlock(void) { if (s_rx_mux) xSemaphoreGive(s_rx_mux); }
static uint16_t s_latest;
static bool     s_have_latest;
static int64_t  s_last_rx_ms;
static uint32_t s_rx_msgs;
static uint32_t s_rx_dropped;

static bool s_play_head_valid;
/*
 * First sequence number of the current stream, and whether it is known.
 *
 * The sender's sequence counter never resets between transmissions, so a new
 * stream's first packet is contiguous with the previous one's last.  Anchoring
 * the play head at "latest - REL_PLAY_LAG" as soon as the first packet arrives
 * therefore pointed it at the PREVIOUS transmission's last packets -- the far
 * end replayed the handheld's squelch tail at the start of every new call.
 * A stream may only be primed once REL_PLAY_LAG packets of THAT stream exist.
 */
static uint16_t s_stream_first;
static bool     s_have_first;
static uint16_t s_play_head;
static bool s_amp_on;

/* One encoder for the transmit path, one decoder for whatever sender we are
 * currently playing: the G.722 state carries the QMF history and the ADPCM
 * predictors, so two interleaved talkers would have to share it.  Switching
 * sender re-initialises it, which costs one frame of settling. */
static opus_link_enc_t *s_opus_enc;
static opus_link_dec_t *s_opus_dec;
static uint8_t          s_opus_dec_mac[6];
static uint8_t          s_stream_mac[6];

static void relay_reset_stream(void)
{
    memset(s_jbuf_valid, 0, sizeof(s_jbuf_valid));
    s_have_latest = s_have_first = s_play_head_valid = false;
    opus_link_dec_free(s_opus_dec);
    s_opus_dec = NULL;
    memset(s_opus_dec_mac, 0, sizeof(s_opus_dec_mac));
}

/* Only complete fixed-size codec frames reach this function.  MQTT reception
 * and the player serialize stream state with s_rx_mux. */
static void relay_store_packet(const rel_header_t *h, const uint8_t *audio,
                               size_t audio_len)
{
    if (audio_len != REL_AUDIO_OPUS || !s_jbuf) { ++s_ev_badlen; return; }
    const uint16_t seq = h->seq;
    const bool sender_changed = memcmp(s_stream_mac, h->mac, 6) != 0;
    if (sender_changed || (s_have_latest && now_ms() - s_last_pkt_ms > REL_STREAM_IDLE_MS)) {
        relay_reset_stream();
        memcpy(s_stream_mac, h->mac, 6);
    }
    if (s_have_latest && (int16_t)(seq - s_latest) >= REL_JBUF_SLOTS / 2) {
        relay_reset_stream();
    }
    if (s_play_head_valid && (int16_t)(seq - s_play_head) < 0) { ++s_late; return; }
    if (!s_have_latest) { s_stream_first = seq; s_have_first = true; }
    if (!s_have_latest || (int16_t)(seq - s_latest) > 0) { s_latest = seq; }
    s_have_latest = true;
    s_last_rx_ms = s_last_pkt_ms = now_ms();
    ++s_rx_msgs;
    const uint16_t slot = seq % REL_JBUF_SLOTS;
    memcpy(s_jbuf[slot], audio, REL_AUDIO_OPUS);
    memcpy(s_jbuf_mac[slot], h->mac, 6);
    s_jbuf_seq[slot] = seq;
    s_jbuf_valid[slot] = true;
    if (h->flags & 1) { radio_led_note_net_rx(NETCOM_MODE_RELAY); }
}

/* ------------------------------------------------------- MQTT events */

static void relay_subscribe(void)
{
    if (!s_client || !s_connected) {
        return;
    }
    snprintf(s_sub_topic, sizeof(s_sub_topic), "%s/tx/+", s_topic_root);
    /*
     * MQTT 5 "No Local": without it the broker echoes every frame this board
     * publishes back to it, so a 24 KB/s stream costs 24 KB/s upstream *and*
     * 24 KB/s downstream on the same TCP connection.  The echoes are useless
     * (they are filtered by MAC a few lines below) but they fill the receive
     * window, which back-pressures the broker and stalls the publisher.
     */
    esp_mqtt5_subscribe_property_config_t props = { .no_local_flag = true };
    (void)esp_mqtt5_client_set_subscribe_property((esp_mqtt5_client_handle_t)s_client, &props);
    const int id = esp_mqtt_client_subscribe(s_client, s_sub_topic, 0);
    if (id >= 0) {
        s_sub_id = id;
        ESP_LOGI(TAG, "subscribed to %s", s_sub_topic);
    } else {
        ESP_LOGW(TAG, "subscribe %s failed (%d)", s_sub_topic, id);
    }
}

static void relay_mqtt_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    (void)base;
    const esp_mqtt_event_handle_t ev = (const esp_mqtt_event_handle_t)event_data;

    switch (ev->event_id) {
    case MQTT_EVENT_BEFORE_CONNECT:
        ++s_attempts;
        s_last_err[0] = '\0';
        ESP_LOGI(TAG, "connect attempt %d -> %s ...", s_attempts, s_uri);
        break;
    case MQTT_EVENT_ERROR: {
        const int rc = ev->error_handle
                           ? (int)ev->error_handle->connect_return_code : -1;
        if (rc == 5) {
            /* CONNACK 5 = not authorized.  The broker rejected the username or
             * password; say so, because "err type ... rc 5" reads like a
             * network fault and sends the operator looking at the wrong thing. */
            snprintf(s_last_err, sizeof(s_last_err),
                     "not authorized (rc 5) -- check user/password");
        } else {
            snprintf(s_last_err, sizeof(s_last_err),
                     "err type %d / sock %d / tls %d / rc %d",
                     ev->error_handle ? (int)ev->error_handle->error_type : -1,
                     ev->error_handle ? ev->error_handle->esp_transport_sock_errno : -1,
                     ev->error_handle ? ev->error_handle->esp_tls_stack_err : -1,
                     rc);
        }
        ESP_LOGW(TAG, "mqtt error: %s", s_last_err);
        break;
    }
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        s_last_err[0] = '\0';
        ESP_LOGI(TAG, "broker connected (%s) after %d attempt(s)", s_uri, s_attempts);
        relay_subscribe();
        break;

    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        s_sub_id = 0;
        ESP_LOGW(TAG, "broker disconnected -- auto-reconnect, resubscribe on reconnect");
        break;

    case MQTT_EVENT_DATA: {
        const size_t dl = (size_t)ev->data_len;
        if (dl == 0 || ev->current_data_offset != 0 || ev->total_data_len != ev->data_len || dl % REL_PKT_LEN != 0) {
            ++s_ev_badlen;
            break;
        }
        for (size_t off = 0; off < dl; off += REL_PKT_LEN) {
            ++s_ev_total;
            rel_header_t h;
            memcpy(&h, ev->data + off, sizeof(h));
            if (h.magic != REL_MAGIC) { ++s_ev_badmagic; continue; }
            if (memcmp(h.mac, s_mac, 6) == 0) { ++s_ev_self; continue; }
            ++s_ev_ok;
            char id[7];
            snprintf(id, sizeof(id), "%02x%02x%02x", h.mac[3], h.mac[4], h.mac[5]);
            rx_lock();
            peer_seen(id);
            s_rx_ptt = (h.flags & 1) != 0;
            s_rx_ptt_ms = now_ms();
            netcom_note_rx_ptt(s_rx_ptt, false);
            if (s_rx_ptt) { s_rx_hang_ms = 0; }
            relay_store_packet(&h, (const uint8_t *)ev->data + off + REL_HDR_LEN,
                               REL_AUDIO_OPUS);
            rx_unlock();
        }
        break;
    }

    default:
        break;
    }
}

/* -------------------------------------------------------- transmit side */

static int      s_tx_acc_n;
static uint16_t s_tx_seq;
static uint32_t s_tx_msgs;
static uint32_t s_tx_errs;
static int64_t  s_last_tx_ms;

/* Frame assembly is shared by the TALK task, the monitor task's analog bridge
 * and the injected TX self-test tone, so accumulator, sequence number and the
 * MQTT publish all run under s_tx_mux: concurrent producers must not tear
 * s_tx_acc_n, overrun the array, reuse a sequence number, or publish two blocks
 * out of sequence order (netp2pnet.c has the same three producers).  A mutex,
 * not a spinlock: esp_mqtt_client_publish() may block. */
static SemaphoreHandle_t s_tx_mux;

static void tx_lock(void)   { if (s_tx_mux) { xSemaphoreTake(s_tx_mux, portMAX_DELAY); } }
static void tx_unlock(void) { if (s_tx_mux) { xSemaphoreGive(s_tx_mux); } }

/* Caller holds s_tx_mux. */
/*
 * Publishing must never run in the audio task.
 *
 * esp_mqtt_client_publish() writes to the socket and can block for as long as
 * the network timeout, and this used to be called straight from the monitor /
 * talk task: at 8 kHz a 246-byte frame got away with it, at 12 kHz a 710-byte
 * frame every 29 ms does not -- the capture ring overruns (ovf climbs) and the
 * forwarded audio comes out with holes that sound like stutter.
 *
 * So the audio task only formats the frame and drops it in a queue; a dedicated
 * task owns the socket.  Twelve frames is ~350 ms: if the link stalls longer
 * than that the oldest frame is dropped, because audio older than that is
 * worthless anyway.
 */
#define REL_TXQ_DEPTH 96   /* 1.9 s of slack at 20 ms frames.  Measured: a 48-deep
                            * queue (960 ms) still dropped 20 frames in six
                            * minutes of relay traffic on the public broker, so
                            * a publish can stall for close to a second.  The
                            * queue lives in PSRAM, so depth costs nothing that
                            * matters. */

static QueueHandle_t s_txq;
static TaskHandle_t  s_tx_task;
static volatile bool s_tx_stop;
/* Two locks, on purpose: s_tx_mux protects the accumulator/sequence and is held
 * for microseconds by the audio task, while s_pub_mux is held across
 * esp_mqtt_client_publish() by the transmit task.  Sharing one lock made the
 * audio task wait for the socket -- bursts of 0-20 frames per second instead of
 * a steady 34.5, heard as stutter. */
static SemaphoreHandle_t s_pub_mux;
static uint32_t      s_tx_qdrop;
static int16_t       s_tx_acc[REL_PKT_SAMPLES];

typedef struct {
    int16_t pcm[REL_PKT_SAMPLES];
    uint8_t ptt;
    int64_t end_ms;             /* diagnostic only; 0 for ordinary audio */
} rel_tx_item_t;

/*
 * Queue the audio, not the packet.
 *
 * Opus encoding costs ~12 ms per 40 ms frame on this chip; doing it on the audio
 * task (which is what the G.722 path did, at ~0.1 ms) would stall the sample
 * clock.  The transmit task owns the encoder instead.
 */
static void rel_enqueue_pcm(const int16_t *pcm, bool ptt)
{
    if (!s_txq) {
        return;
    }
    rel_tx_item_t item;
    memcpy(item.pcm, pcm, sizeof(item.pcm));
    item.ptt = ptt ? 1 : 0;
    item.end_ms = ptt ? 0 : now_ms();

    if (xQueueSend(s_txq, &item, 0) == pdTRUE) {
        return;
    }
    rel_tx_item_t dump;
    (void)xQueueReceive(s_txq, &dump, 0);
    ++s_tx_qdrop;                  /* the newest frame wins, the oldest is lost */
    if (xQueueSend(s_txq, &item, 0) != pdTRUE) {
        ++s_tx_qdrop;
    }
}

/*
 * Batch frames into one publish.
 *
 * Measured on a 182 ms-RTT link the board could only push ~12 of the needed
 * 34.5 publishes per second -- whether the TCP send buffer was 4 or 8 MSS, so
 * the cost is per message, not per byte.  One MQTT message can carry several
 * complete frames (each keeps its own 14-byte header and sequence), so four
 * frames leave every ~116 ms instead of one every 29 ms.  The receiver walks
 * the payload frame by frame (see MQTT_EVENT_DATA), so a single-frame sender
 * keeps working too.
 */
/*
 * How long a publish may wait for the rest of its batch.  Frames are 20 ms now
 * (they were 29 ms when this was tuned), so 40 ms collects two, and the cap of
 * four means a batch is 2-4 frames = 40-80 ms of audio.  Waiting longer would
 * buy nothing: the receiver plays from a 320 ms cushion either way, and every
 * extra millisecond here is latency the operator hears.
 */
#define REL_BATCH_WAIT_US 40000
#define REL_TXBATCH 4

static void relay_tx_task(void *arg)
{
    (void)arg;
    /* PSRAM, not the task stack: a batch is a few hundred bytes and
     * esp_mqtt_client_publish() is deep enough on its own (a stack array here
     * once panicked the board). */
    const size_t cap = (size_t)REL_PKT_LEN * REL_TXBATCH;
    uint8_t *pkt = (uint8_t *)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!pkt) {
        pkt = (uint8_t *)malloc(cap);
    }
    if (!pkt) {
        ESP_LOGE(TAG, "tx task: no buffer");
        s_tx_task = NULL;
        vTaskDeleteWithCaps(NULL);
        return;
    }
    if (!s_opus_enc) {
        s_opus_enc = opus_link_enc_new();
    }
    if (!s_opus_enc) { ESP_LOGE(TAG, "tx task: no Opus encoder"); }

    while (!s_tx_stop) {
        rel_tx_item_t item;
        if (!s_txq || xQueueReceive(s_txq, &item, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }
        size_t bytes = 0;
        int64_t end_ms = 0;
        bool has_end = false;
        bool has_audio = false;
        const int64_t t_batch = esp_timer_get_time();
        for (;;) {
            rel_header_t *h = (rel_header_t *)(pkt + bytes);
            h->magic = REL_MAGIC;
            h->reserved = 0;
            h->seq = s_tx_seq++;
            memcpy(h->mac, s_mac, 6);
            h->flags = item.ptt;
            h->reserved2 = 0;
            const int n = s_opus_enc
                              ? opus_link_encode(s_opus_enc, item.pcm,
                                                 pkt + bytes + REL_HDR_LEN,
                                                 REL_AUDIO_OPUS)
                              : -1;
            if (n != REL_AUDIO_OPUS) {
                ++s_tx_errs;
                if (!item.ptt) { netcom_note_end_sent(item.end_ms, false); }
                break; /* Never publish stale bytes as an Opus frame. */
            }
            bytes += REL_PKT_LEN;
            if (item.ptt) { has_audio = true; }
            if (!item.ptt) {
                has_end = true;
                end_ms = item.end_ms;
            }
            /* PTT=0 completes a stream: publish it without waiting for a new
             * batch, and keep the next transmission in its own publish. */
            if (has_end || bytes + REL_PKT_LEN > cap) {
                break;
            }
            const int64_t left_us = REL_BATCH_WAIT_US - (esp_timer_get_time() - t_batch);
            if (left_us <= 0) {
                break;
            }
            if (xQueueReceive(s_txq, &item, pdMS_TO_TICKS(left_us / 1000 + 1)) != pdTRUE) {
                break;
            }
        }
        /*
         * The mutex is shared with relay_stop(): a publish must never race the
         * client's destruction.
         */
        if (!bytes) { continue; }
        bool published = false;
        if (s_pub_mux) { xSemaphoreTake(s_pub_mux, portMAX_DELAY); }
        if (s_client && s_connected) {
            snprintf(s_my_topic, sizeof(s_my_topic), "%s/tx/%s",
                     s_topic_root, device_id());
            const int id = esp_mqtt_client_publish(s_client, s_my_topic,
                                                   (const char *)pkt,
                                                   (int)bytes, 0, 0);
            if (id >= 0) {
                published = true;
                s_tx_msgs += (uint32_t)(bytes / REL_PKT_LEN);   /* count frames */
                s_last_tx_ms = now_ms();
                if (has_audio) { radio_led_note_net_tx(NETCOM_MODE_RELAY); }
            } else {
                s_tx_errs += (uint32_t)(bytes / REL_PKT_LEN);
            }
        }
        if (s_pub_mux) { xSemaphoreGive(s_pub_mux); }
        if (has_end) { netcom_note_end_sent(end_ms, published); }
    }
    opus_link_enc_free(s_opus_enc);
    s_opus_enc = NULL;
    free(pkt);
    s_tx_task = NULL;
    vTaskDeleteWithCaps(NULL);
}

static void relay_tx_sample(const int16_t *pcm, size_t n, bool ptt)
{
    tx_lock();
    if (!s_started || !ptt) {
        s_tx_acc_n = 0;
        tx_unlock();
        return;
    }
    for (size_t i = 0; i < n; ++i) {
        s_tx_acc[s_tx_acc_n++] = pcm[i];
        if (s_tx_acc_n == REL_PKT_SAMPLES) {
            s_tx_acc_n = 0;
            rel_enqueue_pcm(s_tx_acc, ptt);
        }
    }
    tx_unlock();
}

/*
 * End of transmission: publish the final partial packet (silence-padded) with
 * the PTT flag cleared, so the room stops forwarding the moment our tail tone
 * ends (see netp2pnet_tx_end in netp2pnet.c for the reasoning).
 */
static void relay_tx_end(void)
{
    tx_lock();
    if (!s_started) {
        tx_unlock();
        return;
    }
    if (s_tx_acc_n > 0) {
        while (s_tx_acc_n < REL_PKT_SAMPLES) { s_tx_acc[s_tx_acc_n++] = 0; }
    } else {
        memset(s_tx_acc, 0, sizeof(s_tx_acc));
    }
    s_tx_acc_n = 0;
    rel_enqueue_pcm(s_tx_acc, false);   /* explicit end-of-transmission */
    tx_unlock();
}

/* -------------------------------------------------------- playback task */

static volatile bool s_play_stop;
static TaskHandle_t s_play_task;

/*
 * Fetch the next jitter-buffer block for playback: advances s_play_head and
 * re-anchors on loss.  Returns the block, or NULL for a silence gap / nothing
 * to play this loop, and puts the block's peak in *peak_out.
 */
static bool rel_jbuf_next(int16_t *out, int *peak_out)
{
    *peak_out = 0;
    if (s_have_latest && now_ms() - s_last_pkt_ms > REL_STREAM_IDLE_MS) {
        s_play_head_valid = false;
        s_have_latest = false;
        s_have_first = false;
        memset(s_jbuf_valid, 0, sizeof(s_jbuf_valid));
        opus_link_dec_free(s_opus_dec);
        s_opus_dec = NULL;
        memset(s_opus_dec_mac, 0, sizeof(s_opus_dec_mac));
    }
    if (!s_have_latest) { return false; }
    if (!s_play_head_valid) {
        if (!s_have_first || (int16_t)(s_latest - s_stream_first) < REL_PLAY_LAG) {
            return false;
        }
        s_play_head = (uint16_t)(s_latest - REL_PLAY_LAG);
        s_play_head_valid = true;
        ++s_primes;
        s_prime_first = s_stream_first;
        s_prime_play = s_play_head;
    } else {
        const int32_t behind = (int32_t)(int16_t)(s_latest - s_play_head);
        if (behind > (REL_JBUF_SLOTS - REL_PLAY_LAG)) {
            s_play_head = (uint16_t)(s_latest - REL_PLAY_LAG);
        }
    }
    if ((int16_t)(s_play_head - s_latest) > 0) { return false; }
    const uint16_t slot = s_play_head % REL_JBUF_SLOTS;
    if (s_jbuf_valid[slot] && s_jbuf_seq[slot] == s_play_head) {
        if (!s_opus_dec || memcmp(s_opus_dec_mac, s_jbuf_mac[slot], 6) != 0) {
            opus_link_dec_free(s_opus_dec);
            s_opus_dec = opus_link_dec_new();
            memcpy(s_opus_dec_mac, s_jbuf_mac[slot], 6);
        }
        if (!s_opus_dec || opus_link_decode(s_opus_dec, (const uint8_t *)s_jbuf[slot],
                                            REL_AUDIO_OPUS, out) <= 0) {
            memset(out, 0, REL_PKT_SAMPLES * sizeof(*out));
        }
        s_jbuf_valid[slot] = false;
        s_play_head++;
        s_last_rx_ms = now_ms();
    } else if (s_opus_dec) {
        if (opus_link_decode(s_opus_dec, NULL, 0, out) > 0) {
            ++s_plc;
            s_play_head++;
            s_last_rx_ms = now_ms();
        } else {
            s_play_head++;
            memset(out, 0, REL_PKT_SAMPLES * sizeof(*out));
        }
    } else {
        s_play_head++;
        return false;
    }
    for (int i = 0; i < REL_PKT_SAMPLES; ++i) {
        const int a = out[i] < 0 ? -out[i] : out[i];
        if (a > *peak_out) { *peak_out = a; }
    }
    return true;
}

/* PTT and buffered playback control the carrier; PCM amplitude never does. */
static int64_t s_last_play_ms;
static void bridge_key_update(bool played)
{
    rx_lock();
    const int64_t now = now_ms();
    if (s_rx_ptt && now - s_rx_ptt_ms > REL_RX_PTT_TIMEOUT_MS) {
        s_rx_ptt = false;
        netcom_note_rx_ptt(false, true);
    }
    if (s_rx_ptt || played) { s_last_play_ms = now; }
    const bool keyed = s_rx_ptt ||
        (s_last_play_ms != 0 && now - s_last_play_ms < REL_RX_HANG_MS);
    netcom_note_rx_carrier(keyed);
    rx_unlock();
    bool tailing = voice_net_tail_active();
    if (played) {
        s_tail_played = false;            /* a new over may have its own tone */
        if (tailing) { voice_net_tail_stop(); tailing = false; }
    }
    if (!keyed && !tailing && !s_tail_played && bridge_active()) {
        tailing = voice_net_tail_start();  /* request; never fails on timing */
        if (tailing) { s_tail_played = true; }
    }
    if (bridge_active()) {
        ptt_bridge_key(keyed || tailing);
    } else if (ptt_owned(PTT_OWNER_NETWORK, 1)) {
        ptt_bridge_key(false);
    }
}

static void relay_play_task(void *arg)
{
    (void)arg;
    int16_t block[REL_PKT_SAMPLES];
    int16_t silence[REL_PKT_SAMPLES] = {0};
    int64_t next_ms = 0;
    while (!s_play_stop) {
        if (voice_mode() == VOICE_MONITOR) {
            const int64_t now = now_ms();
            if (!next_ms || now - next_ms > 4 * REL_PKT_MS) { next_ms = now; }
            bool played = false;
            for (int due = 0; due < 4 && now >= next_ms; ++due) {
                int peak = 0;
                rx_lock();
                const bool have = rel_jbuf_next(block, &peak);
                rx_unlock();
                if (have) { voice_bridge_inject(block, REL_PKT_SAMPLES); played = true; }
                next_ms += REL_PKT_MS;
            }
            bridge_key_update(played);
            /* The monitor owns the amplifier in this mode. */
            if (s_amp_on) { board_amp_enable(false); s_amp_on = false; }
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        next_ms = 0;
        if (!audio_i2s_clock_running()) { (void)audio_i2s_start_clock_only(); }
        const bool dac_owned = !voice_dac_held_by_talk();
        if (dac_owned != s_amp_on) { board_amp_enable(dac_owned); s_amp_on = dac_owned; }
        const size_t preview = dac_owned ? tail_preview_fetch(block, REL_PKT_SAMPLES) : 0;
        if (preview) {
            voice_speaker_apply(block, preview);
            (void)audio_i2s_write(block, preview);
            continue;
        }
        int peak = 0;
        rx_lock();
        const bool have = rel_jbuf_next(block, &peak);
        rx_unlock();
        bridge_key_update(have);
        int16_t *pcm = have ? block : silence;
        if (!ptt_bridge_keyed()) { voice_speaker_apply(pcm, REL_PKT_SAMPLES); }
        if (voice_dac_held_by_talk()) {
            s_amp_on = false;
            vTaskDelay(pdMS_TO_TICKS(REL_PKT_MS));
        } else {
            (void)audio_i2s_write(pcm, REL_PKT_SAMPLES);
        }
    }
    s_play_task = NULL;
    vTaskDeleteWithCaps(NULL);
}

/*
 * Self-test: feed a 1 kHz tone into the jitter buffer as if a peer had sent
 * it (console: `bridge_test 5000`).  The packet is precomputed once and the
 * commit is a memcpy per slot so the 300 ms stream-idle timeout cannot fire
 * mid-commit and drop the tone (see netp2pnet.c).
 */
static TaskHandle_t s_test_tone_task;
static void relay_tone_task(void *arg)
{
    const int ms = (int)(intptr_t)arg;
    opus_link_enc_t *enc = opus_link_enc_new();
    int16_t pcm[REL_PKT_SAMPLES];
    uint8_t packet[REL_AUDIO_OPUS];
    rel_header_t h = {.magic = REL_MAGIC, .seq = 1000, .flags = 1};
    memset(h.mac, 0xEE, sizeof(h.mac));
    const int frames = (ms + REL_PKT_MS - 1) / REL_PKT_MS;
    for (int f = 0; enc && f < frames && s_started; ++f) {
        for (int i = 0; i < REL_PKT_SAMPLES; ++i) {
            pcm[i] = (int16_t)(6000.0 * sin(2.0 * M_PI * 1000.0 *
                (double)(f * REL_PKT_SAMPLES + i) / AUDIO_SAMPLE_RATE_HZ));
        }
        if (opus_link_encode(enc, pcm, packet, sizeof(packet)) != REL_AUDIO_OPUS) { break; }
        rx_lock();
        relay_store_packet(&h, packet, sizeof(packet));
        s_rx_ptt = true; s_rx_ptt_ms = now_ms();
        rx_unlock();
        ++h.seq;
        vTaskDelay(pdMS_TO_TICKS(REL_PKT_MS));
    }
    rx_lock(); s_rx_ptt = false; rx_unlock();
    opus_link_enc_free(enc);
    s_test_tone_task = NULL;
    vTaskDeleteWithCaps(NULL);
}

static void relay_test_tone(int ms)
{
    if (!s_started || ms <= 0 || s_test_tone_task) { return; }
    if (ms > 30000) { ms = 30000; }
    if (xTaskCreateWithCaps(relay_tone_task, "rel_tone", 16384,
            (void *)(intptr_t)ms, 5, &s_test_tone_task, MALLOC_CAP_SPIRAM) != pdPASS) {
        s_test_tone_task = NULL;
        ESP_LOGE(TAG, "test tone task failed to start");
    }
}

/* ------------------------------------------------ room change (not used) */

/*
 * The relay topic no longer carries a room, so a room change needs no
 * resubscribe and the engine leaves the netcom on_room_change seam NULL.
 */

/* -------------------------------------------------------- engine seams */

static char s_start_err[64];

static esp_err_t relay_stop(void);

static esp_err_t relay_start(const netcom_cfg_t *cfg)
{
    if (s_started) { return ESP_OK; }
    if (s_play_task || s_tx_task || s_test_tone_task || s_client) { return ESP_ERR_INVALID_STATE; }
    if (!s_rx_mux) { s_rx_mux = xSemaphoreCreateMutex(); }
    if (!s_tx_mux) { s_tx_mux = xSemaphoreCreateMutex(); }
    if (!s_pub_mux) { s_pub_mux = xSemaphoreCreateMutex(); }
    if (!s_rx_mux || !s_tx_mux || !s_pub_mux) { return ESP_ERR_NO_MEM; }
    s_start_err[0] = '\0';
    if (esp_wifi_get_mac(WIFI_IF_STA, s_mac) != ESP_OK) {
        snprintf(s_start_err, sizeof(s_start_err), "wifi sta mac failed");
        return ESP_FAIL;
    }

    /*
     * Jitter buffer in PSRAM before the MQTT client exists: once the client
     * is started, inbound messages write into the buffer from the MQTT task.
     */
    if (!s_jbuf) {
        s_jbuf = (uint8_t (*)[REL_AUDIO_OPUS])heap_caps_malloc(
            (size_t)REL_JBUF_SLOTS * REL_AUDIO_OPUS,
            MALLOC_CAP_SPIRAM);
        if (!s_jbuf) {
            snprintf(s_start_err, sizeof(s_start_err),
                     "jbuf PSRAM alloc failed");
            ESP_LOGE(TAG, "start: jitter buffer PSRAM alloc failed");
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGI(TAG, "jitter buffer in PSRAM: %u bytes at %p",
                 (unsigned)((size_t)REL_JBUF_SLOTS * REL_AUDIO_OPUS), (void *)s_jbuf[0]);
    }

    if (!s_rx_mux) { s_rx_mux = xSemaphoreCreateMutex(); }
    if (!s_rx_mux) { return ESP_ERR_NO_MEM; }
    snprintf(s_uri, sizeof(s_uri), "mqtt://%s", netcom_broker_str());
    ESP_LOGI(TAG, "connecting to broker %s as %s", s_uri, device_id());

    esp_mqtt_client_config_t mcfg = {
        .broker.address.uri = s_uri,
        .session.keepalive = 30,
        /*
         * The broker is on the LAN: a TCP connect either completes in
         * milliseconds or the broker is down.  The IDF defaults (10 s
         * operation timeout, 10 s reconnect delay) make a dead broker cost
         * ~20 s of silence per attempt; tighten both for a snappy relay.
         */
        /*
         * 4 KB.  A larger buffer (16 KB) was measured and changed nothing: the
         * out_size must hold one batched publish (REL_TXBATCH * REL_PKT_LEN =
         * 2840 B); size (the receive buffer, default 1024) must hold the same,
         * otherwise esp-mqtt hands the payload over in 1024-byte fragments and
         * the receiver cannot split it back into whole frames (symptoms:
         * badlen climbing, most audio dropped, sender looks fine).
         */
        .session.protocol_ver = MQTT_PROTOCOL_V_5,
        .buffer.size = 2048,
        .buffer.out_size = 16384,
        /*
         * A subscriber can legitimately see seconds of silence (the far end
         * stopped talking, or the broker is waiting on our own receive window).
         * 3 s made esp-mqtt abort with "Network timeout while reading MQTT
         * message" and reconnect every ~10 s during a live transmission; the
         * keepalive below is what actually detects a dead link.
         */
        .network.timeout_ms = 20000,
        .network.reconnect_timeout_ms = 2000,
    };
    /*
     * Broker authentication.  Anonymous stays the default: with no user set the
     * CONNECT packet carries no credentials and any open broker still works.
     * The pointers are safe to keep -- netcom's config is static storage and
     * esp-mqtt copies what it needs.
     */
    if (cfg && cfg->user[0]) {
        mcfg.credentials.username = cfg->user;
        mcfg.credentials.authentication.password = cfg->pass;
        ESP_LOGI(TAG, "broker auth: username '%s', password %s",
                 cfg->user, cfg->pass[0] ? "set" : "empty");
    }
    if (!s_txq) {
        s_txq = xQueueCreateWithCaps(REL_TXQ_DEPTH, sizeof(rel_tx_item_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_txq) {
            /* Internal RAM fallback: without the queue the audio task would
             * have to publish inline again, which is what this change removes. */
            s_txq = xQueueCreateWithCaps(REL_TXQ_DEPTH, sizeof(rel_tx_item_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
    }
    if (!s_pub_mux) {
        s_pub_mux = xSemaphoreCreateMutex();
    }
    s_client = esp_mqtt_client_init(&mcfg);
    if (!s_client) {
        snprintf(s_start_err, sizeof(s_start_err), "mqtt client init failed");
        ESP_LOGE(TAG, "mqtt client init failed for %s", s_uri);
        (void)relay_stop();
        return ESP_ERR_NO_MEM;
    }
    if (esp_mqtt_client_register_event(s_client, MQTT_EVENT_ANY,
                                       relay_mqtt_handler, NULL) != ESP_OK) {
        snprintf(s_start_err, sizeof(s_start_err), "mqtt event register failed");
        (void)relay_stop();
        return ESP_FAIL;
    }

    rx_lock();
    memset(s_jbuf_valid, 0, sizeof(s_jbuf_valid));
    s_have_latest = false;
    s_have_first = false;
    s_play_head_valid = false;
    opus_link_dec_free(s_opus_dec); s_opus_dec = NULL;
    memset(s_opus_dec_mac, 0, sizeof(s_opus_dec_mac));
    rx_unlock();
    memset(s_peers, 0, sizeof(s_peers));
    s_have_latest = false;
    s_play_head_valid = false;

    /* Created before s_started flips, so every tx_sample/tx_end sees a valid
     * handle; the engine keeps it for its whole lifetime. */
    if (!s_tx_mux) {
        s_tx_mux = xSemaphoreCreateMutex();
    }
    tx_lock();
    s_tx_acc_n = 0;
    s_tx_seq = 0;
    tx_unlock();
    s_rx_msgs = 0;
    s_rx_dropped = 0;
    s_tx_msgs = 0;
    s_tx_errs = 0;
    s_last_rx_ms = 0;
    s_last_tx_ms = 0;
    s_connected = false;
    s_amp_on = false;
    s_last_play_ms = 0;
    s_rx_ptt = false;

    if (ptt_owned(PTT_OWNER_NETWORK, 1)) {
        ptt_bridge_key(false);
    }
    board_amp_enable(false);

    if (!s_txq) { (void)relay_stop(); return ESP_ERR_NO_MEM; }
    s_tx_stop = false;
    s_started = true;
    s_start_ms = now_ms();
    s_attempts = 0;
    s_last_err[0] = '\0';
    s_play_stop = false;
    if (esp_mqtt_client_start(s_client) != ESP_OK) {
        snprintf(s_start_err, sizeof(s_start_err), "mqtt client start failed");
        (void)relay_stop(); return ESP_FAIL;
    }
    if (xTaskCreateWithCaps(relay_tx_task, "rel_tx", 16384, NULL, 5,
                            &s_tx_task, MALLOC_CAP_SPIRAM) != pdPASS) {
        snprintf(s_start_err, sizeof(s_start_err), "tx task create failed");
        (void)relay_stop(); return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCoreWithCaps(relay_play_task, "netrel", 16384, NULL, 8,
                                &s_play_task, 1, MALLOC_CAP_SPIRAM) != pdPASS) {
        snprintf(s_start_err, sizeof(s_start_err), "play task create failed");
        (void)relay_stop();
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "relay engine up: broker %s (FMO-style, QoS 0), topic %s/tx/+",
             s_uri, s_topic_root);
    return ESP_OK;
}

static esp_err_t relay_stop(void)
{
    tx_lock(); s_started = false; s_tx_stop = true; tx_unlock();
    s_play_stop = true;
    if (ptt_owned(PTT_OWNER_NETWORK, 1)) { ptt_bridge_key(false); }
    for (int i = 0; i < 1000 && (s_play_task || s_tx_task || s_test_tone_task); ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_play_task || s_tx_task || s_test_tone_task) {
        ESP_LOGE(TAG, "workers did not stop; retaining MQTT client and buffers");
        return ESP_ERR_TIMEOUT;
    }
    board_amp_enable(false);
    s_amp_on = false;
    if (s_client) {
        (void)esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
    s_connected = false;
    tx_lock();
    if (s_txq) { vQueueDeleteWithCaps(s_txq); s_txq = NULL; }
    tx_unlock();
    rx_lock();
    opus_link_dec_free(s_opus_dec); s_opus_dec = NULL;
    memset(s_jbuf_valid, 0, sizeof(s_jbuf_valid));
    if (s_jbuf) { heap_caps_free(s_jbuf); s_jbuf = NULL; }
    s_have_latest = s_have_first = s_play_head_valid = false;
    s_rx_ptt = false; s_last_play_ms = 0;
    rx_unlock();
    ESP_LOGI(TAG, "relay engine down (sent %u / recv %u msgs, %u errors)",
             (unsigned)s_tx_msgs, (unsigned)s_rx_msgs, (unsigned)s_tx_errs);
    return ESP_OK;
}

static void relay_stats(uint32_t *tx_msgs, uint32_t *rx_msgs,
                        int *peers, const char **state)
{
    *tx_msgs = s_tx_msgs;
    *rx_msgs = s_rx_msgs;
    rx_lock();
    *peers = peer_count();
    rx_unlock();
    if (!s_started) {
        *state = "stopped";
        return;
    }
    const int64_t now = now_ms();
    if (s_last_tx_ms && now - s_last_tx_ms < 500) {
        *state = "tx";
    } else if (s_last_rx_ms && now - s_last_rx_ms < 500) {
        *state = "rx";
    } else if (s_connected) {
        *state = "idle";
    } else {
        *state = "connecting";
    }
}

static void relay_diag(char *out, size_t n)
{
    out[0] = '\0';
    if (!s_started) {
        if (s_start_err[0]) {
            snprintf(out, n, "start failed: %s", s_start_err);
        }
        return;
    }
    if (s_connected) {
        snprintf(out, n, "connected, ev=%u ok=%u badlen=%u badmagic=%u self=%u starve=%u qdrop=%u late=%u plc=%u prime=%u first=%u play=%u",
                 (unsigned)s_ev_total, (unsigned)s_ev_ok, (unsigned)s_ev_badlen,
                 (unsigned)s_ev_badmagic, (unsigned)s_ev_self, (unsigned)s_starves,
                 (unsigned)s_tx_qdrop, (unsigned)s_late, (unsigned)s_plc,
                 (unsigned)s_primes, (unsigned)s_prime_first, (unsigned)s_prime_play);
        return;
    }
    const int up_s = (int)((now_ms() - s_start_ms) / 1000);
    snprintf(out, n, "trying %d, %d s up, %s", s_attempts, up_s,
             s_last_err[0] ? s_last_err : "no error yet");
}

static const netcom_engine_t s_engine = {
    .name             = "mqtt-relay",
    .start            = relay_start,
    .stop             = relay_stop,
    .tx_sample        = relay_tx_sample,
    .tx_end           = relay_tx_end,
    .rx_sample        = NULL,
    .stats            = relay_stats,
    .on_room_change   = NULL,   /* topic no longer carries a room */
    .diag             = relay_diag,
    .test_tone        = relay_test_tone,
};

uint32_t netrelay_tx_qdrop(void)
{
    return s_tx_qdrop;
}

void netrelay_init(void)
{
    netcom_register_engine(NETCOM_MODE_RELAY, &s_engine);
}
