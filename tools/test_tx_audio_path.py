"""Run the production bridge, SpeexDSP and both network TX workers on marked PCM."""

from pathlib import Path
import re
import subprocess
import tempfile

from test_np_peer_runtime import function
from test_rf_denoise import compile_executable


ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "firmware/main"

ENV = r'''
#include <assert.h>
#include <stdint.h>
#define audio_spectrum_tap(...) ((void)0)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bridge.h"
#include "net_rx_order.h"
#include "net_tx_pcm.h"
#include "opus_link.h"
#include "ptt.h"
#include "tx_timeout.h"

#define AUDIO_PKT_SAMPLES OPUS_LINK_SAMPLES
#define AUDIO_SAMPLE_RATE_HZ OPUS_LINK_RATE_HZ
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define NP_PKT_SAMPLES OPUS_LINK_SAMPLES
#define NP_AUDIO_OPUS 80
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
#define portMAX_DELAY 0
#define RX_GUARD_OUT_NET 1
#define TAIL_OFF 0
#define ESP_LOGI(tag, ...) ((void)0)
#define ESP_LOGW(tag, ...) ((void)0)
#define ESP_LOGE(tag, ...) ((void)0)

static int64_t clock_us = 1000000;
int64_t esp_timer_get_time(void) { return clock_us; }
static int64_t now_ms(void) { return clock_us / 1000; }
static bool s_started = true, s_tx_open, s_tx_acc_tail, s_tx_epoch_open, s_tx_stop;
static int64_t s_tx_acc_expiry, s_last_tx_ms;
static int s_tx_acc_n;
static int16_t s_tx_acc[AUDIO_PKT_SAMPLES];
static uint16_t s_tx_seq, s_tx_epoch;
static uint32_t s_tx_msgs, s_tx_errs;
static void *s_tx_task;
static net_tx_pcm_counters_t s_tx_audio;
static void tx_lock(void) {}
static void tx_unlock(void) {}
static void vTaskDeleteWithCaps(void *task) { assert(!task); }
static unsigned sent_ends, failed_ends;
void netcom_note_end_sent(int64_t ms, bool ok) {
    assert(ms > 0 && ms <= now_ms());
    if (ok) { ++sent_ends; } else { ++failed_ends; }
}
void *heap_caps_malloc(size_t n, unsigned caps) { (void)caps; return malloc(n); }
void *heap_caps_calloc(size_t n, size_t sz, unsigned caps) { (void)caps; return calloc(n, sz); }
void *heap_caps_aligned_alloc(size_t alignment, size_t n, unsigned caps) {
    void *p = heap_caps_malloc(n, caps); assert((uintptr_t)p % alignment == 0); return p;
}
void heap_caps_free(void *p) { free(p); }

/* Keep the exact PCM passed to the encoder. Use a reversible frame index at
 * the codec/socket boundary so sample identity survives transport fixtures. */
typedef struct {
    int16_t pcm[AUDIO_PKT_SAMPLES];
    uint16_t seq, epoch;
    bool ptt, tail;
} wire_frame_t;
static wire_frame_t encoded[256], wire[256];
static size_t encoded_count, wire_count;
static net_rx_order_t receiver;
static bool fail_send, fail_encode;
static opus_link_enc_t *s_opus_enc;
opus_link_enc_t *opus_link_enc_new(void) { return (opus_link_enc_t *)&encoded_count; }
void opus_link_enc_free(opus_link_enc_t *enc) { (void)enc; }
int opus_link_encode(opus_link_enc_t *enc, const int16_t *pcm, uint8_t *out, size_t cap) {
    assert(enc && cap > 2 && encoded_count < 256);
    if (fail_encode) { return -1; }
    memcpy(encoded[encoded_count].pcm, pcm, sizeof(encoded[0].pcm));
    out[0] = (uint8_t)encoded_count++;
    return (int)cap;
}
static void deliver(uint16_t seq, uint16_t epoch, bool ptt, bool tail, const uint8_t *payload) {
    assert(wire_count < 256 && payload[0] < encoded_count && epoch);
    if (wire_count) { assert(seq == (uint16_t)(wire[wire_count - 1].seq + 1)); }
    assert(net_rx_order_accept(&receiver, epoch, seq, ptt, now_ms()) >= NET_RX_FORWARD);
    wire[wire_count] = encoded[payload[0]];
    wire[wire_count].seq = seq;
    wire[wire_count].epoch = epoch;
    wire[wire_count].ptt = ptt;
    wire[wire_count++].tail = tail;
}
'''

