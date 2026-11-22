#!/usr/bin/env python3
"""Host regression for the production bridge.c, with hardware/RTOS stubs.

Requires a host C compiler (gcc by default). Does not contact any board.
The production source is copied unchanged into a temporary build directory.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent

STUBS = r'''
#ifndef BRIDGE_TEST_STUBS_H
#define BRIDGE_TEST_STUBS_H
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
typedef int esp_err_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_NVS_NOT_FOUND 3
#define pdTRUE 1
#define pdMS_TO_TICKS(n) (n)
#define RX_GUARD_OUT_NET 1
#define PTT_TRACE_RFNET_ON 5
#define PTT_TRACE_RFNET_OFF 6
#define PTT_OWNER_NONE 0
static void ptt_trace_note(unsigned k,unsigned s,unsigned a,unsigned b) {(void)k;(void)s;(void)a;(void)b;}
#define AUDIO_SAMPLE_RATE_HZ 16000
#define AUDIO_PKT_SAMPLES 320
#define TAIL_OFF 0
#define TAIL_COUNT 9
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define NETCOM_MODE_LOCAL 0
#define NETCOM_MODE_NETP2P 1
#define NETCOM_MODE_RELAY 2
#define portMAX_DELAY 0
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGE(tag, ...) ((void)(tag))
typedef int nvs_handle_t;
#include "sa818_cfg.h"
typedef int *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t h, int timeout);
int xSemaphoreGive(SemaphoreHandle_t h);
int64_t esp_timer_get_time(void);
esp_err_t nvs_open(const char *, int, nvs_handle_t *);
esp_err_t nvs_get_u8(nvs_handle_t, const char *, uint8_t *);
esp_err_t nvs_set_u8(nvs_handle_t, const char *, uint8_t);
esp_err_t nvs_get_u32(nvs_handle_t, const char *, uint32_t *);
esp_err_t nvs_set_u32(nvs_handle_t, const char *, uint32_t);
esp_err_t nvs_get_blob(nvs_handle_t, const char *, void *, size_t *);
esp_err_t nvs_set_blob(nvs_handle_t, const char *, const void *, size_t);
esp_err_t nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
int channels_count(void);
bool channels_get(int, char *, size_t, sa818_cfg_t *);
bool sa818_cfg_tx_ready(void);
bool sa818_cfg_compatible(const sa818_cfg_t *);
esp_err_t sa818_cfg_update(const sa818_cfg_t *, char *, size_t);
int netcom_mode(void);
uint16_t netcom_room(void);
bool netcom_engine_running(void);
void netcom_tx_sample(const int16_t *, size_t, bool);
bool netcom_tx_sample_until(const int16_t *, size_t, bool, int64_t);
bool netcom_tx_room(size_t, bool, int64_t);
void netcom_tx_end(void);
bool ptt_bridge_keyed(void);
int ptt_owner(void);
void ptt_bridge_key(bool);
void voice_monitor_borrow(const char *);
void voice_monitor_release(const char *);
void rx_guard_init(void);
void rx_guard_context(uint8_t, bool);
uint8_t rx_guard_blocked(void);
bool tail_should_play(uint8_t);
int tail_get(void);
int tail_id_for_mode(uint8_t mode);
size_t tail_samples(int);
size_t tail_fetch(int, size_t, int16_t *, size_t);
#endif
'''

HARNESS = r'''
#include "channel_control.c"
#include "bridge.c"
void audio_spectrum_tap(audio_spectrum_path_t path, const int16_t *pcm, size_t n)
{ (void)path; (void)pcm; (void)n; }
void audio_spectrum_tap_session(audio_spectrum_path_t path, uint32_t session,
                                const int16_t *pcm, size_t n)
{ (void)path; (void)session; (void)pcm; (void)n; }
static unsigned samples, ends, tunes, releases;
static bool engine_up = true, keyed, probe_release, radio_ready = true, compatible = true;
static esp_err_t tune_error;
static int mode = 1;
static int64_t clock_ms = 100;
static int16_t pcm[] = {0, 1, -1, 32767, -32768};
static char events[1024];
static unsigned event_n;
static uint8_t inhibited;
static bool tail_enabled;
static unsigned tail_reads;
static sa818_cfg_t current;
static int channel_count = 1;
static bool storage_fail;
static uint8_t denoise_stored;
static uint8_t local_denoise_stored;
static bool denoise_exists, denoise_pending, commit_fail;
static bool legacy_config_exists;
static uint32_t legacy_config;
static uint32_t denoise_config, pending_config;
static uint32_t settings_revision, settings_paths, settings_level;
static uint32_t pending_revision, pending_paths, pending_level;
static unsigned denoise_commits;
static rf_denoise_level_t started_level;
static bool protect_during_finish;
static size_t queued_samples;
#define LEVEL(setting, level) ((uint32_t)(level) << (3 * ((setting) - 1)))
void rx_guard_init(void) {}
void rx_guard_context(uint8_t m, bool active) { (void)m; (void)active; }
uint8_t rx_guard_blocked(void) { return inhibited; }
bool tail_should_play(uint8_t m) { (void)m; return tail_enabled; }
int tail_get(void) { return 1; }
size_t tail_samples(int id) { (void)id; return 1; }
size_t tail_fetch(int id, size_t pos, int16_t *out, size_t n) {
    (void)id;
    if (pos || !n) { return 0; }
    out[0] = 123;
    ++tail_reads;
    return 1;
}

SemaphoreHandle_t xSemaphoreCreateMutex(void) { return calloc(1, sizeof(int)); }
int xSemaphoreTake(SemaphoreHandle_t h, int timeout) {
    if (h == s_select_mux && *h) { return 0; }
    (void)timeout; assert(*h == 0); *h = 1; return 1;
}
int xSemaphoreGive(SemaphoreHandle_t h) { assert(*h == 1); *h = 0; return 1; }
int64_t esp_timer_get_time(void) { return clock_ms++ * 1000; }
esp_err_t nvs_open(const char *ns, int rw, nvs_handle_t *h) {
    (void)ns; (void)rw; *h = 1; return ESP_OK;
}
esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *v) {
    (void)h;
    *v = strcmp(key, "brchan") == 0 ? 1 :
        strcmp(key, "rfdenoise1") == 0 ? denoise_stored :
        strcmp(key, "auddn_local1") == 0 ? local_denoise_stored : 0;
    return ESP_OK;
}
esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t v) {
    (void)h;
    if (storage_fail) { return ESP_ERR_INVALID_STATE; }
    if (strcmp(key, "rfdenoise1") == 0) { denoise_stored = v; }
    if (strcmp(key, "auddn_local1") == 0) { local_denoise_stored = v; }
    return ESP_OK;
}
esp_err_t nvs_get_u32(nvs_handle_t h, const char *key, uint32_t *v) {
    (void)h;
    if (strcmp(key, "auddn_cfg2") == 0) {
        if (!legacy_config_exists) { return ESP_ERR_INVALID_STATE; }
        *v = legacy_config;
        return ESP_OK;
    }
    if (strcmp(key, "auddn_lvl1") == 0) { return ESP_ERR_NVS_NOT_FOUND; }
    assert(strcmp(key, "auddn_sel1") == 0);
    if (!denoise_exists) { return ESP_ERR_INVALID_STATE; }
    *v = denoise_config;
    return ESP_OK;
}
esp_err_t nvs_set_u32(nvs_handle_t h, const char *key, uint32_t v) {
    (void)h; assert(strcmp(key, "auddn_sel1") == 0);
    if (storage_fail) { return ESP_ERR_INVALID_STATE; }
    denoise_pending = true; pending_config = v;
    return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *value, size_t *size) {
    (void)h;
    assert(!strcmp(key,"auddn_sel2") && *size == 5 * sizeof(uint32_t));
    if (!denoise_exists) { return ESP_ERR_NVS_NOT_FOUND; }
    const uint32_t record[] = {2, settings_revision, denoise_config, settings_paths, settings_level};
    memcpy(value,record,sizeof(record));
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *value, size_t size) {
    (void)h;
    assert(!strcmp(key,"auddn_sel2") && size == 5 * sizeof(uint32_t));
    if (storage_fail) { return ESP_ERR_INVALID_STATE; }
    const uint32_t *record = value;
    assert(record[0]==2);
    pending_revision=record[1]; pending_config=record[2]; pending_paths=record[3]; pending_level=record[4];
    denoise_pending=true;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) {
    (void)h;
    if (denoise_pending) {
        if (commit_fail) { return ESP_ERR_INVALID_STATE; }
        denoise_config = pending_config; denoise_exists = true; ++denoise_commits;
        settings_revision=pending_revision; settings_paths=pending_paths; settings_level=pending_level;
    }
    return ESP_OK;
}
void nvs_close(nvs_handle_t h) { (void)h; denoise_pending = false; }
int channels_count(void) { return channel_count; }
bool channels_get(int i, char *name, size_t n, sa818_cfg_t *cfg) {
    (void)name; (void)n;
    if (i < 0 || i >= channel_count) return false;
    if (cfg) { memset(cfg, 0, sizeof(*cfg)); cfg->rx_mhz = 433.5f + i * .0125f; cfg->tx_mhz = cfg->rx_mhz; }
    return true;
}
bool sa818_cfg_tx_ready(void) { return radio_ready; }
bool sa818_cfg_compatible(const sa818_cfg_t *cfg) { (void)cfg; return compatible; }
esp_err_t sa818_cfg_update(const sa818_cfg_t *cfg, char *log, size_t n) {
    if (n) *log = 0;
    if (!compatible) return ESP_ERR_INVALID_ARG;
    ++tunes;
    if (!tune_error) current = *cfg;
    return tune_error;
}
esp_err_t sa818_cfg_load(sa818_cfg_t *cfg) { *cfg = current; return ESP_OK; }
int netcom_mode(void) { return mode; }
uint16_t netcom_room(void) { return 1; }
void rf_denoise_prepare(rf_denoise_route_t route) { assert(route < RF_DENOISE_ROUTE_COUNT); }
void rf_denoise_stop(rf_denoise_route_t route) { (void)route; }
void rf_denoise_invalidate(rf_denoise_route_t route) { (void)route; }
bool rf_denoise_start(rf_denoise_route_t route, rf_denoise_level_t level) {
    assert((route == RF_DENOISE_BRIDGE || route == RF_DENOISE_RELAY_BRIDGE) && *s_rx_tx_mux);
    started_level = level;
    queued_samples = 0;
    return true;
}
bool rf_denoise_push(rf_denoise_route_t route, const int16_t *p, size_t n, rf_denoise_emit_t emit, void *ctx) {
    (void)p; (void)emit; (void)ctx;
    assert(route == RF_DENOISE_BRIDGE && !*s_rx_tx_mux);
    queued_samples += n;
    return true;
}
bool rf_denoise_finish(rf_denoise_route_t route, rf_denoise_emit_t emit, void *ctx) {
    assert(route == RF_DENOISE_BRIDGE && !*s_rx_tx_mux);
    if (protect_during_finish) inhibited = RX_GUARD_OUT_NET;
    return !queued_samples || emit(pcm, queued_samples, ctx);
}
void rf_denoise_stats(rf_denoise_route_t route, rf_denoise_stats_t *stats) {
    (void)route; *stats = (rf_denoise_stats_t){0};
}
bool netcom_engine_running(void) { return engine_up; }
void netcom_tx_sample(const int16_t *p, size_t n, bool ptt) {
    assert(ptt && ((n == 5 && memcmp(p, pcm, sizeof(pcm)) == 0) ||
                   (tail_enabled && n == 1 && p[0] == 123)));
    ++samples; events[event_n++] = 'S';
}
unsigned tx_timeout_seconds(void) { return 5; }
int tail_id_for_mode(uint8_t mode) { return tail_should_play(mode) ? tail_get() : TAIL_OFF; }
bool netcom_tx_room(size_t n, bool tail, int64_t deadline) {
    (void)n; (void)tail; return deadline > esp_timer_get_time();
}
bool netcom_tx_sample_until(const int16_t *pcm, size_t n, bool tail, int64_t deadline) {
    (void)tail;
    assert(deadline > esp_timer_get_time());
    netcom_tx_sample(pcm, n, true);
    return true;
}
void netcom_tx_end(void) { ++ends; events[event_n++] = 'E'; }
bool ptt_bridge_keyed(void) { return keyed; }
int ptt_owner(void) { return keyed ? 1 : PTT_OWNER_NONE; }
void ptt_bridge_key(bool on) { keyed = on; }
void voice_monitor_borrow(const char *who) { (void)who; }
void voice_monitor_release(const char *who) {
    (void)who; ++releases;
    /* A final monitor block can run during release. It must not reopen TX,
     * and the tap mutex must already have been released by bridge_apply. */
    if (probe_release) bridge_rx_tap(pcm, 5, true, true);
}
static void tap(bool open, bool enabled) { bridge_rx_tap(pcm, 5, open, enabled); }
int main(void) {
    channels_get(0, NULL, 0, &current);
    bridge_init();
    tap(false, true); tap(true, true);
    assert(samples == 0 && ends == 0);
    assert(bridge_set_en(true) == ESP_OK && bridge_active());
    tap(false, true); assert(ends == 0);

    /* Regression: a closed COS must deliver one end marker, after the PCM. */
    tap(true, true); tap(true, true); tap(false, true);
    for (int i = 0; i < 20; ++i) tap(false, true);
    assert(samples == 2 && ends == 1 && strcmp(events, "SSE") == 0);
    bridge_rx_stats_t stats;
    bridge_rx_stats(&stats);
    assert(!stats.open && stats.starts == 1 && stats.ends == 1);
    assert(stats.end_ms >= stats.start_ms);
    assert(!strcmp(stats.end_reason, "cos"));
    assert(bridge_rf_config_begin());
    tap(true, true); assert(samples == 2 && ends == 1);
    bridge_rf_config_end();

    /* Missing/empty blocks must not flap a still-open stream. */
    tap(true, true);
    assert(!bridge_rf_config_begin());
    bridge_rx_tap(NULL, 0, true, true);
    bridge_rx_tap(NULL, 5, true, true);
    assert(ends == 1);
    bridge_rx_end(); bridge_rx_end(); assert(ends == 2);

    /* Preview/injection takeover and half-duplex keying each close once. */
    tap(true, true); tap(true, false); tap(true, false); assert(ends == 3);
    tap(true, true); keyed = true; tap(true, true); tap(true, true);
    assert(ends == 4); keyed = false;

    /* Disable must block a racing final block before stopping the monitor. */
    tap(true, true); unsigned before = samples; probe_release = true;
    bridge_set_en(false);
    assert(!bridge_active() && samples == before && ends == 5 && releases == 1);
    bridge_set_en(false); tap(true, true); assert(ends == 5);

    /* Engine stop/mode switch reset the bridge; next call is a fresh stream. */
    bridge_set_en(true); tap(true, true);
    engine_up = false; bridge_mode_changed(); assert(ends == 6 && !bridge_active());
    engine_up = true; mode = 2; bridge_mode_changed();
    tap(true, true); tap(false, true); assert(ends == 7);
    mode = 0; bridge_mode_changed(); tap(true, true); assert(ends == 7);
    assert(tunes == 0);  /* All toggles must preserve the user's RF profile. */
    bridge_rx_stats(&stats);
    assert(stats.starts == 7 && stats.ends == 7 && !stats.open);
    mode = 1;
    radio_ready = false; bridge_apply();
    assert(!bridge_active());
    radio_ready = true; compatible = false; bridge_apply();
    assert(!bridge_active());
    assert(bridge_set_chan(1) == ESP_ERR_INVALID_ARG && tunes == 0);
    compatible = true; bridge_apply(); assert(bridge_active());
    bridge_channel_deleted(0);
    assert(bridge_chan() == 0 && !bridge_active() && tunes == 0);
    tune_error = ESP_ERR_INVALID_ARG;
    assert(bridge_set_chan(1) == tune_error && bridge_chan() == 0);
    tune_error = ESP_OK;
    assert(bridge_set_chan(1) == ESP_OK && bridge_chan() == 1 && bridge_active());
    tail_enabled = true;
    bridge_rx_tap(pcm, 5, true, true);
    const unsigned before_samples = samples, before_ends = ends;
    inhibited = RX_GUARD_OUT_NET;
    bridge_rx_tap(pcm, 5, true, true);
    bridge_rx_tap(pcm, 5, true, true);
    assert(samples == before_samples && ends == before_ends + 1 && tail_reads == 0);
    inhibited = 0;
    bridge_rx_tap(pcm, 5, true, true);
    bridge_rx_inhibit();
    assert(tail_reads == 0);
    bridge_rx_tap(pcm, 5, true, true);
    tap(false, true); tap(false, true);
    assert(tail_reads == 1); /* a normal over still has exactly one tone */
    /* Re-selecting the same bridge channel must restore a profile changed by
     * a frequency editor. A separate stale channel cursor cannot prevent it. */
    char log[192];
    unsigned t = tunes;
    current.squelch = 5;
    assert(channel_current() == 0);
    assert(channel_select(1, true, log, sizeof(log)) == ESP_OK && tunes == t + 1);
    assert(current.squelch == 0 && channel_current() == 1);
    assert(channel_select(1, true, log, sizeof(log)) == ESP_OK && tunes == t + 2);
    channel_count = 3;
    assert(channel_select(2, false, log, sizeof(log)) == ESP_OK);
    assert(channel_current() == 2 && bridge_chan() == 2);
    tune_error = ESP_ERR_INVALID_STATE;
    assert(channel_select(3, false, log, sizeof(log)) == tune_error);
    assert(channel_current() == 2 && bridge_chan() == 2);
    tune_error = 0;
    assert(bridge_set_en(false) == 0);
    assert(channel_select(3, false, log, sizeof(log)) == 0);
    assert(channel_current() == 3 && bridge_chan() == 2); /* retain bridge preselection */
    assert(channel_select(0, true, log, sizeof(log)) == 0 && bridge_chan() == 0);
    storage_fail = true;
    assert(bridge_set_en(true) != 0 && !bridge_en());
    assert(channel_select(1, true, log, sizeof(log)) != 0 && bridge_chan() == 0);
    assert(strstr(log, "保存失败"));
    storage_fail = false;
    assert(channel_select(99, true, log, sizeof(log)) == ESP_ERR_INVALID_ARG);
    assert(bridge_chan() == 0 && channel_current() == 1);
    *s_select_mux = 1;
    assert(channel_select(2, true, log, sizeof(log)) == ESP_ERR_INVALID_STATE);
    assert(channel_current() == 1 && bridge_chan() == 0 && strstr(log, "正在切换"));
    *s_select_mux = 0;
    /* A short denoised over is drained before the unmodified tail and OFF. */
    assert(bridge_denoise_set(1, true, false) == ESP_OK &&
           (denoise_config & DENOISE_LEVELS_MAX) == LEVEL(1, RF_DENOISE_LEVEL_STANDARD));
    assert(bridge_denoise_set(2, true, false) == ESP_OK && bridge_denoise_flags(denoise_config) == 6);
    storage_fail = true;
    assert(bridge_denoise_set(2, false, true) != ESP_OK && bridge_denoise_flags(denoise_config) == 6);
    storage_fail = false;
    assert(bridge_denoise_set(2, false, true) == ESP_OK && bridge_denoise_flags(denoise_config) == 2);
    mode = 1;
    tail_enabled = true;
    assert(bridge_set_chan(1) == ESP_OK && bridge_set_en(true) == ESP_OK && bridge_active());
    unsigned at = event_n;
    tap(true, true);
    assert(started_level == RF_DENOISE_LEVEL_STANDARD);
    assert(bridge_denoise_set(1, false, true) == ESP_OK);
    tap(false, true);
    tap(false, true);
    assert(event_n == at + 3 && !memcmp(events + at, "SSE", 3));
    at = event_n;
    tap(true, true);
    assert(event_n == at + 1 && events[at] == 'S');
    tap(false, true);
    tap(false, true);
    assert(bridge_denoise_update_levels(1, 2, 0, LEVEL(1, RF_DENOISE_LEVEL_EXTREME)) == ESP_OK);
    at = event_n;
    tap(true, true);
    assert(started_level == RF_DENOISE_LEVEL_EXTREME);
    protect_during_finish = true;
    tap(false, true);
    assert(event_n == at + 1 && events[at] == 'E');
    inhibited = 0;
    protect_during_finish = false;
    at = event_n;
    tap(true, true);
    bridge_rx_room_change(true);
    tap(true, true);
    bridge_rx_room_change(false);
    assert(event_n == at + 1 && events[at] == 'E' && !s_rx.open);
    assert(!(bridge_denoise_flags(bridge_denoise_levels()) & 24u));
    assert(bridge_denoise_set(DENOISE_SETTING_SPEAKER, true, false) == ESP_OK);
    assert((bridge_denoise_flags(bridge_denoise_levels()) & 24u) == 8u);
    assert(bridge_denoise_set(DENOISE_SETTING_HANDSET_TX, true, false) == ESP_OK);
    assert(bridge_denoise_flags(denoise_config) == 26 && denoise_stored == 0 && local_denoise_stored == 0);
    assert(bridge_denoise_set(DENOISE_SETTING_SPEAKER, false, true) == ESP_OK);
    bridge_init();
    assert(bridge_denoise_flags(bridge_denoise_levels()) == 18);
    assert(bridge_denoise_level(bridge_denoise_levels(), 1) == RF_DENOISE_LEVEL_EXTREME);
    const uint32_t p2p = (1u << 5) | (1u << 6) | (1u << 7);
    const uint32_t relay = (1u << 8) | (1u << 9) | (1u << 10);
    assert(bridge_denoise_update(0, p2p, 0, p2p) == ESP_ERR_INVALID_ARG);
    assert(bridge_denoise_update(1, p2p | relay, 0, p2p) == ESP_ERR_INVALID_ARG);
    unsigned commits_before = denoise_commits;
    commit_fail = true;
    assert(bridge_denoise_update(1, p2p, 0, p2p) != ESP_OK);
    assert(bridge_denoise_levels() == (denoise_config & DENOISE_LEVELS_MAX) && bridge_denoise_flags(denoise_config) == 18 &&
           denoise_commits == commits_before);
    commit_fail = false;
    assert(bridge_denoise_update(1, p2p, 0, p2p) == ESP_OK);
    assert(denoise_commits == ++commits_before && bridge_denoise_flags(denoise_config) == (18 | p2p));
    assert(bridge_denoise_update(1, p2p, 0, p2p) == ESP_OK && denoise_commits == commits_before);
    assert(bridge_denoise_update(1, p2p, 0, 0) == ESP_ERR_INVALID_STATE);
    assert(bridge_denoise_update(2, relay, 0, relay) == ESP_OK);
    bridge_init();
    assert(bridge_denoise_flags(bridge_denoise_levels()) == (18 | p2p | relay));
    denoise_exists = false; denoise_stored = 6; local_denoise_stored = 24;
    bridge_init();
    assert(bridge_denoise_flags(bridge_denoise_levels()) == 30 && !denoise_exists);
    assert(bridge_denoise_update(2, relay, 0, relay) == ESP_OK);
    assert(bridge_denoise_flags(denoise_config) == (30 | relay) && denoise_stored == 6 && local_denoise_stored == 24);

    /* Migrate the recent Boolean mask, preserve that rollback record, and
       check that changes to one saved strength cannot overwrite another. */
    legacy_config_exists = true;
    legacy_config = (1u << 4) | (1u << 6) | (1u << 10);
    denoise_config = LEVEL(1, 7); // An invalid level falls back to the old record.
    bridge_init();
    const uint32_t migrated = LEVEL(4, RF_DENOISE_LEVEL_STANDARD) |
        LEVEL(6, RF_DENOISE_LEVEL_STANDARD) | LEVEL(10, RF_DENOISE_LEVEL_STANDARD);
    assert(bridge_denoise_levels() == migrated && denoise_config == LEVEL(1, 7));
    assert(bridge_denoise_update_levels(1, p2p, 0, LEVEL(5, 5)) == ESP_ERR_INVALID_ARG);
    assert(bridge_denoise_update_levels(1, p2p, 0, DENOISE_LEVELS_MAX + 1u) == ESP_ERR_INVALID_ARG);
    assert(bridge_denoise_update_levels(1, p2p, LEVEL(8, 1), 0) == ESP_ERR_INVALID_ARG);
    assert(bridge_denoise_update_levels(1, relay, 0, 0) == ESP_ERR_INVALID_ARG);
    const uint32_t chosen = LEVEL(5, RF_DENOISE_LEVEL_LIGHT) | LEVEL(6, RF_DENOISE_LEVEL_STRONG) |
        LEVEL(7, RF_DENOISE_LEVEL_EXTREME);
    assert(bridge_denoise_update_levels(1, p2p, 0, chosen) == ESP_ERR_INVALID_STATE);
    assert(bridge_denoise_levels() == migrated && denoise_config == LEVEL(1, 7));
    assert(bridge_denoise_update_levels(1, p2p, LEVEL(6, RF_DENOISE_LEVEL_STANDARD), chosen) == ESP_OK);
    const uint32_t persisted = LEVEL(4, RF_DENOISE_LEVEL_STANDARD) | chosen | LEVEL(10, RF_DENOISE_LEVEL_STANDARD);
    assert(bridge_denoise_levels() == persisted && (denoise_config & DENOISE_LEVELS_MAX) == persisted);
    commits_before = denoise_commits;
    assert(bridge_denoise_update_levels(1, p2p, 0, chosen) == ESP_OK);
    assert(bridge_denoise_update(1, p2p, 0, p2p) == ESP_OK);
    assert(denoise_commits == commits_before && bridge_denoise_levels() == persisted);
    assert(bridge_denoise_update_levels(2, 1u << 10, LEVEL(10, RF_DENOISE_LEVEL_STANDARD), 0) == ESP_OK);
    bridge_init();
    assert(bridge_denoise_levels() == (persisted & ~LEVEL(10, 7)));
    assert(legacy_config == ((1u << 4) | (1u << 6) | (1u << 10)));
    mode = 1;
    const bool enabled = (bridge_denoise_flags(bridge_denoise_levels()) & (1u << 1)) != 0;
    assert(bridge_denoise_set(1, false, enabled) == ESP_OK);
    assert(bridge_set_chan(1) == ESP_OK && bridge_set_en(true) == ESP_OK && bridge_active());
    tail_enabled = true;
    const unsigned start_count = samples, end_count = ends;
    tap(true, true);
    clock_ms += 5000;
    tap(true, true);
    tap(true, true);
    tap(true, true);
    assert(samples == start_count + 2 && ends == end_count + 1);
    tap(false, true);
    tap(true, true);
    assert(samples == start_count + 3);
    bridge_rx_end();
    puts("PASS: RF to network cutoff sends one tail, holds out until true COS closes");
    puts("PASS: RX inhibit drops PCM without a synthetic tail; normal tail preserved; squelch edge, repeated close, empty block, capture stop, preview, "
         "half-duplex, disable race, mode switch, RF profile preservation; common channel application, "
         "same-channel retune, profile matching, bridge follow/preselection, busy and storage failure; "
         "denoise save isolation, short drain, protection during drain and room changes");
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='gcc')
    args = parser.parse_args()
    compiler = shutil.which(args.cc)
    if not compiler:
        parser.error('host compiler not found: ' + args.cc)
    scratch = ROOT / '.tmp'
    scratch.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='orl-bridge-', dir=scratch) as temp:
        directory = Path(temp)
        environment = dict(os.environ, TEMP=temp, TMP=temp, TMPDIR=temp)
        (directory / 'freertos').mkdir()
        for name in ('bridge.c', 'bridge.h', 'audio_spectrum.h', 'rf_denoise.h', 'channel_control.c', 'channel_control.h', 'sa818_cfg.h', 'sa818_freq.h', 'tx_timeout.h'):
            shutil.copy2(ROOT / 'firmware/main' / name, directory / name)
        (directory / 'stubs.h').write_text(STUBS, encoding='utf-8')
        for name in ('esp_err.h', 'esp_log.h', 'esp_timer.h', 'nvs.h',
                     'channels.h', 'audio_i2s.h', 'netcom.h', 'ptt.h', 'voice.h', 'voice_tail.h', 'rx_guard.h',
                     'freertos/FreeRTOS.h', 'freertos/semphr.h'):
            (directory / name).write_text('#include "stubs.h"\n', encoding='utf-8')
        (directory / 'test.c').write_text(HARNESS, encoding='utf-8')
        executable = directory / 'test.exe'
        subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-I', str(directory), str(directory / 'test.c'),
                        '-o', str(executable)], check=True, env=environment)
        subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    main()
