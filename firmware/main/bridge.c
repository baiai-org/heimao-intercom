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
#define KEY_DENOISE "rfdenoise1"
#define KEY_DENOISE_CONFIG "auddn_cfg2"
#define DENOISE_MODES ((1u << NETCOM_MODE_NETP2P) | (1u << NETCOM_MODE_RELAY))
#define KEY_LOCAL_DENOISE "auddn_local1"
#define DENOISE_LOCAL ((1u << DENOISE_SETTING_SPEAKER) | (1u << DENOISE_SETTING_HANDSET_TX))

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
static atomic_uint s_denoise_modes;
static uint32_t s_rx_generation;
static uint8_t s_rx_denoise_mode;
static uint16_t s_rx_room;
static int s_rx_chan;
static bool s_rx_room_changing;

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
    if (!s_rx_denoise_mode ||
        (netcom_mode() == s_rx_denoise_mode && netcom_room() == s_rx_room)) {
        netcom_tx_end();
    }
    s_rx.open = false;
    s_rx_denoise_mode = 0;
    ++s_rx_generation;
    ++s_rx.ends;
    ptt_trace_note(PTT_TRACE_RFNET_OFF, netcom_mode(), s_rx.ends, with_tail ? 1 : 0);
    ESP_LOGI(TAG, "RF->NET PTT end");
}

void bridge_rx_end(void)
{
    rx_tx_lock();
    bridge_rx_end_locked(!s_rx_denoise_mode);
    rx_tx_unlock();
}

void bridge_rx_inhibit(void)
{
    rx_tx_lock();
    bridge_rx_end_locked(false);
    rx_tx_unlock();
}

void bridge_rx_room_change(bool changing)
{
    rx_tx_lock();
    s_rx_room_changing = changing;
    if (changing && s_rx_denoise_mode) { bridge_rx_end_locked(false); }
    rx_tx_unlock();
}

uint8_t bridge_denoise_network_setting(uint8_t mode, denoise_network_path_t path)
{
    if (mode != NETCOM_MODE_NETP2P && mode != NETCOM_MODE_RELAY) { return 0; }
    return (mode == NETCOM_MODE_NETP2P ? 5u : 8u) + path;
}

uint32_t bridge_denoise_mode_mask(uint8_t mode)
{
    if (mode == NETCOM_MODE_LOCAL) { return DENOISE_LOCAL; }
    const uint8_t first = bridge_denoise_network_setting(mode, DENOISE_NET_TX);
    return first ? DENOISE_LOCAL | (1u << mode) | (7u << first) : 0;
}

rf_denoise_route_t bridge_denoise_route(uint8_t setting)
{
    static const rf_denoise_route_t routes[] = {
        RF_DENOISE_ROUTE_COUNT, RF_DENOISE_BRIDGE, RF_DENOISE_BRIDGE,
        RF_DENOISE_SPEAKER, RF_DENOISE_HANDSET_TX,
        RF_DENOISE_NET_TX, RF_DENOISE_P2P_SPEAKER, RF_DENOISE_P2P_RF,
        RF_DENOISE_NET_TX, RF_DENOISE_RELAY_SPEAKER, RF_DENOISE_RELAY_RF,
    };
    return setting < sizeof(routes) / sizeof(*routes) ? routes[setting] : RF_DENOISE_ROUTE_COUNT;
}

static void prepare_denoise(uint32_t flags)
{
    for (uint8_t setting = 1; setting <= 10; ++setting) {
        if (flags & (1u << setting)) { rf_denoise_prepare(bridge_denoise_route(setting)); }
    }
}

