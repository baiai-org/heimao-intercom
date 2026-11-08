/*
 * Radio operating profile -- implementation.
 */
#include "sa818_cfg.h"

#include "board_hw.h"
#include "ctcss.h"

#include <ctype.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "sa818.h"
#include "bridge.h"
#include "ptt.h"
#include "voice.h"
#include "esp_timer.h"

static const char *TAG = "sa818cfg";

/* Set by the idle refresh path: it must leave no trace in the log. */
static bool s_quiet_apply;
_Static_assert(sizeof(sa818_cfg_t) == 56, "preserve legacy NVS/channel profile layout");

#define NVS_NS    "orl"
#define NVS_KEY   "sa818"

/* Zero frequencies mean not configured; never invent an operating channel. */
#define DEFAULT_TX_MHZ   0.0f
#define DEFAULT_RX_MHZ   0.0f
#define DEFAULT_SQUELCH  3
#define DEFAULT_VOLUME   8

void sa818_cfg_defaults(sa818_cfg_t *cfg)
{
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->bandwidth          = 1;             /* 25 kHz, as requested */
    cfg->high_power         = 0;             /* H/L grounded: low power */
    cfg->tx_mhz             = DEFAULT_TX_MHZ;
    cfg->rx_mhz             = DEFAULT_RX_MHZ;
    strncpy(cfg->tx_ctcss, "0000", sizeof(cfg->tx_ctcss) - 1);
    strncpy(cfg->rx_ctcss, "0000", sizeof(cfg->rx_ctcss) - 1);
    cfg->squelch            = DEFAULT_SQUELCH;
    cfg->volume             = DEFAULT_VOLUME;
    /*
     * The module's OWN end-of-transmission tail tone is OFF by default.
     *
     * The firmware generates the courtesy signal itself (voice_tail: K-CHOO,
     * MDC-1200, ROGER, Icom ...), timed and levelled with the voice path, and
     * it releases PTT the moment that tone's last sample is out.  With
     * AT+SETTAIL=1 the SA818 adds a tail of its own AFTER PTT goes high -- a
     * length the firmware neither controls nor waits for -- which is heard as
     * "the carrier stays up about a second after the tail finished".
     */
    cfg->tail               = false;
    /*
     * All three module filters BYPASSED (1 = bypass, per the SA818 programming
     * manual, AT+SETFILTER: "1: voice_lowpass_filter_bypass / 0: normal").
     *
     * The operator's complaint was that the audio is still muffled even with the
     * firmware anti-alias filter out of the way, and the module's own voice
     * low-pass is applied TWICE in a bridge -- once in the receiving module and
     * once in the transmitting one.  Bypassing pre/de-emphasis on both ends is
     * safe: the source handset's pre-emphasis is cancelled by the destination
     * handset's de-emphasis, so the end-to-end response stays flat.  The only
     * path that changes character is browser listening, which has no handset
     * de-emphasis behind it and will sound bright.
     */
    cfg->filter_pre_deemph  = 1;
    cfg->filter_highpass    = 1;
    cfg->filter_lowpass     = 1;
}


/* Band metadata is separate from the historical raw configuration blob. */
#define NVS_BAND_KEY "rfband"
static atomic_int s_band = SA818_BAND_UNSET;
static atomic_bool s_ready;
static atomic_bool s_configuring;
static SemaphoreHandle_t s_apply_mutex;

/* Keep the legacy profile blob layout while allowing one-time migrations to
 * rewrite only the filter flags. */
static esp_err_t store_profile(const sa818_cfg_t *cfg);

sa818_band_t sa818_cfg_band(void) { return (sa818_band_t)atomic_load(&s_band); }
bool sa818_cfg_tx_ready(void)
{
    return atomic_load(&s_ready) && !atomic_load(&s_configuring);
}

static esp_err_t store_band(sa818_band_t band)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) { return e; }
    e = nvs_set_u8(h, NVS_BAND_KEY, (uint8_t)band);
    if (e == ESP_OK) { e = nvs_commit(h); }
    nvs_close(h);
    return e;
}

