"""Run the production SDR PTT and release loop with a simulated clock and outputs."""
import os
from pathlib import Path
import subprocess
import tempfile

from test_np_peer_runtime import function


ROOT = Path(__file__).resolve().parents[1]
SIMULATION = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SDR_RELEASE_MAX_US 6000000
#define SDR_RELEASE_AUDIO_US 1100000
#define SDR_PTT_HOLD_MAX_MS 30000
#define SDR_NO_PCM_GRACE_MS 1500
#define SDR_TX_STALL_MS 1200
#define SDR_WS_STALE_MS 6000
#define SDR_NO_CLIENT_STALE_MS 30000
#define AUDIO_I2S_BUFFER_MS 48
#define AUDIO_SAMPLE_RATE_HZ 16000
#define SDR_PKT_SAMPLES 320
#define SDR_TX_RING_SAMPLES 8192
#define SDR_TX_APPEND UINT32_MAX
#define NETCOM_MODE_LOCAL 0
#define PTT_OWNER_NONE 0
#define PTT_OWNER_SDR 1
#define portMAX_DELAY 0
#define portENTER_CRITICAL(lock) ((void)0)
#define portEXIT_CRITICAL(lock) ((void)0)

static int64_t clock_us = 100000;
static int64_t s_client_us, s_release_started_us, s_ptt_started_us, s_pcm_us, s_ws_last_us;
static int64_t s_tail_drain_us, s_net_tail_due_us;
static uint32_t s_tx_id, s_cancelled_tx, owner, tail_done, s_last_tx_ms, s_last_rel_ms;
static uint32_t s_session = 7, s_tail_upload_sid;
static bool s_on = true;
static bool s_limit_release;
static int64_t speech_deadline;
static unsigned timeout_ms = 5000;
static uint32_t s_rel_total, s_rel_tail, s_rel_notail, s_rel_pcm_timeout, s_rel_force;
static uint32_t s_rel_tail_timeout, s_rel_released_notail, s_owner_mismatch;
static uint32_t s_begin_busy, s_off_stale, s_off_reject, s_on_reject;
static uint32_t s_tx_pkts, s_tx_samples, s_tx_dry;
static uint32_t s_tx_received_samples, s_tx_rejected_samples, s_tx_routed_samples;
static uint32_t s_tx_rf_submitted, s_tx_net_submitted, s_tx_rf_abandoned, s_tx_net_abandoned;
static bool s_ptt_hold, s_releasing, s_tail_started, s_net_tx, s_rf_tx, s_pcm_ever;
static bool s_rf_tail_started;
static bool s_last_had_pcm, s_last_tail, s_ws_dead, keyed, allow_key = true;
static bool tone_configured = true, rf_on_network, rf_pending, rf_room = true, network_room = true;
static unsigned tone_starts, network_ends;
static size_t network_tail_count, s_tail_samples, s_net_tail_pos;
static uint32_t s_tx_raw_peak, s_tx_processed_peak, s_tx_processed_clip;
static int16_t rf_pcm[SDR_TX_RING_SAMPLES], network_pcm[SDR_TX_RING_SAMPLES];
static size_t rf_count, network_count;
static int mode, s_key = 1, s_ws_fd = -1, s_monitor;
static size_t s_tx_head, s_tx_ready, s_tx_rf_read, s_tx_net_read;
static int16_t s_tx_ring[SDR_TX_RING_SAMPLES];
static const char *s_end_reason, *s_release_cause, *s_rf_end, *s_net_end;
static char s_last_reason[24];