esp_err_t bridge_denoise_update(uint8_t mode, uint32_t mask, uint32_t expected, uint32_t values)
{
    if (!mask || (mask & ~bridge_denoise_mode_mask(mode)) || ((values | expected) & ~mask)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_state_mux || xSemaphoreTake(s_state_mux, pdMS_TO_TICKS(2500)) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t before = atomic_load(&s_denoise_modes);
    const uint32_t updated = (before & ~mask) | values;
    esp_err_t err = ESP_OK;
    if (before != updated) {
        const uint32_t conflicts = mask & (before ^ expected);
        nvs_handle_t h;
        err = conflicts ? ESP_ERR_INVALID_STATE : nvs_open(NVS_NS, NVS_READWRITE, &h);
        if (err == ESP_OK) {
            err = nvs_set_u32(h, KEY_DENOISE_CONFIG, updated);
            if (err == ESP_OK) { err = nvs_commit(h); }
            nvs_close(h);
        }
    }
    if (err == ESP_OK) {
        prepare_denoise(values);
        atomic_store(&s_denoise_modes, updated);
    }
    xSemaphoreGive(s_state_mux);
    return err;
}

esp_err_t bridge_denoise_set(uint8_t setting, bool enabled, bool expected)
{
    if (setting < 1 || setting > DENOISE_SETTING_HANDSET_TX) { return ESP_ERR_INVALID_ARG; }
    const uint32_t bit = 1u << setting;
    const uint8_t mode = setting <= NETCOM_MODE_RELAY ? setting : NETCOM_MODE_LOCAL;
    return bridge_denoise_update(mode, bit, expected ? bit : 0, enabled ? bit : 0);
}

uint32_t bridge_denoise_flags(void)
{
    return atomic_load(&s_denoise_modes);
}

bool bridge_denoise_enabled(uint8_t setting)
{
    return setting && setting <= 10 && (bridge_denoise_flags() & (1u << setting)) != 0;
}

void bridge_denoise_status(uint32_t *flags, uint8_t *active_mode, rf_denoise_stats_t *stats)
{
    rx_tx_lock();
    *flags = atomic_load(&s_denoise_modes);
    *active_mode = s_rx_denoise_mode;
    rf_denoise_stats(RF_DENOISE_BRIDGE, stats);
    if (!*active_mode) {
        stats->state = !(*flags & DENOISE_MODES) ? RF_DENOISE_OFF : stats->workspace_bytes ?
            RF_DENOISE_ARMED : RF_DENOISE_MEMORY_BYPASS;
    } else if (stats->state == RF_DENOISE_OFF) {
        stats->state = RF_DENOISE_ARMED;
    }
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
    uint32_t flags = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t en = 0, chan = 0;
        if (nvs_get_u8(h, KEY_EN, &en) == ESP_OK) {
            s_en = (en != 0);
        }
        if (nvs_get_u8(h, KEY_CHAN, &chan) == ESP_OK) {
            s_chan = (int)chan;
        }
        if (nvs_get_u32(h, KEY_DENOISE_CONFIG, &flags) != ESP_OK || (flags & ~DENOISE_CONFIG_MASK)) {
            uint8_t denoise = 0, local = 0;
            (void)nvs_get_u8(h, KEY_DENOISE, &denoise);
            (void)nvs_get_u8(h, KEY_LOCAL_DENOISE, &local);
            flags = (denoise & DENOISE_MODES) | (local & DENOISE_LOCAL);
        }
        nvs_close(h);
    }
    prepare_denoise(flags);
    atomic_store(&s_denoise_modes, flags);
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
        bridge_rx_end_locked(!s_rx_denoise_mode);
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

static bool denoised_route_ready(void)
{
    return s_active && !s_rx_room_changing && s_rx_denoise_mode == netcom_mode() &&
           s_rx_room == netcom_room() && s_rx_chan == s_chan && netcom_engine_running() &&
           sa818_cfg_tx_ready() && !ptt_bridge_keyed() &&
           !(rx_guard_blocked() & RX_GUARD_OUT_NET);
}

static bool denoised_emit(const int16_t *pcm, size_t n, void *context)
{
    const uint32_t generation = *(const uint32_t *)context;
    rx_tx_lock();
    const bool same_stream = s_rx.open && s_rx_generation == generation;
    const bool allowed = same_stream && denoised_route_ready();
    if (allowed) { netcom_tx_sample(pcm, n, true); }
    else if (same_stream) { bridge_rx_end_locked(false); }
    rx_tx_unlock();
    return allowed;
}

void bridge_rx_tap(const int16_t *pcm, size_t n, bool squelch_open, bool enabled)
{
    rx_tx_lock();
    const bool inhibited = (rx_guard_blocked() & RX_GUARD_OUT_NET) != 0;
    const bool allowed = s_active && sa818_cfg_tx_ready() && enabled &&
                         !ptt_bridge_keyed() && !inhibited;
    uint32_t generation = s_rx_generation;
    if (s_rx_denoise_mode && (!allowed || !denoised_route_ready())) {
        bridge_rx_end_locked(false);
        rx_tx_unlock();
        return;
    }
    if (!allowed || !squelch_open) {
        const bool drain = allowed && s_rx_denoise_mode;
        if (!drain) {
            bridge_rx_end_locked(!inhibited);
            rx_tx_unlock();
            return;
        }
        rx_tx_unlock();
        const bool drained = rf_denoise_finish(RF_DENOISE_BRIDGE, denoised_emit, &generation);
        rx_tx_lock();
        if (s_rx.open && s_rx_generation == generation) {
            bridge_rx_end_locked(drained && denoised_route_ready());
        }
        rx_tx_unlock();
        return;
    }
    const uint8_t mode = netcom_mode();
    const bool configured = mode != NETCOM_MODE_LOCAL && bridge_denoise_enabled(mode);
    if (!pcm || !n || (configured && s_rx_room_changing)) {
        rx_tx_unlock();
        return;
    }
    /*
     * Half-duplex mute: while the bridge keys the radio for the room audio,
     * the RF side is transmitting, and whatever squelch state the module
     * reports (it can open on the board's own transmission) must not be
     * forwarded back into the room -- that is a feedback loop.
     */
    const bool starting = !s_rx.open;
    if (starting) {
        if (configured) {
            s_rx_denoise_mode = mode;
            s_rx_room = netcom_room();
            s_rx_chan = s_chan;
        }
        s_rx.open = true;
        s_rx.start_ms = esp_timer_get_time() / 1000;
        ++s_rx.starts;
        ptt_trace_note(PTT_TRACE_RFNET_ON, netcom_mode(), s_rx.starts, 0);
        ESP_LOGI(TAG, "RF->NET PTT start");
    }
    const bool denoise = s_rx_denoise_mode != 0;
    if (!denoise) {
        netcom_tx_sample(pcm, n, true);
        rx_tx_unlock();
        return;
    }
    rx_tx_unlock();
    /* The monitor owns the DSP state. Commit revalidates the generation,
     * active route and receive guard after the expensive computation. */
    if (starting) {
        (void)rf_denoise_start(RF_DENOISE_BRIDGE, bridge_denoise_enabled(DENOISE_SETTING_SPEAKER));
    }
    (void)rf_denoise_push(RF_DENOISE_BRIDGE, pcm, n, denoised_emit, &generation);
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