esp_err_t sa818_cfg_init(void)
{
    if (s_apply_mutex) { return ESP_OK; }
    s_apply_mutex = xSemaphoreCreateMutex();
    if (!s_apply_mutex) { return ESP_ERR_NO_MEM; }
    nvs_handle_t h;
    uint8_t band = 0;
    esp_err_t e = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (e == ESP_OK) {
        e = nvs_get_u8(h, NVS_BAND_KEY, &band);
        nvs_close(h);
    }
    if (e == ESP_ERR_NVS_NOT_FOUND) {
        sa818_cfg_t legacy;
        if (sa818_cfg_load(&legacy) == ESP_OK &&
            sa818_freq_pair_valid(SA818_BAND_UHF, legacy.tx_mhz, legacy.rx_mhz)) {
            /* Previous firmware only supported UHF. No RF field is rewritten. */
            if (store_band(SA818_BAND_UHF) == ESP_OK) {
                band = SA818_BAND_UHF;
                ESP_LOGI(TAG, "legacy SA818S-U profile retained; band metadata added");
            }
        }
    }
    if (band != SA818_BAND_UHF && band != SA818_BAND_VHF) { band = 0; }
    atomic_store(&s_band, band);

    /* The accepted receive path bypasses every SA818 module filter and uses
     * the fixed ESP32 270 Hz / 3.7 kHz receive band instead.  Older NVS blobs
     * may still contain the former module-high-pass trial values; normalize
     * those flags once so all boards running this firmware stay identical. */
    sa818_cfg_t filter_cfg;
    if (sa818_cfg_load(&filter_cfg) == ESP_OK &&
        (filter_cfg.filter_pre_deemph != 1 ||
         filter_cfg.filter_highpass != 1 ||
         filter_cfg.filter_lowpass != 1)) {
        filter_cfg.filter_pre_deemph = 1;
        filter_cfg.filter_highpass = 1;
        filter_cfg.filter_lowpass = 1;
        if (store_profile(&filter_cfg) == ESP_OK) {
            ESP_LOGI(TAG, "migrated stored module filters to bypass: pre/de=1 highpass=1 lowpass=1");
        } else {
            ESP_LOGW(TAG, "could not persist module-filter bypass migration");
        }
    }
    ESP_LOGI(TAG, "module band %s; RF waits for valid profile ACKs",
             sa818_band_name(sa818_cfg_band()));
    return ESP_OK;
}

esp_err_t sa818_cfg_set_band(sa818_band_t band)
{
    if (band != SA818_BAND_UHF && band != SA818_BAND_VHF) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_apply_mutex || xSemaphoreTake(s_apply_mutex, pdMS_TO_TICKS(2500)) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    atomic_store(&s_configuring, true);
    esp_err_t e = ESP_ERR_INVALID_STATE;
    if (!board_ptt_is_asserted()) {
        e = store_band(band);
        if (e == ESP_OK && band != sa818_cfg_band()) {
            atomic_store(&s_ready, false);
            atomic_store(&s_band, band);
            ESP_LOGI(TAG, "module band -> %s; waiting for panel profile", sa818_band_name(band));
        }
    }
    atomic_store(&s_configuring, false);
    xSemaphoreGive(s_apply_mutex);
    return e;
}

esp_err_t sa818_cfg_validate(const sa818_cfg_t *cfg, char *log, size_t log_len)
{
    const char *why = NULL;
    const sa818_band_t band = sa818_cfg_band();
    if (!cfg) { why = "missing radio profile"; }
    else if (band == SA818_BAND_UNSET) { why = "select installed module band in panel first"; }
    else if (!sa818_freq_pair_valid(band, cfg->tx_mhz, cfg->rx_mhz)) {
        if (log && log_len) {
            snprintf(log, log_len, "%s TX/RX must be %.4f-%.4f MHz on 12.5k grid",
                     sa818_band_name(band), sa818_band_min(band), sa818_band_max(band));
        }
        return ESP_ERR_INVALID_ARG;
    } else if (cfg->bandwidth < 0 || cfg->bandwidth > 1 ||
               cfg->high_power < 0 || cfg->high_power > 1 ||
               cfg->squelch < 0 || cfg->squelch > 8 || cfg->volume < 1 || cfg->volume > 8 ||
               cfg->filter_pre_deemph < 0 || cfg->filter_pre_deemph > 1 ||
               cfg->filter_highpass < 0 || cfg->filter_highpass > 1 ||
               cfg->filter_lowpass < 0 || cfg->filter_lowpass > 1) {
        why = "radio parameter out of range";
    } else {
        char norm[8];
        if (!memchr(cfg->tx_ctcss, 0, sizeof(cfg->tx_ctcss)) ||
            !memchr(cfg->rx_ctcss, 0, sizeof(cfg->rx_ctcss)) ||
            !ctcss_normalise(cfg->tx_ctcss, norm) || strcmp(norm, cfg->tx_ctcss) ||
            !ctcss_normalise(cfg->rx_ctcss, norm) || strcmp(norm, cfg->rx_ctcss)) {
            why = "invalid stored tone code";
        }
    }
    if (log && log_len) { snprintf(log, log_len, "%s", why ? why : "valid"); }
    return why ? ESP_ERR_INVALID_ARG : ESP_OK;
}