QUEUE = r'''
typedef struct { tx_item_t frames[TX_DEPTH]; size_t head, count; } queue_t;
static queue_t backing;
static queue_t *TX_QUEUE = &backing;
static size_t uxQueueSpacesAvailable(queue_t *q) { return TX_DEPTH - q->count; }
static size_t uxQueueMessagesWaiting(queue_t *q) { return q->count; }
static int xQueueSend(queue_t *q, const tx_item_t *item, unsigned wait) {
    assert(!wait);
    if (q->count == TX_DEPTH) { return 0; }
    q->frames[(q->head + q->count++) % TX_DEPTH] = *item;
    return pdTRUE;
}
static int xQueueReceive(queue_t *q, tx_item_t *item, unsigned wait) {
    (void)wait;
    if (!q->count) { s_tx_stop = true; return 0; }
    *item = q->frames[q->head++ % TX_DEPTH];
    --q->count;
    return pdTRUE;
}
'''

P2P = r'''
#define SLOT_MAX 1
#define F_AUDIO 4
typedef struct { bool in_use, selftest; } rt_peer_t;
static rt_peer_t s_peer[SLOT_MAX] = {{true, false}};
static bool s_tx_tail_frame;
static atomic_uint s_tx_qdrop;
static uint8_t s_enc_buf[NP_AUDIO_OPUS];
static void peer_lock(void) {}
static void peer_unlock(void) {}
static bool peer_addr_set(const rt_peer_t *p) { return p == s_peer; }
static bool frame_send_to(rt_peer_t *p, int kind, uint16_t seq, bool ptt, const uint8_t *payload) {
    assert(p == s_peer && kind == F_AUDIO);
    if (fail_send) { return false; }
    deliver(seq, s_tx_epoch, ptt, s_tx_tail_frame, payload);
    return true;
}
'''

MQTT = r'''
static uint32_t s_tx_qdrop;
static bool s_connected = true;
static void *s_pub_mux, *s_client = &s_connected;
static char s_my_topic[48];
static uint8_t s_mac[6];
static const char *s_topic_root = "fixture";
static size_t largest_publish;
static const char *device_id(void) { return "tx"; }
static void xSemaphoreTake(void *mux, int wait) { (void)mux; (void)wait; }
static void xSemaphoreGive(void *mux) { (void)mux; }
static void radio_led_note_net_tx(int mode) { assert(mode == MODE); }
static int esp_mqtt_client_publish(void *client, const char *topic, const char *payload,
                                   int size, int qos, int retain) {
    assert(client && !strcmp(topic, "fixture/tx/tx") && !qos && !retain);
    assert(size % REL_PKT_LEN == 0 && size <= REL_TXBATCH * REL_PKT_LEN);
    if ((size_t)size > largest_publish) { largest_publish = (size_t)size; }
    if (fail_send) { return -1; }
    for (int off = 0; off < size; off += REL_PKT_LEN) {
        rel_header_t h;
        memcpy(&h, payload + off, sizeof(h));
        assert(h.magic == REL_MAGIC && (h.flags & NET_RX_FLAG_EPOCH));
        deliver(h.seq, h.reserved, h.flags & 1, h.flags & NET_RX_FLAG_TAIL,
                (const uint8_t *)payload + off + REL_HDR_LEN);
    }
    return 0;
}
'''