static void strlcpy(char *out, const char *value, size_t n) { snprintf(out, n, "%s", value); }
static int64_t esp_timer_get_time(void) { return clock_us; }
static void xSemaphoreTake(int sem, int wait) { (void)sem; (void)wait; }
static void xSemaphoreGive(int sem) { (void)sem; }
static bool session_locked(uint32_t sid) { return s_on && sid == s_session; }
static bool ptt_owned(int source, uint32_t token) { return source == PTT_OWNER_SDR && owner && owner == token; }
static bool ptt_session_start(int source,uint32_t token) {
    speech_deadline = clock_us + (int64_t)timeout_ms * 1000;
    return ptt_owned(source,token);
}
static bool ptt_speech_expired(int source,uint32_t token){return ptt_owned(source,token) && clock_us >= speech_deadline;}
static void ptt_begin_release(int source,uint32_t token){assert(ptt_owned(source,token));}
static int ptt_owner(void) { return owner ? PTT_OWNER_SDR : PTT_OWNER_NONE; }
static uint32_t ptt_owner_token(void) { return owner; }
static bool voice_sdr_begin(uint32_t tx) { if (owner) return false; owner = tx; return true; }
static bool ptt_source_key(int source, uint32_t tx, bool on) {
    if (!allow_key || !ptt_owned(source, tx)) return false;
    keyed = on;
    return true;
}
static void voice_sdr_end(uint32_t tx) { if (owner == tx) { owner = 0; keyed = false; } }
static void voice_sdr_rf_end(uint32_t tx) { if (owner == tx) { keyed = false; } }
static void monitor_rx_local_tx(int *monitor, bool tx) { *monitor = tx; }
static int netcom_mode(void) { return mode; }
static bool netcom_rf_tx(void) { return rf_on_network; }
static bool netcom_tx_ready(void) { return true; }
static bool netcom_tx_room(size_t samples, bool tail, int64_t expires) {
    (void)samples; (void)tail; (void)expires; return network_room;
}
static int64_t ptt_release_deadline_us(int source, uint32_t token) {
    assert(ptt_owned(source, token)); return speech_deadline + 6000000;
}
static void netcom_tx_end(void) { ++network_ends; }
static void bridge_rx_inhibit(void) {}
static size_t voice_bridge_level(void) { return rf_room ? 0 : 8000; }
static int voice_apply_net_tx_processing(int16_t *pcm, size_t n) { (void)pcm; (void)n; return 0; }
static void voice_sdr_inject_processed(uint32_t token, const int16_t *pcm, size_t n) {
    assert(ptt_owned(PTT_OWNER_SDR, token) && rf_count + n <= SDR_TX_RING_SAMPLES);
    memcpy(rf_pcm + rf_count, pcm, n * sizeof(*pcm));
    rf_count += n;
}
static bool netcom_tx_sample_until(const int16_t *pcm, size_t n, bool tail, int64_t expires) {
    assert(network_room && expires > clock_us);
    if (tail) { network_tail_count += n; return true; }
    assert(network_count + n <= SDR_TX_RING_SAMPLES);
    memcpy(network_pcm + network_count, pcm, n * sizeof(*pcm));
    network_count += n;
    return true;
}
static bool voice_sdr_drain_pcm(uint32_t tx) { assert(ptt_owned(PTT_OWNER_SDR,tx)); return rf_pending; }
static size_t voice_sdr_tail_samples(void) { return tone_configured ? 80000 : 0; }
static size_t voice_sdr_tail_read(size_t offset, int16_t *pcm, size_t n) {
    assert(offset + n <= voice_sdr_tail_samples());
    memset(pcm, 0, n * sizeof(*pcm));
    return n;
}
static bool voice_sdr_tail_start(uint32_t tx) {
    assert(ptt_owned(PTT_OWNER_SDR, tx) && s_tx_rf_read == s_tx_head);
    if (tone_configured) ++tone_starts;
    return tone_configured;
}
static uint32_t voice_sdr_tail_done(void) {
    const uint32_t done = tail_done;
    tail_done = 0;
    return done;
}
"""

TEST = r"""
static void browser_markers(uint32_t sid, uint32_t token, size_t n, int16_t marker) {
    uint8_t pcm[1280];
    assert(n <= sizeof(pcm) / 2);
    for (size_t i = 0; i < n; ++i) {
        pcm[2*i] = (uint8_t)marker;
        pcm[2*i+1] = (uint8_t)((uint16_t)marker >> 8);
    }
    assert(sdr_tx_push_bytes(sid, token, pcm, n * 2));
}

