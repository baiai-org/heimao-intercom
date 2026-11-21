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
#include "audio_i2s.h"
#include "channel_control.h"
#include "netcom.h"
#include "ptt.h"
#include "tx_timeout.h"
#include "sa818_cfg.h"
#include "voice.h"
#include "voice_tail.h"

static const char *TAG = "bridge";

#define NVS_NS   "orl"
#define KEY_EN   "bren"
#define KEY_CHAN "brchan"
#define KEY_DENOISE "rfdenoise1"
#define KEY_DENOISE_CONFIG "auddn_cfg2"
#define KEY_DENOISE_LEVELS "auddn_lvl1"
#define KEY_DENOISE_SELECTION "auddn_sel1"
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
/* Keep the user's selected level even when every path is off. The lower
 * thirty bits retain the effective per-path levels, including old mixed data. */
static atomic_uint s_denoise_config;
static uint32_t s_rx_generation;
static uint8_t s_rx_mode;
static uint8_t s_rx_denoise_mode;
static uint16_t s_rx_room;
static int s_rx_chan;
static bool s_rx_room_changing;
static tx_timeout_window_t s_rx_window;
static bool s_rx_limited;
static int s_rx_tail_id;
static size_t s_rx_tail_pos;
static int64_t s_rx_tail_next_us;

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
static void bridge_rx_end_locked(void)
{
    if (!s_rx.open) {
        return;
    }
    s_rx.end_ms = esp_timer_get_time() / 1000;
    const bool with_tail = s_rx_tail_pos != 0;
    if (netcom_mode() == s_rx_mode && netcom_room() == s_rx_room) {
        netcom_tx_end();
    }
    if (s_rx_denoise_mode) { rf_denoise_invalidate(bridge_denoise_route(s_rx_denoise_mode)); }
    s_rx.open = false;
    s_rx_window = (tx_timeout_window_t){0};
    s_rx_tail_id = TAIL_OFF;
    s_rx_tail_pos = 0;
    s_rx_tail_next_us = 0;
    s_rx_denoise_mode = 0;
    ++s_rx_generation;
    ++s_rx.ends;
    ptt_trace_note(PTT_TRACE_RFNET_OFF, netcom_mode(), s_rx.ends, with_tail ? 1 : 0);
    ESP_LOGI(TAG, "RF->NET PTT end");
}

void bridge_rx_end(void)
{
    rx_tx_lock();
    bridge_rx_end_locked();
    rx_tx_unlock();
}

void bridge_rx_inhibit(void)
{
    rx_tx_lock();
    bridge_rx_end_locked();
    rx_tx_unlock();
}

void bridge_rx_room_change(bool changing)
{
    rx_tx_lock();
    s_rx_room_changing = changing;
    if (changing) { bridge_rx_end_locked(); }
    rx_tx_unlock();
}

uint8_t bridge_denoise_network_setting(uint8_t mode, denoise_network_path_t path)
{
    if (mode != NETCOM_MODE_NETP2P && mode != NETCOM_MODE_RELAY) { return 0; }
    return (mode == NETCOM_MODE_NETP2P ? 5u : 8u) + path;
}

static uint32_t denoise_mode_mask(uint8_t mode)
{
    if (mode == NETCOM_MODE_LOCAL) { return DENOISE_LOCAL; }
    const uint8_t first = bridge_denoise_network_setting(mode, DENOISE_NET_TX);
    return first ? DENOISE_LOCAL | (1u << mode) | (7u << first) : 0;
}

rf_denoise_route_t bridge_denoise_route(uint8_t setting)
{
    static const rf_denoise_route_t routes[] = {
        RF_DENOISE_ROUTE_COUNT, RF_DENOISE_BRIDGE, RF_DENOISE_RELAY_BRIDGE,
        RF_DENOISE_SPEAKER, RF_DENOISE_HANDSET_TX,
        RF_DENOISE_NET_TX, RF_DENOISE_P2P_SPEAKER, RF_DENOISE_P2P_RF,
        RF_DENOISE_RELAY_TX, RF_DENOISE_RELAY_SPEAKER, RF_DENOISE_RELAY_RF,
    };
    return setting < sizeof(routes) / sizeof(*routes) ? routes[setting] : RF_DENOISE_ROUTE_COUNT;
}

static unsigned denoise_level_shift(uint8_t setting)
{
    return (setting - 1u) * DENOISE_LEVEL_BITS;
}

#define DENOISE_LEVEL_FIELD ((1u << DENOISE_LEVEL_BITS) - 1u)