BRIDGE = r'''
static bridge_rx_stats_t s_rx;
static bool s_active = true;
static uint32_t s_rx_generation, s_denoise_config;
static uint8_t s_rx_mode, s_rx_denoise_mode;
static uint16_t s_rx_room;
static int s_rx_chan, s_chan = 1, s_rx_tail_id;
static size_t s_rx_tail_pos;
static int64_t s_rx_tail_next_us;
static bool s_rx_limited, s_rx_room_changing, s_rx_configuring;
static tx_timeout_window_t s_rx_window;

static const netcom_engine_t engine = {.tx_sample=TX_SAMPLE, .tx_room=TX_ROOM, .tx_end=TX_END};
static const netcom_engine_t *active_engine(void) { return &engine; }
static struct { unsigned tx_end_requests; int64_t tx_end_ms; } s_ptt_diag;
#define portENTER_CRITICAL(lock) ((void)0)
#define portEXIT_CRITICAL(lock) ((void)0)

uint8_t netcom_mode(void) { return MODE; }
uint16_t netcom_room(void) { return 23; }
bool netcom_engine_running(void) { return true; }
static bool sa818_cfg_tx_ready(void) { return true; }
ptt_owner_t ptt_owner(void) { return PTT_OWNER_NONE; }
static unsigned rx_guard_blocked(void) { return 0; }
static void rx_tx_lock(void) {}
static void rx_tx_unlock(void) {}
unsigned tx_timeout_seconds(void) { return 5; }
uint32_t bridge_denoise_levels(void) { return s_denoise_config; }
void ptt_trace_note(unsigned kind, unsigned source, uint32_t a, uint32_t b) {
    (void)b; assert((kind == PTT_TRACE_RFNET_ON || kind == PTT_TRACE_RFNET_OFF) && source == MODE && a);
}
static size_t tone_samples;
static int tail_id_for_mode(uint8_t mode) { assert(mode == MODE); return tone_samples ? 1 : 0; }
static size_t tail_samples(int tone) { assert(tone == 1); return tone_samples; }
static size_t tail_fetch(int tone, size_t pos, int16_t *pcm, size_t n) {
    assert(tone == 1 && pos + n <= tone_samples);
    for (size_t i = 0; i < n; ++i) { pcm[i] = 20000; }
    return n;
}
'''

