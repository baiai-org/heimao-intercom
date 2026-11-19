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
#define SDR_RELEASE_NOTONE_US 600000
#define SDR_PTT_HOLD_MAX_MS 30000
#define SDR_NO_PCM_GRACE_MS 1500
#define SDR_TX_STALL_MS 1200
#define SDR_WS_STALE_MS 6000
#define SDR_NO_CLIENT_STALE_MS 30000
#define AUDIO_I2S_BUFFER_MS 48
#define SDR_PKT_SAMPLES 320
#define SDR_TX_RING_SAMPLES 640
#define NETCOM_MODE_LOCAL 0
#define PTT_OWNER_NONE 0
#define PTT_OWNER_SDR 1
#define portMAX_DELAY 0
#define portENTER_CRITICAL(lock) ((void)0)
#define portEXIT_CRITICAL(lock) ((void)0)

static int64_t clock_us = 100000;
static int64_t s_client_us, s_release_started_us, s_ptt_started_us, s_pcm_us, s_ws_last_us;
static int64_t s_tail_drain_us;
static uint32_t s_tx_id, s_cancelled_tx, owner, tail_done, s_last_tx_ms, s_last_rel_ms;
static uint32_t s_rel_total, s_rel_tail, s_rel_notail, s_rel_pcm_timeout, s_rel_force;
static uint32_t s_rel_tail_timeout, s_rel_released_notail, s_owner_mismatch;
static uint32_t s_begin_busy, s_off_stale, s_off_reject, s_on_reject;
static uint32_t s_tx_pkts, s_tx_samples, s_tx_dry;
static bool s_ptt_hold, s_releasing, s_tail_started, s_net_tx, s_rf_tx, s_pcm_ever;
static bool s_last_had_pcm, s_last_tail, s_ws_dead, keyed, allow_key = true;
static bool tone_configured = true, rf_on_network;
static unsigned tone_starts, network_ends, routed;
static int mode, s_key = 1, s_ws_fd = -1, s_monitor;
static size_t s_tx_head, s_tx_tail;
static int16_t s_tx_ring[SDR_TX_RING_SAMPLES];
static const char *s_end_reason;
static char s_last_reason[24];

static void strlcpy(char *out, const char *value, size_t n) { snprintf(out, n, "%s", value); }
static int64_t esp_timer_get_time(void) { return clock_us; }
static void xSemaphoreTake(int sem, int wait) { (void)sem; (void)wait; }
static void xSemaphoreGive(int sem) { (void)sem; }
static bool session_locked(uint32_t sid) { return sid == 7; }
static bool ptt_owned(int source, uint32_t token) { return source == PTT_OWNER_SDR && owner && owner == token; }
static int ptt_owner(void) { return owner ? PTT_OWNER_SDR : PTT_OWNER_NONE; }
static uint32_t ptt_owner_token(void) { return owner; }
static bool voice_sdr_begin(uint32_t tx) { if (owner) return false; owner = tx; return true; }
static void ptt_set_rf_keying(bool on) { (void)on; }
static bool ptt_source_key(int source, uint32_t tx, bool on) {
    if (!allow_key || !ptt_owned(source, tx)) return false;
    keyed = on;
    return true;
}
static void voice_sdr_end(uint32_t tx) { if (owner == tx) { owner = 0; keyed = false; } }
static void monitor_rx_local_tx(int *monitor, bool tx) { *monitor = tx; }
static int netcom_mode(void) { return mode; }
static bool netcom_rf_tx(void) { return rf_on_network; }
static bool netcom_tx_ready(void) { return true; }
static void netcom_tx_end(void) { ++network_ends; }
static size_t voice_bridge_level(void) { return 0; }
static size_t tx_level_locked(void) { return s_tx_head - s_tx_tail; }
static size_t ring_take(int16_t *ring, size_t capacity, size_t *head, size_t *tail,
                        int16_t *pcm, size_t count) {
    (void)ring; (void)capacity; (void)pcm;
    const size_t available = *head - *tail;
    const size_t n = count < available ? count : available;
    *tail += n;
    return n;
}
static void sdr_route_tx_pcm(int16_t *pcm, size_t n) { (void)pcm; routed += n; }
static bool voice_sdr_tail_configured(void) { return tone_configured; }
static uint32_t voice_sdr_tail_ms(void) { return 5000; }
static bool voice_sdr_tail_start(uint32_t tx) {
    assert(ptt_owned(PTT_OWNER_SDR, tx) && tx_level_locked() == 0);
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
int main(void)
{
    const uint32_t sid = 7;
    assert(!sdr_ptt_request(sid + 1, 1, true));
    assert(sdr_ptt_request(sid, 1, true) && keyed);
    s_tx_head = 200; /* below normal playback prefill */
    assert(sdr_ptt_request(sid, 1, false) && s_releasing);
    sdr_tx_step();
    assert(routed == 200 && tone_starts == 1 && keyed);
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
    assert(s_tail_started && tone_starts == 2 && !network_ends);
    tail_done = 3;
    sdr_tx_step();
    assert(!s_tail_started && !owner && network_ends == 1 && s_rel_tail == 2);
    assert(!sdr_ptt_request(sid, 4, true));

    tone_configured = false;
    assert(sdr_ptt_request(sid, 5, true));
    assert(sdr_ptt_request(sid, 5, false));
    sdr_tx_step();
    assert(!owner && !s_releasing && network_ends == 2 && tone_starts == 2);
    assert(s_rel_notail == 1 && s_rel_released_notail == 1);
    puts("PASS: SDR short/long release, last DAC buffer, network end, duplicate/late OFF and blocked late ON");
    return 0;
}
"""


def main():
    source = (ROOT / "firmware/main/sdr.c").read_text("utf-8")
    names = ("release_locked", "request_release_locked", "finish_release_locked",
             "start_tx_locked", "sdr_ptt_request", "sdr_tx_step")
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