uint32_t bridge_denoise_levels(void)
{
    return atomic_load(&s_denoise_config) & DENOISE_LEVELS_MAX;
}

bridge_denoise_config_t bridge_denoise_config(void)
{
    const uint32_t packed = atomic_load(&s_denoise_config);
    return (bridge_denoise_config_t){
        .levels = packed & DENOISE_LEVELS_MAX,
        .selection_level = (packed >> (DENOISE_LEVEL_BITS * DENOISE_SETTING_COUNT)) + RF_DENOISE_LEVEL_LIGHT,
    };
}

static uint32_t pack_denoise(uint32_t levels, rf_denoise_level_t level)
{
    return levels | ((uint32_t)(level - RF_DENOISE_LEVEL_LIGHT) <<
                     (DENOISE_LEVEL_BITS * DENOISE_SETTING_COUNT));
}

rf_denoise_level_t bridge_denoise_level(uint32_t levels, uint8_t setting)
{
    if (!setting || setting > DENOISE_SETTING_COUNT) { return RF_DENOISE_LEVEL_OFF; }
    return (levels >> denoise_level_shift(setting)) & DENOISE_LEVEL_FIELD;
}

uint32_t bridge_denoise_flags(uint32_t levels)
{
    uint32_t flags = 0;
    for (uint8_t setting = 1; setting <= DENOISE_SETTING_COUNT; ++setting) {
        if (bridge_denoise_level(levels, setting) != RF_DENOISE_LEVEL_OFF) { flags |= 1u << setting; }
    }
    return flags;
}

static uint32_t denoise_levels_from_flags(uint32_t flags, rf_denoise_level_t level)
{
    uint32_t levels = 0;
    for (uint8_t setting = 1; setting <= DENOISE_SETTING_COUNT; ++setting) {
        if (flags & (1u << setting)) {
            levels |= (uint32_t)level << denoise_level_shift(setting);
        }
    }
    return levels;
}

static rf_denoise_level_t denoise_selection_level(uint32_t levels)
{
    rf_denoise_level_t common = RF_DENOISE_LEVEL_OFF;
    for (uint8_t setting = 1; setting <= DENOISE_SETTING_COUNT; ++setting) {
        const rf_denoise_level_t level = bridge_denoise_level(levels, setting);
        if (!level) { continue; }
        if (common && common != level) { return RF_DENOISE_LEVEL_OFF; }
        common = level;
    }
    return common;
}

static bool denoise_levels_valid(uint32_t levels, uint32_t mask)
{
    if (levels > DENOISE_LEVELS_MAX) { return false; }
    for (uint8_t setting = 1; setting <= DENOISE_SETTING_COUNT; ++setting) {
        const rf_denoise_level_t level = bridge_denoise_level(levels, setting);
        if (level > RF_DENOISE_LEVEL_EXTREME || (level && !(mask & (1u << setting)))) {
            return false;
        }
    }
    return true;
}

static void prepare_denoise(uint32_t levels, uint32_t mask)
{
    for (uint8_t setting = 1; setting <= DENOISE_SETTING_COUNT; ++setting) {
        if ((mask & (1u << setting)) && bridge_denoise_level(levels, setting)) {
            rf_denoise_prepare(bridge_denoise_route(setting));
        }
    }
}

static esp_err_t save_denoise_locked(uint32_t next, uint32_t mask)
{
    if (next != atomic_load(&s_denoise_config)) {
        nvs_handle_t h;
        esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
        if (err != ESP_OK) { return err; }
        err = nvs_set_u32(h, KEY_DENOISE_SELECTION, next);
        if (err == ESP_OK) { err = nvs_commit(h); }
        nvs_close(h);
        if (err != ESP_OK) { return err; }
    }
    prepare_denoise(next & DENOISE_LEVELS_MAX, mask);
    atomic_store(&s_denoise_config, next);
    return ESP_OK;
}

static bool valid_selection_level(rf_denoise_level_t level)
{
    return level >= RF_DENOISE_LEVEL_LIGHT && level <= RF_DENOISE_LEVEL_EXTREME;
}