CASES = r'''
extern bool rf_denoise_test_autorun;

static void worker(void) {
    s_tx_stop = false;
    TX_WORKER(NULL);
    assert(!backing.count);
}

static void next_case(void) {
    assert(!backing.count && !s_tx_open && !s_rx.open);
    wire_count = encoded_count = sent_ends = failed_ends = 0;
    receiver = (net_rx_order_t){0};
    clock_us += 100000;
}

static void queue_capacity(void) {
    const int64_t deadline = clock_us + 1000000;
    int16_t pcm[AUDIO_PKT_SAMPLES * 2] = {0};
    for (int i = 0; i < TX_DEPTH - NET_TX_PCM_CLOSE_SLOTS; ++i) {
        pcm[0] = (int16_t)(i + 1);
        assert(netcom_tx_sample_until(pcm, AUDIO_PKT_SAMPLES, false, deadline));
    }
    pcm[0] = 12345;
    assert(netcom_tx_sample_until(pcm, 160, false, deadline));
    const uint32_t before = atomic_load(&s_tx_audio.rejected_samples);
    assert(!netcom_tx_sample_until(pcm, AUDIO_PKT_SAMPLES * 2, false, deadline));
    assert(atomic_load(&s_tx_audio.rejected_samples) - before == AUDIO_PKT_SAMPLES * 2);
    assert(!netcom_tx_room(320, true, deadline));
    netcom_tx_end(); netcom_tx_end();
    assert(backing.count == TX_DEPTH);
    worker();
#ifdef REL_TXBATCH
    assert(largest_publish == REL_TXBATCH * REL_PKT_LEN && largest_publish >= 6 * REL_PKT_LEN);
#endif
    for (int i = 0; i < TX_DEPTH - NET_TX_PCM_CLOSE_SLOTS; ++i) {
        assert(wire[i].pcm[0] == i + 1 && wire[i].ptt && !wire[i].tail);
    }
    assert(wire_count == TX_DEPTH && wire[TX_DEPTH - 2].pcm[0] == 12345);
    assert(wire[TX_DEPTH - 2].ptt && !wire[TX_DEPTH - 1].ptt && sent_ends == 1);
    printf("PASS mode=%d overload: %u whole speech frames and the partial PCM survive, one OFF\n",
           MODE, TX_DEPTH - NET_TX_PCM_CLOSE_SLOTS);
}

static int16_t speech[2560];
static void start_bridge(rf_denoise_level_t level, bool worker_runs) {
    next_case();
    s_denoise_config = (uint32_t)level << ((MODE - 1) * DENOISE_LEVEL_BITS);
    rf_denoise_test_autorun = worker_runs;
    tone_samples = (TX_DEPTH - NET_TX_PCM_CLOSE_SLOTS) * AUDIO_PKT_SAMPLES;
    for (size_t i = 0; i < 2560; i += 256) {
        bridge_rx_tap(speech + i, 256, true, true);
        clock_us += 16000;
    }
    bridge_rx_tap(speech, 256, false, true);
    assert(s_rx.captured_samples == 2560 && s_rx.submitted_samples == 2560 && !s_rx.rejected_samples);
    assert(backing.count == 8 && s_rx_tail_pos == 0);
    worker();
    for (size_t i = 0; i < 8; ++i) {
        assert(wire[i].ptt && !wire[i].tail);
        if (!level || !worker_runs) {
            assert(!memcmp(wire[i].pcm, speech + i * AUDIO_PKT_SAMPLES, sizeof(wire[i].pcm)));
        }
    }
}

static void bridge_levels(void) {
    for (size_t i = 0; i < 2560; ++i) { speech[i] = (int16_t)((i % 31) * 281 - 4000); }
    for (int level = RF_DENOISE_LEVEL_OFF; level <= RF_DENOISE_LEVEL_EXTREME; ++level) {
        for (int healthy = 0; healthy <= 1; ++healthy) {
            start_bridge(level, healthy != 0);
            for (int ticks = 0; s_rx.open && ticks < 400; ++ticks) {
                clock_us += 16000;
                const size_t before = s_rx_tail_pos;
                bridge_rx_tap(speech, 256, false, true);
                assert(!s_rx.open || s_rx_tail_pos - before <= AUDIO_PKT_SAMPLES);
                worker();
            }
            const size_t tails = tone_samples / AUDIO_PKT_SAMPLES;
            assert(!s_rx.open && wire_count == 8 + tails + 1 && sent_ends == 1);
            for (size_t i = 8; i < 8 + tails; ++i) {
                assert(wire[i].ptt && wire[i].tail && wire[i].pcm[0] == 20000);
            }
            assert(!wire[8 + tails].ptt && !wire[8 + tails].tail);
        }
    }
    printf("PASS mode=%d bridge: five levels, healthy/stalled DSP, all 2560 samples before %u ms tail and OFF\n",
           MODE, (unsigned)(tone_samples / 16));
}

static void next_over_during_tail(void) {
    start_bridge(RF_DENOISE_LEVEL_OFF, true);
    for (int i = 0; i < 40; ++i) {
        clock_us += 16000;
        bridge_rx_tap(speech, 256, false, true);
    }
    assert(s_rx.open && backing.count == NET_TX_TAIL_BACKLOG_FRAMES);
    bridge_rx_tap(speech, 256, true, true);
    tone_samples = 0;
    bridge_rx_tap(speech, 256, false, true);
    worker();
    const size_t first_end = 8 + NET_TX_TAIL_BACKLOG_FRAMES;
    assert(wire_count == first_end + 3 && !wire[first_end].ptt && sent_ends == 2);
    assert(wire[first_end + 1].ptt && !wire[first_end + 1].tail && !wire[first_end + 2].ptt);
    assert(wire[first_end].epoch != wire[first_end + 1].epoch);
    assert(!memcmp(wire[first_end + 1].pcm, speech, 256 * sizeof(*speech)));
    printf("PASS mode=%d next COS during stalled tail: old OFF then new speech and OFF\n", MODE);
}

static void worker_errors(void) {
    next_case();
    int16_t pcm[AUDIO_PKT_SAMPLES] = {123};
    const uint32_t expired = atomic_load(&s_tx_audio.expired_frames);
    assert(netcom_tx_sample_until(pcm, AUDIO_PKT_SAMPLES, false, clock_us + 1000));
    netcom_tx_end(); clock_us += 2000; worker();
    assert(atomic_load(&s_tx_audio.expired_frames) == expired + 1 && sent_ends == 1);
    next_case();
    const uint32_t send = atomic_load(&s_tx_audio.send_errors);
    fail_send = true;
    assert(netcom_tx_sample_until(pcm, AUDIO_PKT_SAMPLES, false, clock_us + 1000000));
    netcom_tx_end(); worker(); fail_send = false;
    assert(atomic_load(&s_tx_audio.send_errors) == send + 2 && !wire_count && failed_ends == 1);
    next_case();
    const uint32_t encode = atomic_load(&s_tx_audio.encode_errors);
    fail_encode = true;
    assert(netcom_tx_sample_until(pcm, AUDIO_PKT_SAMPLES, false, clock_us + 1000000));
    netcom_tx_end(); worker(); fail_encode = false;
    assert(atomic_load(&s_tx_audio.encode_errors) == encode + 2 && !wire_count && failed_ends == 1);
    printf("PASS mode=%d expiration, transport and encoder failures are counted independently\n", MODE);
}

int main(void) {
    queue_capacity();
    bridge_levels();
    next_over_during_tail();
    worker_errors();
    return 0;
}
'''