static void network_tail_finish(void) {
    for (size_t n = 0; n < voice_sdr_tail_samples(); n += SDR_PKT_SAMPLES) {
        clock_us += 20000;
        sdr_tx_step();
    }
}

int main(void)
{
    const uint32_t sid = 7;
    s_ws_fd = 9;
    sdr_tail_upload_pause(true);
    clock_us += 31000000;
    sdr_tx_step();
    assert(s_ws_fd == 9 && !s_ws_dead && s_tail_upload_sid == sid);
    sdr_tail_upload_pause(false);
    assert(!s_tail_upload_sid && s_ws_last_us == clock_us && s_client_us == clock_us);
    sdr_tx_step();
    assert(s_ws_fd == 9 && !s_ws_dead);
    s_ws_fd = -1;
    assert(!sdr_ptt_request(sid + 1, 1, true));
    assert(sdr_ptt_request(sid, 1, true) && keyed);
    s_tx_head = 200; /* below normal playback prefill */
    assert(sdr_ptt_request(sid, 1, false) && s_releasing);
    sdr_tx_step();
    assert(rf_count == 200 && tone_starts == 1 && keyed);
    assert(!sdr_ptt_request(sid, 2, true));
    assert(strcmp(s_end_reason, "tail_pending") == 0);
    assert(sdr_ptt_request(sid, 2, false));
    assert(sdr_ptt_request(sid, 1, false));
    assert(s_tx_id == 1 && s_cancelled_tx == 2 && owner == 1 && keyed);
    assert(s_rel_total == 0 && !network_ends);
    clock_us += 5000000; /* longest supported clip */
    tail_done = 1;
    sdr_tx_step();
    assert(keyed && s_tail_started);
    clock_us += AUDIO_I2S_BUFFER_MS * 1000 - 1;
    sdr_tx_step();
    assert(keyed);
    ++clock_us;
    sdr_tx_step();
    assert(!keyed && !owner && s_rel_tail == 1 && s_last_tail);
    assert(!s_rel_tail_timeout && !s_owner_mismatch && !s_rel_force);
    assert(!sdr_ptt_request(sid, 2, true));

    mode = 1; /* network only */
    assert(sdr_ptt_request(sid, 3, true) && !keyed);
    assert(sdr_ptt_request(sid, 4, false) && s_ptt_hold && owner == 3);
    assert(!sdr_ptt_request(sid, 1, false) && s_ptt_hold);
    assert(sdr_ptt_request(sid, 3, false));
    sdr_tx_step();
    assert(s_tail_started && tone_starts == 1 && !network_ends && network_tail_count == 320);
    network_tail_finish();
    assert(network_tail_count == 80000);
    assert(!s_tail_started && !owner && network_ends == 1 && s_rel_tail == 2);
    assert(!sdr_ptt_request(sid, 4, true));

    tone_configured = false;
    assert(sdr_ptt_request(sid, 5, true));
    assert(sdr_ptt_request(sid, 5, false));
    sdr_tx_step();
    assert(!owner && !s_releasing && network_ends == 2 && tone_starts == 1);
    assert(s_rel_notail == 1 && s_rel_released_notail == 1);
    tone_configured = true;
    assert(sdr_ptt_request(sid, 6, true));
    clock_us = speech_deadline - 1000;
    s_client_us = s_pcm_us = clock_us;
    s_pcm_ever = true;
    sdr_tx_step();
    assert(s_ptt_hold && !s_releasing && tone_starts == 1);
    clock_us = speech_deadline;
    sdr_tx_step();
    assert(!s_ptt_hold && s_releasing && owner == 6);
    sdr_tx_step();
    assert(s_tail_started && tone_starts == 1 && network_ends == 2);
    network_tail_finish();
    assert(!owner && !s_releasing && network_ends == 3 && !strcmp(s_last_reason, "hold_limit"));

    mode = 0; tone_configured = false; rf_pending = true;
    assert(sdr_ptt_request(sid, 7, true) && keyed);
    s_tx_head += 180; /* short utterance can still be below the 300 ms prefill */
    assert(sdr_ptt_request(sid, 7, false));
    sdr_tx_step();
    assert(keyed && s_releasing && s_tx_routed_samples == 180 && !s_tail_drain_us);
    clock_us += 16000; sdr_tx_step(); assert(keyed);
    rf_pending = false; sdr_tx_step(); assert(keyed && s_tail_drain_us);
    clock_us += AUDIO_I2S_BUFFER_MS * 1000 - 1; sdr_tx_step(); assert(keyed);
    ++clock_us; sdr_tx_step(); assert(!keyed && !owner);

    mode = 1;
    assert(sdr_ptt_request(sid, 8, true));
    s_tx_head += 320; network_room = false;
    sdr_tx_step(); assert(tx_level_locked() == 320 && s_tx_routed_samples == 320 && !s_tx_net_submitted);
    network_room = true; sdr_tx_step(); assert(tx_level_locked() == 0 && s_tx_routed_samples == 320);
    assert(sdr_ptt_request(sid, 8, false)); sdr_tx_step(); assert(!owner);

    /* Same 640/640/480 sample payload as the browser HTTP/WS fixture. The
     * last short packet is still at ingress when the OFF request arrives. */
    rf_count = network_count = 0;
    rf_on_network = true;
    assert(sdr_ptt_request(sid, 9, true));
    browser_markers(sid, 9, 640, 111);
    browser_markers(sid, 9, 640, 222);
    browser_markers(sid, 9, 480, 333);
    assert(sdr_ptt_request(sid, 9, false));
    uint8_t late[2] = {0};
    assert(!sdr_tx_push_bytes(sid, 9, late, sizeof(late)));
    network_room = false;
    sdr_tx_step();
    assert(tx_level_locked() == 1760);
    network_room = true;
    for (int i = 0; i < 6; ++i) { clock_us += 5000; sdr_tx_step(); }
    assert(s_tx_received_samples == 1760 && s_tx_routed_samples == 1760 && !s_tx_rejected_samples);
    assert(rf_count == 1760 && network_count == 1760 && keyed);
    assert(!memcmp(rf_pcm, network_pcm, rf_count * sizeof(*rf_pcm)));
    for (size_t i = 0; i < rf_count; ++i) {
        assert(rf_pcm[i] == (i < 640 ? 111 : i < 1280 ? 222 : 333));
    }
    clock_us += 48000; sdr_tx_step(); assert(!keyed && !owner);

    /* A full ingress ring accepts no part of the rejected message. */
    rf_count = network_count = 0;
    assert(sdr_ptt_request(sid, 10, true));
    for (size_t i = 0; i < 12; ++i) { browser_markers(sid, 10, 640, (int16_t)(100 + i)); }
    uint8_t excess[1280] = {0};
    assert(!sdr_tx_push_bytes(sid, 10, excess, sizeof(excess)));
    assert(s_releasing && s_tx_rejected_samples == 640 && tx_level_locked() == 7680);
    for (int i = 0; i < 24; ++i) { clock_us += 5000; sdr_tx_step(); }
    assert(s_tx_routed_samples == 7680 && rf_count == 7680 && network_count == 7680);
    for (size_t i = 0; i < rf_count; ++i) { assert(rf_pcm[i] == (int16_t)(100 + i / 640)); }
    clock_us += 48000; sdr_tx_step(); assert(!keyed && !owner && !strcmp(s_last_reason, "pcm_overflow"));
    release_locked("disabled");
    assert(!strcmp(s_last_reason, "pcm_overflow") && s_last_had_pcm);

    /* A blocked network can hold its own last speech/tail but cannot hold RF. */
    rf_count = network_count = 0;
    assert(sdr_ptt_request(sid, 11, true) && keyed);
    network_room = false;
    browser_markers(sid, 11, 320, 444);
    assert(sdr_ptt_request(sid, 11, false));
    sdr_tx_step();
    clock_us += 48000; sdr_tx_step();
    assert(!keyed && owner == 11 && rf_count == 320 && network_count == 0);
    network_room = true; sdr_tx_step();
    assert(!owner && network_count == 320 && !strcmp(s_rf_end, "released"));

    /* Conversely, net OFF follows its full tail while RF still cannot take PCM. */
    assert(sdr_ptt_request(sid, 12, true) && keyed);
    rf_room = false; tone_configured = true; network_tail_count = 0;
    browser_markers(sid, 12, 320, 555);
    assert(sdr_ptt_request(sid, 12, false));
    sdr_tx_step(); network_tail_finish();
    assert(keyed && owner == 12 && !s_net_tx && network_tail_count == 80000);
    clock_us += 1000000; sdr_tx_step();
    assert(!owner && !keyed && s_tx_rf_abandoned == 320 && !strcmp(s_rf_end, "tail_timeout"));
    assert(!strcmp(s_net_end, "released"));

    /* A replay after a broken POST includes bytes the first attempt queued. */
    mode = 1; rf_on_network = false; tone_configured = false;
    network_count = 0;
    uint8_t retry[800];
    for (size_t i = 0; i < 400; ++i) { retry[2*i] = (uint8_t)i; retry[2*i+1] = 1; }
    assert(sdr_ptt_request(sid, 13, true));
    assert(sdr_tx_push_bytes_at(sid, 13, retry, 512, 0));
    assert(s_tx_received_samples == 256);
    assert(!sdr_tx_push_bytes_at(sid, 13, retry, 64, 320));
    assert(sdr_tx_push_bytes_at(sid, 13, retry, sizeof(retry), 0));
    assert(sdr_tx_push_bytes_at(sid, 13, retry, sizeof(retry), 0));
    assert(s_tx_received_samples == 400 && !s_tx_rejected_samples);
    assert(sdr_ptt_request(sid, 13, false));
    sdr_tx_step(); sdr_tx_step();
    assert(!owner && network_count == 400);
    for (size_t i = 0; i < 400; ++i) { assert(network_pcm[i] == 256 + (uint8_t)i); }
    puts("PASS: SDR configured cutoff stops capture, sends a single tail and then closes the network over");
    puts("PASS: SDR short/long release, last DAC buffer, network end, duplicate/late OFF and blocked late ON");
    puts("PASS: 1760 browser samples route intact to RF and network on release; overflow preserves 7680 accepted samples");
    puts("PASS: RF unkeys independently; network sends the full tail and OFF independently");
    puts("PASS: broken request and duplicate retry preserve exactly 400 ordered samples");
    return 0;
}
"""


def main():
    source = (ROOT / "firmware/main/sdr.c").read_text("utf-8")
    names = ("tx_ring_write", "tx_ring_read", "tx_level_locked", "stop_net_locked", "stop_rf_locked",
             "release_locked", "request_release_locked", "finish_release_locked",
             "sdr_tail_upload_pause", "start_tx_locked", "sdr_ptt_request", "sdr_tx_push_bytes_at", "sdr_tx_push_bytes",
             "sdr_pcm_peak", "sdr_process_tx_pcm", "sdr_tx_drain_locked", "mark_tail_locked",
             "finish_rf_locked", "finish_net_locked", "sdr_tx_step")
    code = SIMULATION + "\n".join(function(source, name) for name in names) + TEST
    scratch = ROOT / ".tmp"
    scratch.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="sdr-tail-", dir=scratch) as temp:
        work = Path(temp)
        program = work / "regression.c"
        exe = work / "regression.exe"
        program.write_text(code, encoding="utf-8")
        env = dict(os.environ, TEMP=temp, TMP=temp, TMPDIR=temp)
        subprocess.run(
            ["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(program), "-o", str(exe)],
            check=True, env=env)
        subprocess.run([str(exe)], cwd=work, check=True, env=env)


if __name__ == "__main__":
    main()
