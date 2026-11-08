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
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define NETCOM_MODE_LOCAL 0
#define portMAX_DELAY 0
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGE(tag, ...) ((void)(tag))
typedef int nvs_handle_t;
typedef int sa818_cfg_t;
typedef int *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t h, int timeout);
int xSemaphoreGive(SemaphoreHandle_t h);
int64_t esp_timer_get_time(void);
esp_err_t nvs_open(const char *, int, nvs_handle_t *);
esp_err_t nvs_get_u8(nvs_handle_t, const char *, uint8_t *);
esp_err_t nvs_set_u8(nvs_handle_t, const char *, uint8_t);
esp_err_t nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
int channels_count(void);
bool channels_get(int, char *, size_t, sa818_cfg_t *);
bool sa818_cfg_tx_ready(void);
bool sa818_cfg_compatible(const sa818_cfg_t *);
esp_err_t sa818_cfg_update(const sa818_cfg_t *, char *, size_t);
int netcom_mode(void);
bool netcom_engine_running(void);
void netcom_tx_sample(const int16_t *, size_t, bool);
void netcom_tx_end(void);
bool ptt_bridge_keyed(void);
void ptt_bridge_key(bool);
void voice_monitor_borrow(const char *);
void voice_monitor_release(const char *);
#endif
'''

HARNESS = r'''
#include "bridge.c"
static unsigned samples, ends, tunes, releases;
static bool engine_up = true, keyed, probe_release, radio_ready = true, compatible = true;
static esp_err_t tune_error;
static int mode = 1;
static int64_t clock_ms = 100;
static int16_t pcm[] = {0, 1, -1, 32767, -32768};
static char events[1024];
static unsigned event_n;

SemaphoreHandle_t xSemaphoreCreateMutex(void) { return calloc(1, sizeof(int)); }
int xSemaphoreTake(SemaphoreHandle_t h, int timeout) {
    (void)timeout; assert(*h == 0); *h = 1; return 1;
}
int xSemaphoreGive(SemaphoreHandle_t h) { assert(*h == 1); *h = 0; return 1; }
int64_t esp_timer_get_time(void) { return clock_ms++ * 1000; }
esp_err_t nvs_open(const char *ns, int rw, nvs_handle_t *h) {
    (void)ns; (void)rw; *h = 1; return ESP_OK;
}
esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *v) {
    (void)h; *v = strcmp(key, "brchan") == 0 ? 1 : 0; return ESP_OK;
}
esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t v) {
    (void)h; (void)key; (void)v; return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }
int channels_count(void) { return 1; }
bool channels_get(int i, char *name, size_t n, sa818_cfg_t *cfg) {
    (void)i; (void)name; (void)n; (void)cfg; return true;
}
bool sa818_cfg_tx_ready(void) { return radio_ready; }
bool sa818_cfg_compatible(const sa818_cfg_t *cfg) { (void)cfg; return compatible; }
esp_err_t sa818_cfg_update(const sa818_cfg_t *cfg, char *log, size_t n) {
    (void)cfg; if (n) *log = 0; ++tunes; return tune_error;
}
int netcom_mode(void) { return mode; }
bool netcom_engine_running(void) { return engine_up; }
void netcom_tx_sample(const int16_t *p, size_t n, bool ptt) {
    assert(ptt && n == 5 && memcmp(p, pcm, sizeof(pcm)) == 0);
    ++samples; events[event_n++] = 'S';
}
void netcom_tx_end(void) { ++ends; events[event_n++] = 'E'; }
bool ptt_bridge_keyed(void) { return keyed; }
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

    /* Missing/empty blocks must not flap a still-open stream. */
    tap(true, true);
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
    puts("PASS: squelch edge, repeated close, empty block, capture stop, preview, "
         "half-duplex, disable race, mode switch, RF profile preservation");
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
        for name in ('bridge.c', 'bridge.h'):
            shutil.copy2(ROOT / 'firmware/main' / name, directory / name)
        (directory / 'stubs.h').write_text(STUBS, encoding='utf-8')
        for name in ('esp_err.h', 'esp_log.h', 'esp_timer.h', 'nvs.h',
                     'channels.h', 'netcom.h', 'ptt.h', 'sa818_cfg.h', 'voice.h',
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