bool sa818_cfg_compatible(const sa818_cfg_t *cfg)
{
    return sa818_cfg_validate(cfg, NULL, 0) == ESP_OK;
}

esp_err_t sa818_cfg_load(sa818_cfg_t *cfg)
{
    if (!cfg) { return ESP_ERR_INVALID_ARG; }
    sa818_cfg_defaults(cfg);
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (e != ESP_OK) { return e; }
    size_t len = sizeof(*cfg);
    e = nvs_get_blob(h, NVS_KEY, cfg, &len);
    nvs_close(h);
    if (e != ESP_OK || len != sizeof(*cfg)) {
        sa818_cfg_defaults(cfg);
        return e == ESP_OK ? ESP_ERR_INVALID_SIZE : e;
    }
    /* Bound strings for status formatting even if storage was corrupted. */
    cfg->tx_ctcss[sizeof(cfg->tx_ctcss) - 1] = 0;
    cfg->rx_ctcss[sizeof(cfg->rx_ctcss) - 1] = 0;
    /* RX low-cut trial uses ESP32 filtering only, including profiles saved
     * during the module-high-pass trial. Do not rewrite the stored blob. */
    if (cfg->filter_highpass == 0) { cfg->filter_highpass = 1; }
    return ESP_OK;
}

static esp_err_t store_profile(const sa818_cfg_t *cfg)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) { return e; }
    e = nvs_set_blob(h, NVS_KEY, cfg, sizeof(*cfg));
    if (e == ESP_OK) { e = nvs_commit(h); }
    nvs_close(h);
    return e;
}

static bool ack_ok(const char *reply, const char *tag)
{
    if (!reply || strncmp(reply, tag, strlen(tag))) { return false; }
    const char *suffix = reply + strlen(tag);
    return !strcmp(suffix, ":0");
}

