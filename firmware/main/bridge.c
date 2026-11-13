/*
 * bridge -- analog <-> network bridge, implementation.  See bridge.h.
 *
 * State machine: the bridge becomes "active" when all of these hold --
 *
 *   - the operator enabled it (persisted),
 *   - a preset channel is selected (and still exists),
 *   - the intercom mode is P2P or relay,
 *   - the engine for that mode is up (can carry the RF -> net audio).
 *
 * Becoming active keeps the operator's radio profile and borrows the monitor
 * task, which is the source
 * of the squelch-gated RF audio.  The net -> RF keying lives in the engines'
 * playback tasks (they own the room audio and the same amp hysteresis); they
 * call ptt_bridge_key() and voice_bridge_inject() directly.
 */
#include "bridge.h"
#include "rx_guard.h"

#include <stdio.h>
#include <string.h>
#include <stdatomic.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "channels.h"
#include "channel_control.h"
#include "netcom.h"
#include "ptt.h"
#include "sa818_cfg.h"
#include "voice.h"
#include "voice_tail.h"

static const char *TAG = "bridge";

#define NVS_NS   "orl"
#define KEY_EN   "bren"
#define KEY_CHAN "brchan"

static atomic_bool s_en;
static atomic_int s_chan;      /* 1-based preset index; 0 = none */
static atomic_bool s_active;
static SemaphoreHandle_t s_state_mux;

/*
 * RF -> network has a stream-level PTT edge in addition to the PCM blocks.
 * Keep that edge in the bridge, because the monitor can stop producing blocks
 * when the bridge is disabled or borrowed by another consumer.  A mutex also
 * serializes the monitor task with panel/mode changes: an end marker must not
 * overtake the last sample block.
 */
static SemaphoreHandle_t s_rx_tx_mux;
static bridge_rx_stats_t s_rx;

static void rx_tx_lock(void)
{
    if (s_rx_tx_mux) {
        (void)xSemaphoreTake(s_rx_tx_mux, portMAX_DELAY);
    }
}

static void rx_tx_unlock(void)
{
    if (s_rx_tx_mux) {
        (void)xSemaphoreGive(s_rx_tx_mux);
    }
}

/* Send exactly one PTT=0 frame for the current RF -> network stream. */
static void bridge_rx_end_locked(bool with_tail)
{
    if (!s_rx.open) {
        return;
    }
    s_rx.end_ms = esp_timer_get_time() / 1000;
    /* Courtesy tone for the RF -> net over: the relay that received it writes
     * the tone into the stream, so the far end just forwards it. */
    if (with_tail && tail_should_play(netcom_mode())) {
        const int id = tail_get();
        if (id > TAIL_OFF && id < TAIL_COUNT) {
            int16_t buf[240];
            size_t pos = 0;
            const size_t total = tail_samples(id);
            while (pos < total) {
                const size_t n = tail_fetch(id, pos, buf, 240);
                if (!n) { break; }
                pos += n;
                netcom_tx_sample(buf, n, true);
            }
        }
    }
        netcom_tx_end();
    s_rx.open = false;
    ++s_rx.ends;
    ptt_trace_note(PTT_TRACE_RFNET_OFF, netcom_mode(), s_rx.ends, with_tail ? 1 : 0);
    ESP_LOGI(TAG, "RF->NET PTT end");
}

void bridge_rx_end(void)
{
    rx_tx_lock();
    bridge_rx_end_locked(true);
    rx_tx_unlock();
}

void bridge_rx_inhibit(void)
{
    rx_tx_lock();
    bridge_rx_end_locked(false);
    rx_tx_unlock();
}

void bridge_rx_stats(bridge_rx_stats_t *out)
{
    rx_tx_lock();
    *out = s_rx;
    rx_tx_unlock();
}

static bool want_active(void)
{
    sa818_cfg_t bound;
    return s_en
        && s_rx_tx_mux != NULL
        && netcom_mode() != NETCOM_MODE_LOCAL
        && s_chan >= 1 && s_chan <= channels_count()
        && sa818_cfg_tx_ready()
        && channels_get(s_chan - 1, NULL, 0, &bound)
        && sa818_cfg_compatible(&bound)
        && netcom_engine_running();
}

void bridge_init(void)
{
    channel_control_init();
    rx_guard_init();
    s_en = false;
    s_chan = 0;
    s_active = false;
    memset(&s_rx, 0, sizeof(s_rx));
    s_state_mux = xSemaphoreCreateMutex();
    s_rx_tx_mux = xSemaphoreCreateMutex();
    if (!s_rx_tx_mux) {
        ESP_LOGE(TAG, "RF->NET edge mutex allocation failed");
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t en = 0, chan = 0;
        if (nvs_get_u8(h, KEY_EN, &en) == ESP_OK) {
            s_en = (en != 0);
        }
        if (nvs_get_u8(h, KEY_CHAN, &chan) == ESP_OK) {
            s_chan = (int)chan;
        }
        nvs_close(h);
    }
    ESP_LOGI(TAG, "init: en=%d chan=%d", s_en ? 1 : 0, s_chan);
}