def extracts(source, names):
    return "\n".join(function(source, name) for name in names.split())


def definitions(source, names):
    return "\n".join(re.search(r"^#define[ \t]+" + name + r"\b[^\n]+", source, re.M).group()
                     for name in names.split())


def program(relay):
    mode = "NETCOM_MODE_RELAY" if relay else "NETCOM_MODE_NETP2P"
    source = (MAIN / ("netrelay.c" if relay else "netp2pnet.c")).read_text(encoding="utf-8")
    bridge = (MAIN / "bridge.c").read_text(encoding="utf-8")
    netcom = (MAIN / "netcom.c").read_text(encoding="utf-8")
    prefix, api = ("rel", "relay") if relay else ("np", "netp2pnet")
    depth = "REL_TXQ_DEPTH" if relay else "NP_TXQ_DEPTH"
    depth_def = definitions(source, depth).split("/*")[0]
    room = f"{api if relay else prefix}_tx_room"
    item = re.search(r"typedef struct \{[^{}]+\}\s*" + prefix + r"_tx_item_t;", source).group()
    mapped = f"""
#define TX_DEPTH {depth}
#define TX_QUEUE {'s_txq' if relay else 's_txq_pcm'}
#define TX_SAMPLE {api}_tx_sample
#define TX_ROOM {room}
#define TX_END {api}_tx_end
#define TX_WORKER {api if relay else prefix}_tx_task
typedef {prefix}_tx_item_t tx_item_t;
"""
    if relay:
        packet_defs = definitions(source, "REL_MAGIC REL_HDR_LEN REL_PKT_LEN REL_PKT_SAMPLES REL_AUDIO_OPUS REL_TXBATCH REL_BATCH_WAIT_US")
        packet_type = re.search(r"#pragma pack\(push, 1\).*?#pragma pack\(pop\)", source, re.S).group()
        setup = "\n".join((packet_defs, packet_type, MQTT))
        sender = "relay_tx_task"
    else:
        setup = P2P
        sender = "send_audio_to_all np_tx_task"
    functions = f"{prefix}_enqueue_pcm {api if relay else prefix}_tx_fits {room} {sender} {api}_tx_sample {api}_tx_end"
    return "\n".join((ENV, f"#define MODE {mode}", depth_def, setup, item, mapped, QUEUE, extracts(source, functions), BRIDGE,
        extracts(netcom, "netcom_tx_sample_until netcom_tx_room netcom_tx_end"),
        "#define DENOISE_LEVEL_FIELD ((1u << DENOISE_LEVEL_BITS) - 1u)",
        extracts(bridge, "bridge_denoise_route denoise_level_shift bridge_denoise_level bridge_rx_end_locked "
                         "denoised_route_ready bridge_rx_interrupt_reason bridge_rx_submit_speech_locked denoised_emit "
                         "bridge_rx_tail_step_locked bridge_rx_begin_tail_locked bridge_rx_tap"), CASES))


def main():
    scratch = ROOT / ".tmp"
    scratch.mkdir(exist_ok=True)
    for relay in (False, True):
        with tempfile.TemporaryDirectory(prefix="tx-audio-", dir=scratch) as temp:
            folder = Path(temp)
            (folder / "esp_err.h").write_text("typedef int esp_err_t;\n", encoding="utf-8")
            executable = compile_executable(folder, program(relay))
            subprocess.run([str(executable)], check=True, timeout=30)


if __name__ == "__main__":
    main()