static esp_err_t apply_profile(const sa818_cfg_t *cfg, bool persist, char *log, size_t log_len)
{
    esp_err_t e = sa818_cfg_validate(cfg, log, log_len);
    if (e != ESP_OK) { return e; }
    /* Saved channels must use the same module-high-pass bypass policy. */
    sa818_cfg_t effective = *cfg;
    effective.filter_highpass = 1;
    cfg = &effective;
    if (!s_apply_mutex || xSemaphoreTake(s_apply_mutex, pdMS_TO_TICKS(2500)) != pdTRUE) {
        if (log && log_len) { snprintf(log, log_len, "radio configuration busy"); }
        return ESP_ERR_INVALID_STATE;
    }
    e = sa818_cfg_validate(cfg, log, log_len); /* band may have changed while waiting */
    if (e != ESP_OK) { xSemaphoreGive(s_apply_mutex); return e; }
    atomic_store(&s_configuring, true);
    if (board_ptt_is_asserted()) {
        if (log && log_len) { snprintf(log, log_len, "release PTT before tuning"); }
        atomic_store(&s_configuring, false);
        xSemaphoreGive(s_apply_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    atomic_store(&s_ready, false);
    char reply[96];
    sa818_profile_t p = {
        .bandwidth = cfg->bandwidth, .tx_mhz = cfg->tx_mhz,
        .rx_mhz = cfg->rx_mhz, .squelch = cfg->squelch,
    };
    /* Validation above guarantees termination inside these fixed-size fields. */
    memcpy(p.tx_ctcss, cfg->tx_ctcss, sizeof(p.tx_ctcss));
    memcpy(p.rx_ctcss, cfg->rx_ctcss, sizeof(p.rx_ctcss));
    const char *failed = NULL;
    if (sa818_set_group(&p, reply, sizeof(reply)) != ESP_OK ||
        !ack_ok(reply, "+DMOSETGROUP")) { failed = "DMOSETGROUP rejected or timed out"; }
    else if (sa818_set_volume(cfg->volume, reply, sizeof(reply)) != ESP_OK ||
             !ack_ok(reply, "+DMOSETVOLUME")) { failed = "DMOSETVOLUME rejected or timed out"; }
    else if (sa818_set_filter(cfg->filter_pre_deemph, cfg->filter_highpass,
                             cfg->filter_lowpass, reply, sizeof(reply)) != ESP_OK ||
             !ack_ok(reply, "+DMOSETFILTER")) { failed = "SETFILTER rejected or timed out"; }
    else if (sa818_set_tail(false, reply, sizeof(reply)) != ESP_OK ||
             !ack_ok(reply, "+DMOSETTAIL")) { failed = "SETTAIL rejected or timed out"; }
    if (!failed && persist && store_profile(cfg) != ESP_OK) {
        failed = "profile applied but storage failed";
    }
    if (!failed) {
        board_sa818_high_power(cfg->high_power != 0);
        atomic_store(&s_ready, true);
        ESP_LOGI(TAG, "module filters acknowledged: pre/de=%d highpass=%d lowpass=%d (0=on)",
                 cfg->filter_pre_deemph, cfg->filter_highpass, cfg->filter_lowpass);
    }
    if (log && log_len) {
        if (failed) { snprintf(log, log_len, "%s; RF blocked", failed); }
        else {
            snprintf(log, log_len, "%s %.4f/%.4f MHz bw=%s sq=%d vol=%d pwr=%s",
                     sa818_band_name(sa818_cfg_band()), cfg->tx_mhz, cfg->rx_mhz,
                     cfg->bandwidth ? "25k" : "12.5k", cfg->squelch, cfg->volume,
                     cfg->high_power ? "high (H/L released)" : "low (H/L grounded)");
        }
    }
    atomic_store(&s_configuring, false);
    xSemaphoreGive(s_apply_mutex);
    return failed ? ESP_FAIL : ESP_OK;
}

esp_err_t sa818_cfg_apply(const sa818_cfg_t *cfg, char *log, size_t log_len)
{
    return apply_profile(cfg, false, log, log_len);
}

esp_err_t sa818_cfg_update(const sa818_cfg_t *cfg, char *log, size_t log_len)
{
    return apply_profile(cfg, true, log, log_len);
}

esp_err_t sa818_cfg_reset(void)
{
    if (!s_apply_mutex || xSemaphoreTake(s_apply_mutex, pdMS_TO_TICKS(2500)) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    atomic_store(&s_configuring, true);
    esp_err_t e = ESP_ERR_INVALID_STATE;
    if (!board_ptt_is_asserted()) {
        atomic_store(&s_ready, false);
        board_sa818_high_power(false);
        nvs_handle_t h;
        e = nvs_open(NVS_NS, NVS_READWRITE, &h);
        if (e == ESP_OK) {
            e = nvs_erase_key(h, NVS_KEY);
            if (e == ESP_ERR_NVS_NOT_FOUND) { e = ESP_OK; }
            if (e == ESP_OK) { e = nvs_commit(h); }
            nvs_close(h);
        }
    }
    atomic_store(&s_configuring, false);
    xSemaphoreGive(s_apply_mutex);
    return e;
}

esp_err_t sa818_cfg_set_power(bool high, char *log, size_t log_len)
{
    if (!s_apply_mutex || xSemaphoreTake(s_apply_mutex, pdMS_TO_TICKS(2500)) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    sa818_cfg_t cfg;
    esp_err_t e = sa818_cfg_load(&cfg);
    if (e == ESP_OK) { e = sa818_cfg_validate(&cfg, log, log_len); }
    if (e == ESP_OK && !sa818_cfg_tx_ready()) { e = ESP_ERR_INVALID_STATE; }
    if (e == ESP_OK) {
        cfg.high_power = high ? 1 : 0;
        e = store_profile(&cfg);
        if (e == ESP_OK) { board_sa818_high_power(high); }
    }
    if (log && log_len) {
        snprintf(log, log_len, "%s", e != ESP_OK ? "configure radio before changing power" :
                 board_sa818_power_pending() ? "power saved; applies after PTT release" :
                 high ? "high power: H/L released" : "low power: H/L grounded");
    }
    xSemaphoreGive(s_apply_mutex);
    return e;
}

esp_err_t sa818_cfg_set_squelch(int squelch, char *log, size_t log_len)
{
    if (squelch < 0 || squelch > 8) { return ESP_ERR_INVALID_ARG; }
    sa818_cfg_t cfg;
    if (sa818_cfg_load(&cfg) != ESP_OK) { return ESP_ERR_INVALID_STATE; }
    cfg.squelch = squelch;
    return sa818_cfg_update(&cfg, log, log_len);
}

void sa818_cfg_print(const sa818_cfg_t *cfg)
{
    if (!cfg) {
        return;
    }
    printf("  TX           : %.4f MHz\n", cfg->tx_mhz);
    printf("  RX           : %.4f MHz%s\n", cfg->rx_mhz,
           (cfg->tx_mhz != cfg->rx_mhz) ? "   (split / repeater)" : "");
    printf("  bandwidth    : %s\n", cfg->bandwidth ? "25 kHz" : "12.5 kHz");
    printf("  TX CTCSS     : %s%s\n", cfg->tx_ctcss,
           strcmp(cfg->tx_ctcss, "0000") ? "" : "  (off)");
    printf("  RX CTCSS     : %s%s\n", cfg->rx_ctcss,
           strcmp(cfg->rx_ctcss, "0000") ? "" : "  (off)");
    printf("  squelch      : %d%s\n", cfg->squelch, cfg->squelch == 0 ? "  (monitor)" : "");
    printf("  AF volume    : %d\n", cfg->volume);
    printf("  tail tone    : %s\n", cfg->tail ? "on" : "off");
    printf("  filters      : pre/de-emph=%d highpass=%d lowpass=%d\n",
           cfg->filter_pre_deemph, cfg->filter_highpass, cfg->filter_lowpass);
}

/* --------------------------------------------------------- idle refresh */

/*
 * Operator rule (2026-11-05): re-send the profile once after every over, but
 * only when the station is genuinely idle -- no PTT, the module's own squelch
 * shut, nothing playing from the network -- held for a second, and then five
 * seconds more.  A retune mutes the receiver, so it must never land inside a
 * call.  This path deliberately logs nothing.
 */
static bool s_refresh_pending;
static int64_t s_idle_since_us;
static int64_t s_refresh_due_us;

void sa818_cfg_arm_refresh(void)
{
    s_refresh_pending = true;
    s_idle_since_us = 0;
    s_refresh_due_us = 0;
}

static bool station_busy(void)
{
    if (ptt_get_state() != PTT_IDLE) { return true; }
    if (voice_module_squelch_open()) { return true; }
    if (voice_bridge_level() > 0) { return true; }
    if (voice_sdr_tail_active()) { return true; }
    return false;
}

void sa818_cfg_refresh_tick(void)
{
    if (!s_refresh_pending) { return; }
    const int64_t now = esp_timer_get_time();
    if (station_busy()) {
        s_idle_since_us = 0;            /* the window starts over when it clears */
        s_refresh_due_us = 0;
        return;
    }
    if (s_idle_since_us == 0) { s_idle_since_us = now; return; }
    if (s_refresh_due_us == 0) {
        if (now - s_idle_since_us < 1000000LL) { return; }
        s_refresh_due_us = now + 10000000LL;  /* one second confirmed, ten more to go */
        return;
    }
    if (now < s_refresh_due_us) { return; }
    sa818_cfg_t cfg;
    if (sa818_cfg_load(&cfg) == ESP_OK) {
        s_quiet_apply = true;
        (void)apply_profile(&cfg, true, NULL, 0);
        bridge_apply();
        s_quiet_apply = false;
    }
    s_refresh_pending = false;
    s_idle_since_us = 0;
    s_refresh_due_us = 0;
}