static esp_err_t persist_values(bool en, int chan)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) { return err; }
    err = nvs_set_u8(h, KEY_EN, en ? 1 : 0);
    if (err == ESP_OK) { err = nvs_set_u8(h, KEY_CHAN, (uint8_t)chan); }
    if (err == ESP_OK) { err = nvs_commit(h); }
    nvs_close(h);
    return err;
}

void bridge_apply(void)
{
    const bool want = want_active();
    rx_guard_context(netcom_mode(), want);
    if (want && !s_active) {
        /* Keep the operator's current radio profile. */
        /* Start unkeyed; the playback task keys up with the room audio. */
        if (ptt_bridge_keyed()) {
            ptt_bridge_key(false);
        }
        voice_monitor_borrow("bridge");
        rx_tx_lock();
        s_active = true;
        rx_tx_unlock();
        ESP_LOGW(TAG, "BRIDGE ON: RF <-> network on channel %d", s_chan);
    } else if (!want) {
        rx_tx_lock();
        const bool was_active = s_active;
        /* Block new samples before sending the end marker or stopping capture. */
        s_active = false;
        bridge_rx_end_locked(true);
        rx_tx_unlock();
        if (was_active) {
            if (ptt_bridge_keyed()) {
                ptt_bridge_key(false);
            }
            /* Never wait for the monitor while holding its tap mutex. */
            voice_monitor_release("bridge");
            ESP_LOGW(TAG, "BRIDGE OFF");
        }
    }
}

esp_err_t bridge_set_en(bool en)
{
    if (!s_state_mux || xSemaphoreTake(s_state_mux, pdMS_TO_TICKS(2500)) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t err = persist_values(en, s_chan);
    if (err == ESP_OK) { s_en = en; }
    xSemaphoreGive(s_state_mux);
    if (err != ESP_OK) { return err; }
    bridge_apply();
    ESP_LOGI(TAG, "enabled -> %d", s_en ? 1 : 0);
    return ESP_OK;
}

esp_err_t bridge_set_chan(int index)
{
    char log[192];
    const esp_err_t err = channel_select(index, true, log, sizeof(log));
    if (err != ESP_OK) { ESP_LOGW(TAG, "channel selection failed: %s", log); }
    return err;
}

esp_err_t bridge_channel_selected(int index, bool bind_bridge)
{
    if (!s_state_mux || xSemaphoreTake(s_state_mux, pdMS_TO_TICKS(2500)) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!bind_bridge && !s_en) { xSemaphoreGive(s_state_mux); return ESP_OK; }
    const esp_err_t err = persist_values(s_en, index);
    if (err == ESP_OK) { s_chan = index; }
    xSemaphoreGive(s_state_mux);
    if (err != ESP_OK) { return err; }
    bridge_apply();
    ESP_LOGI(TAG, "channel -> %d", s_chan);
    return ESP_OK;
}

void bridge_rx_tap(const int16_t *pcm, size_t n, bool squelch_open, bool enabled)
{
    rx_tx_lock();
    const bool inhibited = (rx_guard_blocked() & RX_GUARD_OUT_NET) != 0;
    if (!s_active || !sa818_cfg_tx_ready() || !enabled || !squelch_open || ptt_bridge_keyed() ||
        inhibited) {
        bridge_rx_end_locked(!inhibited);
        rx_tx_unlock();
        return;
    }
    if (!pcm || n == 0) {
        rx_tx_unlock();
        return;
    }
    /*
     * Half-duplex mute: while the bridge keys the radio for the room audio,
     * the RF side is transmitting, and whatever squelch state the module
     * reports (it can open on the board's own transmission) must not be
     * forwarded back into the room -- that is a feedback loop.
     */
    if (!s_rx.open) {
        s_rx.open = true;
        s_rx.start_ms = esp_timer_get_time() / 1000;
        ++s_rx.starts;
        ptt_trace_note(PTT_TRACE_RFNET_ON, netcom_mode(), s_rx.starts, 0);
        ESP_LOGI(TAG, "RF->NET PTT start");
    }
    netcom_tx_sample(pcm, n, squelch_open);
    rx_tx_unlock();
}

void bridge_channel_deleted(int index)
{
    if (!s_state_mux || xSemaphoreTake(s_state_mux, pdMS_TO_TICKS(2500)) != pdTRUE) { return; }
    if (s_chan == index + 1) { s_chan = 0; }
    else if (s_chan > index + 1) { --s_chan; }
    (void)persist_values(s_en, s_chan);
    xSemaphoreGive(s_state_mux);
    bridge_apply();
}

void bridge_mode_changed(void)
{
    bridge_apply();
}

bool bridge_en(void)      { return s_en; }
int  bridge_chan(void)    { return s_chan; }
bool bridge_active(void)  { return s_active && sa818_cfg_tx_ready(); }
bool bridge_keyed(void)   { return ptt_bridge_keyed(); }