esp_err_t bridge_denoise_save_selection(rf_denoise_level_t level, uint32_t paths,
                                        bridge_denoise_config_t expected)
{
    if (!valid_selection_level(level) || !valid_selection_level(expected.selection_level) ||
        (paths & ~DENOISE_CONFIG_MASK) || !denoise_levels_valid(expected.levels, DENOISE_CONFIG_MASK)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_state_mux || xSemaphoreTake(s_state_mux, pdMS_TO_TICKS(2500)) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t before = atomic_load(&s_denoise_config);
    const uint32_t next = pack_denoise(denoise_levels_from_flags(paths, level), level);
    const esp_err_t err = (before == next || before == pack_denoise(expected.levels, expected.selection_level))
        ? save_denoise_locked(next, paths) : ESP_ERR_INVALID_STATE;
    xSemaphoreGive(s_state_mux);
    return err;
}

static esp_err_t denoise_update(uint8_t mode, uint32_t mask, uint32_t expected,
                                uint32_t values, bool legacy)
{
    if (!mask || (mask & ~denoise_mode_mask(mode))) { return ESP_ERR_INVALID_ARG; }
    const bool valid = legacy ? ((values | expected) & ~mask) == 0 :
        denoise_levels_valid(values, mask) && denoise_levels_valid(expected, mask);
    if (!valid) { return ESP_ERR_INVALID_ARG; }
    if (!s_state_mux || xSemaphoreTake(s_state_mux, pdMS_TO_TICKS(2500)) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    const bridge_denoise_config_t config = bridge_denoise_config();
    const uint32_t before = config.levels;
    uint32_t updated = before;
    esp_err_t err = ESP_OK;
    for (uint8_t setting = 1; setting <= DENOISE_SETTING_COUNT; ++setting) {
        const uint32_t bit = 1u << setting;
        if (!(mask & bit)) { continue; }
        const rf_denoise_level_t current = bridge_denoise_level(before, setting);
        const unsigned observed = legacy ? current != RF_DENOISE_LEVEL_OFF : current;
        const unsigned original = legacy ? (expected & bit) != 0 : bridge_denoise_level(expected, setting);
        const unsigned requested = legacy ? (values & bit) != 0 : bridge_denoise_level(values, setting);
        if (observed == requested) { continue; }
        if (observed != original) {
            err = ESP_ERR_INVALID_STATE;
            break;
        }
        const unsigned shift = denoise_level_shift(setting);
        const uint32_t level = legacy && requested ? RF_DENOISE_LEVEL_STANDARD : requested;
        updated = (updated & ~(DENOISE_LEVEL_FIELD << shift)) | (level << shift);
    }
    if (err == ESP_OK) {
        const rf_denoise_level_t common = denoise_selection_level(updated);
        err = save_denoise_locked(pack_denoise(updated, common ? common : config.selection_level), mask);
    }
    xSemaphoreGive(s_state_mux);
    return err;
}

esp_err_t bridge_denoise_update_levels(uint8_t mode, uint32_t mask, uint32_t expected, uint32_t values)
{
    return denoise_update(mode, mask, expected, values, false);
}

esp_err_t bridge_denoise_update(uint8_t mode, uint32_t mask, uint32_t expected, uint32_t values)
{
    return denoise_update(mode, mask, expected, values, true);
}

esp_err_t bridge_denoise_set(uint8_t setting, bool enabled, bool expected)
{
    if (setting < 1 || setting > DENOISE_SETTING_HANDSET_TX) { return ESP_ERR_INVALID_ARG; }
    const uint32_t bit = 1u << setting;
    const uint8_t mode = setting <= NETCOM_MODE_RELAY ? setting : NETCOM_MODE_LOCAL;
    return bridge_denoise_update(mode, bit, expected ? bit : 0, enabled ? bit : 0);
}

void bridge_denoise_status(uint8_t mode, uint32_t levels, uint8_t *active_mode, rf_denoise_stats_t *stats)
{
    rx_tx_lock();
    *active_mode = s_rx_denoise_mode;
    rf_denoise_stats(mode == NETCOM_MODE_RELAY ? RF_DENOISE_RELAY_BRIDGE : RF_DENOISE_BRIDGE, stats);
    if (*active_mode != mode || mode == NETCOM_MODE_LOCAL) {
        const bool enabled = bridge_denoise_level(levels, mode) != RF_DENOISE_LEVEL_OFF;
        stats->state = enabled ? (stats->workspace_bytes ? RF_DENOISE_ARMED :
                       RF_DENOISE_MEMORY_BYPASS) : RF_DENOISE_OFF;
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

static uint32_t load_denoise_config(nvs_handle_t h)
{
    uint32_t packed;
    const esp_err_t selection = nvs_get_u32(h, KEY_DENOISE_SELECTION, &packed);
    if (selection == ESP_OK && denoise_levels_valid(packed & DENOISE_LEVELS_MAX, DENOISE_CONFIG_MASK)) {
        return packed;
    }
    uint32_t levels = 0, flags = 0;
    const esp_err_t saved_levels = nvs_get_u32(h, KEY_DENOISE_LEVELS, &levels);
    if (saved_levels != ESP_OK || !denoise_levels_valid(levels, DENOISE_CONFIG_MASK)) {
        const esp_err_t saved_flags = nvs_get_u32(h, KEY_DENOISE_CONFIG, &flags);
        if (saved_flags != ESP_OK || (flags & ~DENOISE_CONFIG_MASK)) {
            uint8_t network = 0, local = 0;
            const esp_err_t saved_network = nvs_get_u8(h, KEY_DENOISE, &network);
            const esp_err_t saved_local = nvs_get_u8(h, KEY_LOCAL_DENOISE, &local);
            flags = (network & DENOISE_MODES) | (local & DENOISE_LOCAL);
            if (selection == ESP_ERR_NVS_NOT_FOUND && saved_levels == ESP_ERR_NVS_NOT_FOUND &&
                saved_flags == ESP_ERR_NVS_NOT_FOUND && saved_network == ESP_ERR_NVS_NOT_FOUND &&
                saved_local == ESP_ERR_NVS_NOT_FOUND) {
                flags = DENOISE_CONFIG_MASK;
            }
        }
        levels = denoise_levels_from_flags(flags, RF_DENOISE_LEVEL_STANDARD);
    }
    const rf_denoise_level_t common = denoise_selection_level(levels);
    return pack_denoise(levels, common ? common : RF_DENOISE_LEVEL_STANDARD);
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
    uint32_t config = pack_denoise(0, RF_DENOISE_LEVEL_STANDARD);
    nvs_handle_t h;
    const esp_err_t opened = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (opened == ESP_OK) {
        uint8_t en = 0, chan = 0;
        if (nvs_get_u8(h, KEY_EN, &en) == ESP_OK) {
            s_en = (en != 0);
        }
        if (nvs_get_u8(h, KEY_CHAN, &chan) == ESP_OK) {
            s_chan = (int)chan;
        }
        config = load_denoise_config(h);
        nvs_close(h);
    } else if (opened == ESP_ERR_NVS_NOT_FOUND) {
        config = pack_denoise(denoise_levels_from_flags(DENOISE_CONFIG_MASK, RF_DENOISE_LEVEL_STANDARD),
                              RF_DENOISE_LEVEL_STANDARD);
    }
    prepare_denoise(config & DENOISE_LEVELS_MAX, DENOISE_CONFIG_MASK);
    atomic_store(&s_denoise_config, config);
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
        bridge_rx_end_locked();
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
           sa818_cfg_tx_ready() && ptt_owner() == PTT_OWNER_NONE &&
           !(rx_guard_blocked() & RX_GUARD_OUT_NET);
}

static bool bridge_rx_submit_speech_locked(const int16_t *pcm, size_t n)
{
    if (netcom_tx_sample_until(pcm, n, false, s_rx_window.release_us)) {
        s_rx.submitted_samples += (uint32_t)n;
        return true;
    }
    s_rx.rejected_samples += (uint32_t)n;
    s_rx_limited = true;
    bridge_rx_end_locked();
    ESP_LOGW(TAG, "RF->NET send queue exhausted");
    return false;
}

static bool denoised_emit(const int16_t *pcm, size_t n, void *context)
{
    const uint32_t generation = *(const uint32_t *)context;
    rx_tx_lock();
    const bool same_stream = s_rx.open && s_rx_generation == generation;
    const bool allowed = same_stream && denoised_route_ready();
    bool submitted = false;
    if (allowed) { submitted = bridge_rx_submit_speech_locked(pcm, n); }
    else if (same_stream) { bridge_rx_end_locked(); }
    rx_tx_unlock();
    return submitted;
}

/* One paced packet at a time, after all queued speech. Do not advance the
 * tone cursor until the encoder queue can take the entire next chunk. */
static void bridge_rx_tail_step_locked(int64_t now)
{
    const size_t total = tail_samples(s_rx_tail_id);
    if (now >= s_rx_window.release_us || s_rx_tail_pos >= total) {
        bridge_rx_end_locked();
        return;
    }
    if (now < s_rx_tail_next_us) { return; }
    int16_t tail[AUDIO_PKT_SAMPLES];
    const size_t left = total - s_rx_tail_pos;
    const size_t size = left < AUDIO_PKT_SAMPLES ? left : AUDIO_PKT_SAMPLES;
    if (!netcom_tx_room(size, true, s_rx_window.release_us)) { return; }
    const size_t n = tail_fetch(s_rx_tail_id, s_rx_tail_pos, tail, size);
    if (!n) {
        bridge_rx_end_locked();
        return;
    }
    if (netcom_tx_sample_until(tail, n, true, s_rx_window.release_us)) {
        s_rx_tail_pos += n;
        const int64_t interval = (int64_t)n * 1000000 / AUDIO_SAMPLE_RATE_HZ;
        const int64_t next = s_rx_tail_next_us + interval;
        s_rx_tail_next_us = next > now ? next : now + interval;
    }
}

static void bridge_rx_begin_tail_locked(int64_t now)
{
    s_rx_tail_id = tail_id_for_mode(s_rx_mode);
    s_rx_tail_next_us = now;
    const int64_t release_us = now + (int64_t)TX_TIMEOUT_TAIL_MS * 1000;
    if (s_rx_window.release_us > release_us) { s_rx_window.release_us = release_us; }
    if (s_rx_tail_id == TAIL_OFF) { bridge_rx_end_locked(); }
    else { bridge_rx_tail_step_locked(now); }
}

void bridge_rx_tap(const int16_t *pcm, size_t n, bool squelch_open, bool enabled)
{
    rx_tx_lock();
    const bool inhibited = (rx_guard_blocked() & RX_GUARD_OUT_NET) != 0;
    const bool allowed = s_active && sa818_cfg_tx_ready() && enabled &&
                         ptt_owner() == PTT_OWNER_NONE && !inhibited;
    uint32_t generation = s_rx_generation;
    const int64_t now = esp_timer_get_time();
    if (s_rx_tail_id != TAIL_OFF) {
        if (!allowed || s_rx_mode != netcom_mode() || s_rx_room != netcom_room() ||
            (squelch_open && !s_rx_limited)) {
            bridge_rx_end_locked();
        } else {
            bridge_rx_tail_step_locked(now);
            rx_tx_unlock();
            return;
        }
    }
    if (s_rx_limited) {
        if (enabled && !squelch_open && !s_rx.open) { s_rx_limited = false; }
        rx_tx_unlock();
        return;
    }
    if (s_rx_denoise_mode && (!allowed || !denoised_route_ready())) {
        bridge_rx_end_locked();
        rx_tx_unlock();
        return;
    }
    const bool limit = s_rx.open && tx_timeout_due(s_rx_window, now);
    if (limit) {
        s_rx_limited = true;
        ESP_LOGW(TAG, "RF->NET speech limit");
    }
    if (!allowed || !squelch_open || limit) {
        const bool drain = allowed && s_rx_denoise_mode;
        if (!s_rx.open || !drain) {
            if (s_rx.open && allowed) { bridge_rx_begin_tail_locked(now); }
            else { bridge_rx_end_locked(); }
            rx_tx_unlock();
            return;
        }
        const rf_denoise_route_t route = bridge_denoise_route(s_rx_denoise_mode);
        rx_tx_unlock();
        const bool drained = rf_denoise_finish(route, denoised_emit, &generation);
        rf_denoise_stop(route);
        rx_tx_lock();
        if (s_rx.open && s_rx_generation == generation) {
            const bool with_tail = drained && denoised_route_ready();
            if (with_tail) { bridge_rx_begin_tail_locked(esp_timer_get_time()); }
            else { bridge_rx_end_locked(); }
        }
        rx_tx_unlock();
        return;
    }
    const uint8_t mode = netcom_mode();
    const rf_denoise_level_t level = bridge_denoise_level(bridge_denoise_levels(), mode);
    if (!pcm || !n || (level && s_rx_room_changing)) {
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
        s_rx_mode = mode;
        s_rx_room = netcom_room();
        if (level) {
            s_rx_denoise_mode = mode;
            s_rx_chan = s_chan;
        }
        s_rx.open = true;
        s_rx.captured_samples = s_rx.submitted_samples = s_rx.rejected_samples = 0;
        s_rx_window = tx_timeout_window(now, tx_timeout_seconds());
        s_rx.start_ms = now / 1000;
        ++s_rx.starts;
        ptt_trace_note(PTT_TRACE_RFNET_ON, netcom_mode(), s_rx.starts, 0);
        ESP_LOGI(TAG, "RF->NET PTT start");
    }
    s_rx.captured_samples += (uint32_t)n;
    if (!s_rx_denoise_mode) {
        (void)bridge_rx_submit_speech_locked(pcm, n);
        rx_tx_unlock();
        return;
    }
    const rf_denoise_route_t route = bridge_denoise_route(s_rx_denoise_mode);
    if (starting) { (void)rf_denoise_start(route, level); }
    rx_tx_unlock();
    if (!rf_denoise_push(route, pcm, n, denoised_emit, &generation)) { rf_denoise_stop(route); }
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
