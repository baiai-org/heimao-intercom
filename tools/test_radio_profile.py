#!/usr/bin/env python3
"""Host checks for actual radio profile/frequency code and power GPIO functions.

No boards are contacted. NVS, UART, RTOS and GPIO are simulated. board_hw.c's
power/safe-state sections are extracted unchanged, excluding unrelated ADC/I2C.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
MAIN = ROOT / 'firmware/main'

STUBS = r'''
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 1
#define ESP_ERR_INVALID_ARG 2
#define ESP_ERR_INVALID_STATE 3
#define ESP_ERR_INVALID_SIZE 4
#define ESP_ERR_NVS_NOT_FOUND 5
#define ESP_ERR_TIMEOUT 6
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGE(tag, ...) ((void)(tag))
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define pdMS_TO_TICKS(ms) (ms)
#define pdTRUE 1
#define portMUX_INITIALIZER_UNLOCKED 0
typedef int portMUX_TYPE;
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))
typedef int *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t, int);
int xSemaphoreGive(SemaphoreHandle_t);
typedef int nvs_handle_t;
esp_err_t nvs_open(const char *, int, nvs_handle_t *);
esp_err_t nvs_get_u8(nvs_handle_t, const char *, uint8_t *);
esp_err_t nvs_set_u8(nvs_handle_t, const char *, uint8_t);
esp_err_t nvs_get_blob(nvs_handle_t, const char *, void *, size_t *);
esp_err_t nvs_set_blob(nvs_handle_t, const char *, const void *, size_t);
esp_err_t nvs_erase_key(nvs_handle_t, const char *);
esp_err_t nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
#define PIN_SA818_HL 10
#define PIN_SA818_PTT 21
#define PIN_SA818_PD 14
#define PIN_AMP_SHDN 13
#define SA818_PD_POWERDOWN_ACTIVE_LOW 1
#define GPIO_MODE_INPUT_OUTPUT 1
#define GPIO_MODE_OUTPUT_OD 2
#define GPIO_PULLUP_DISABLE 0
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_DISABLE 0
#define GPIO_LL_GET_HW(n) (n)
typedef struct { uint64_t pin_bit_mask; int mode, pull_up_en, pull_down_en, intr_type; } gpio_config_t;
typedef struct { bool open_drain, pull_up, pull_down, output; } board_sa818_power_io_t;
int gpio_set_level(int, int);
int gpio_get_level(int);
int gpio_config(const gpio_config_t *);
void gpio_ll_get_io_config(int, int, bool *, bool *, bool *, bool *, bool *, uint32_t *, uint32_t *, uint32_t *, bool *);
void board_sa818_power(bool);
void board_sa818_high_power(bool);
int64_t esp_timer_get_time(void);
void bridge_apply(void);
#define PTT_IDLE 0
int ptt_get_state(void);
bool voice_module_squelch_open(void);
bool voice_rf_carrier_present(void);
bool voice_network_busy(void);
int voice_bridge_level(void);
bool voice_sdr_tail_active(void);
bool board_sa818_power_pending(void);
bool board_ptt_is_asserted(void);
bool board_safe_state_verify(int *, int *, int *);
'''

HARNESS = r'''
#include <math.h>
#include <stddef.h>
#include "sa818_cfg.c"
#include "board_power.c"
_Static_assert(sizeof(sa818_cfg_t) == 56, "legacy blob size changed");
_Static_assert(offsetof(sa818_cfg_t, tx_mhz) == 4, "legacy field moved");
_Static_assert(offsetof(sa818_cfg_t, filter_lowpass) == 52, "legacy field moved");
static sa818_cfg_t stored;
static bool have_blob, have_band, fail_store;
static uint8_t stored_band;
static unsigned blob_writes, band_writes, commands, command_step;
static int fail_ack;
static sa818_profile_t last_group;
static int last_volume, last_filters[3];
static int pin_level[64], pin_mode[64];
static bool pull_up[64], pull_down[64];
int64_t esp_timer_get_time(void) { return 0; }
void bridge_apply(void) {}
int ptt_get_state(void) { return board_ptt_is_asserted() ? 1 : PTT_IDLE; }
bool voice_module_squelch_open(void) { return false; }
bool voice_rf_carrier_present(void) { return false; }
bool voice_network_busy(void) { return false; }
int voice_bridge_level(void) { return 0; }
bool voice_sdr_tail_active(void) { return false; }

SemaphoreHandle_t xSemaphoreCreateMutex(void) { return calloc(1, sizeof(int)); }
int xSemaphoreTake(SemaphoreHandle_t h, int timeout) {
    (void)timeout; assert(*h == 0); *h = 1; return pdTRUE;
}
int xSemaphoreGive(SemaphoreHandle_t h) { assert(*h == 1); *h = 0; return pdTRUE; }
esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *h) {
    (void)mode; assert(!strcmp(ns, "orl")); *h = 1; return ESP_OK;
}
esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *out) {
    (void)h; assert(!strcmp(key, "rfband"));
    if (!have_band) return ESP_ERR_NVS_NOT_FOUND;
    *out = stored_band; return ESP_OK;
}
esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t val) {
    (void)h; assert(!strcmp(key, "rfband"));
    if (fail_store) return ESP_FAIL;
    have_band = true; stored_band = val; ++band_writes; return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *n) {
    (void)h; assert(!strcmp(key, "sa818"));
    if (!have_blob) return ESP_ERR_NVS_NOT_FOUND;
    assert(*n == sizeof(stored)); memcpy(out, &stored, *n); return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *in, size_t n) {
    (void)h; assert(!strcmp(key, "sa818") && n == sizeof(stored));
    if (fail_store) return ESP_FAIL;
    memcpy(&stored, in, n); have_blob = true; ++blob_writes; return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return fail_store ? ESP_FAIL : ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key) {
    (void)h; assert(!strcmp(key, "sa818")); have_blob = false; return ESP_OK;
}
int gpio_set_level(int pin, int level) {
    /* Any high on H/L must release an already configured open drain. */
    if (pin == PIN_SA818_HL && level) assert(pin_mode[pin] == GPIO_MODE_OUTPUT_OD);
    pin_level[pin] = level; return ESP_OK;
}
int gpio_get_level(int pin) { return pin_level[pin]; }
int gpio_config(const gpio_config_t *io) {
    for (int p = 0; p < 64; ++p) if (io->pin_bit_mask & (UINT64_C(1) << p)) {
        if (p == PIN_SA818_HL) {
            assert(!pin_level[p] && io->mode == GPIO_MODE_OUTPUT_OD);
            assert(!io->pull_up_en && !io->pull_down_en);
        }
        if (p == PIN_SA818_PTT) assert(pin_level[p] == 1);
        pin_mode[p] = io->mode; pull_up[p] = io->pull_up_en; pull_down[p] = io->pull_down_en;
    }
    return ESP_OK;
}
void gpio_ll_get_io_config(int hw, int pin, bool *pu, bool *pd, bool *ie, bool *oe,
                          bool *od, uint32_t *drv, uint32_t *fn, uint32_t *sig, bool *slp) {
    (void)hw; *pu = pull_up[pin]; *pd = pull_down[pin]; *ie = false; *oe = true;
    *od = pin_mode[pin] == GPIO_MODE_OUTPUT_OD; *drv = *fn = *sig = 0; *slp = false;
}
static esp_err_t ack(char *out, size_t n, const char *tag) {
    assert(!sa818_cfg_tx_ready()); ++commands; ++command_step;
    snprintf(out, n, "%s:%d", tag, fail_ack == (int)command_step ? 1 : 0); return ESP_OK;
}
esp_err_t sa818_set_group(const sa818_profile_t *p, char *out, size_t n) {
    assert(sa818_freq_pair_valid(sa818_cfg_band(), p->tx_mhz, p->rx_mhz));
    last_group = *p;
    command_step = 0; return ack(out, n, "+DMOSETGROUP");
}
esp_err_t sa818_set_volume(int vol, char *out, size_t n) {
    last_volume = vol;
    assert(vol >= 1 && vol <= 8); return ack(out, n, "+DMOSETVOLUME");
}
esp_err_t sa818_set_filter(int a, int b, int c, char *out, size_t n) {
    last_filters[0] = a; last_filters[1] = b; last_filters[2] = c;
    return ack(out, n, "+DMOSETFILTER");
}
esp_err_t sa818_set_tail(bool on, char *out, size_t n) {
    assert(!on); return ack(out, n, "+DMOSETTAIL");
}
static void frequencies(void) {
    unsigned checked = 0;
    for (int band = SA818_BAND_UHF; band <= SA818_BAND_VHF; ++band) {
        int low = (int)sa818_band_min(band) * 2000, high = (int)sa818_band_max(band) * 2000;
        for (int unit = low; unit <= high; ++unit) {
            char text[32]; snprintf(text, sizeof(text), "%.4f", (double)unit / 2000);
            float f = -1; assert(sa818_freq_parse(text, &f));
            assert(sa818_freq_pair_valid(band, f, f));
            char formatted[16]; float again;
            assert(sa818_freq_format(f, formatted, sizeof(formatted)));
            assert(sa818_freq_parse(formatted, &again) && f == again);
            assert(fabs(strtod(formatted, NULL) - strtod(text, NULL)) < 0.00000001);
            assert(!sa818_freq_pair_valid(band == 1 ? 2 : 1, f, f)); ++checked;
        }
    }
    const char *bad[] = {"", "nan", "inf", "438.5junk", "4.385e2", "0x1p8", "438.5001",
                         "144.7562", "144.7563", "144.75625", "144.75624", "144.75626", "438.50001",
                         "133.9875", "174.0125", "399.9875", "480.0125", "250", "-438.5", "438.5,1"};
    for (unsigned i = 0; i < sizeof(bad)/sizeof(bad[0]); ++i) {
        float f = -7; assert(!sa818_freq_parse(bad[i], &f) && f == -7);
    }
    assert(!sa818_freq_pair_valid(1, NAN, 438.5f));
    assert(!sa818_freq_pair_valid(1, 438.5f, 145.0f));
    printf("PASS: %u frequency grid points/float roundtrips and invalid inputs\n", checked);
}
int main(int argc, char **argv) {
    assert(argc == 2); frequencies();
    sa818_cfg_defaults(&stored);
    assert(stored.tx_mhz == 0 && stored.rx_mhz == 0);
    assert(stored.filter_highpass == 1);
    stored.filter_highpass = 0; /* Profile saved during the module-high-pass trial. */
    if (!strcmp(argv[1], "legacy")) { have_blob = true; stored.tx_mhz = 431.8f; stored.rx_mhz = 431.8f; }
    if (!strcmp(argv[1], "vhf")) {
        have_blob = have_band = true; stored_band = SA818_BAND_VHF;
        stored.tx_mhz = 145.0125f; stored.rx_mhz = 145.0125f;
    }
    sa818_cfg_t original = stored, cfg;
    assert(board_safe_state_init() == ESP_OK && pin_level[PIN_SA818_PTT] == 1);
    assert(sa818_cfg_init() == ESP_OK && !sa818_cfg_tx_ready());
    assert(blob_writes == 0 && memcmp(&stored, &original, sizeof(stored)) == 0);
    if (!strcmp(argv[1], "legacy")) assert(sa818_cfg_band() == 1 && band_writes == 1);
    if (!strcmp(argv[1], "vhf")) assert(sa818_cfg_band() == 2 && band_writes == 0);
    if (!strcmp(argv[1], "fresh")) assert(sa818_cfg_band() == 0 && band_writes == 0);
    board_ptt_assert(true); assert(!board_ptt_is_asserted());
    char log[160];
    if (have_blob) {
        assert(sa818_cfg_load(&cfg) == ESP_OK && cfg.filter_highpass == 1);
        sa818_cfg_t expected = original; expected.filter_highpass = 1;
        assert(!memcmp(&cfg, &expected, sizeof(cfg)) && blob_writes == 0);
        assert(sa818_cfg_apply(&original, log, sizeof(log)) == ESP_OK);
        assert(last_filters[1] == 1 && !memcmp(&stored, &original, sizeof(stored)));
        assert(original.filter_highpass == 0 && blob_writes == 0);
    }
    assert(sa818_cfg_set_band(SA818_BAND_UHF) == ESP_OK);
    sa818_cfg_defaults(&cfg); cfg.tx_mhz = cfg.rx_mhz = 438.5f;
    strcpy(cfg.tx_ctcss, "0017"); strcpy(cfg.rx_ctcss, "0008");
    cfg.squelch = 7; cfg.volume = 6;
    for (int pre = 0; pre <= 1; ++pre) for (int low = 0; low <= 1; ++low) {
        cfg.filter_pre_deemph = pre; cfg.filter_lowpass = low; cfg.filter_highpass = 0;
        sa818_cfg_t caller = cfg, expected = cfg; expected.filter_highpass = 1;
        unsigned writes_before = blob_writes;
        assert(sa818_cfg_apply(&cfg, log, sizeof(log)) == ESP_OK);
        assert(last_filters[0] == pre && last_filters[1] == 1 && last_filters[2] == low);
        assert(last_group.tx_mhz == cfg.tx_mhz && last_group.rx_mhz == cfg.rx_mhz);
        assert(last_group.bandwidth == cfg.bandwidth && last_group.squelch == cfg.squelch);
        assert(!strcmp(last_group.tx_ctcss, cfg.tx_ctcss) && !strcmp(last_group.rx_ctcss, cfg.rx_ctcss));
        assert(last_volume == cfg.volume && blob_writes == writes_before);
        assert(!memcmp(&cfg, &caller, sizeof(cfg)));
        assert(sa818_cfg_update(&cfg, log, sizeof(log)) == ESP_OK);
        assert(!memcmp(&stored, &expected, sizeof(stored)) && !memcmp(&cfg, &caller, sizeof(cfg)));
    }
    puts("PASS: module high-pass bypass for boot/channel profiles; RF fields and other filters preserved");
    assert(sa818_cfg_update(&cfg, log, sizeof(log)) == ESP_OK && sa818_cfg_tx_ready());
    sa818_cfg_t last = stored, invalid = cfg; invalid.rx_mhz = 145;
    unsigned writes = blob_writes, sent = commands;
    assert(sa818_cfg_update(&invalid, log, sizeof(log)) != ESP_OK);
    assert(commands == sent && blob_writes == writes && sa818_cfg_tx_ready());
    assert(memcmp(&stored, &last, sizeof(stored)) == 0);
    board_ptt_assert(true); assert(board_ptt_is_asserted());
    assert(sa818_cfg_set_band(2) != ESP_OK && sa818_cfg_band() == 1);
    assert(sa818_cfg_update(&cfg, log, sizeof(log)) != ESP_OK && commands == sent);
    assert(sa818_cfg_set_power(true, log, sizeof(log)) == ESP_OK);
    assert(commands == sent && !board_sa818_high_power_get() && board_sa818_power_pending());
    board_ptt_assert(false);
    assert(board_sa818_high_power_get() && !board_sa818_power_pending());
    assert(sa818_cfg_set_power(false, log, sizeof(log)) == ESP_OK && commands == sent);
    for (fail_ack = 1; fail_ack <= 4; ++fail_ack) {
        writes = blob_writes;
        assert(sa818_cfg_update(&cfg, log, sizeof(log)) != ESP_OK && !sa818_cfg_tx_ready());
        board_ptt_assert(true); assert(!board_ptt_is_asserted() && blob_writes == writes);
    }
    fail_ack = 0; fail_store = true;
    assert(sa818_cfg_update(&cfg, log, sizeof(log)) != ESP_OK && !sa818_cfg_tx_ready());
    fail_store = false;
    assert(sa818_cfg_update(&cfg, log, sizeof(log)) == ESP_OK);
    sa818_cfg_t stale_refresh = stored, new_channel = stored;
    new_channel.rx_mhz += .0125f;
    assert(sa818_cfg_update(&new_channel, log, sizeof(log)) == ESP_OK);
    sent = commands; writes = blob_writes;
    assert(apply_profile(&stale_refresh, true, NULL, 0, true) == ESP_ERR_INVALID_STATE);
    assert(commands == sent && blob_writes == writes && stored.rx_mhz == new_channel.rx_mhz);
    assert(apply_profile(&new_channel, true, NULL, 0, true) == ESP_OK);
    assert(sa818_cfg_update(&cfg, log, sizeof(log)) == ESP_OK);
    writes = blob_writes; sent = commands;
    assert(sa818_cfg_set_band(2) == ESP_OK && !sa818_cfg_tx_ready());
    assert(blob_writes == writes && commands == sent);
    assert(sa818_cfg_load(&last) == ESP_OK && last.tx_mhz == 438.5f);
    assert(sa818_cfg_apply(&last, log, sizeof(log)) != ESP_OK && commands == sent);
    cfg.tx_mhz = 145.0125f; cfg.rx_mhz = 145.6125f;
    assert(sa818_cfg_update(&cfg, log, sizeof(log)) == ESP_OK && sa818_cfg_tx_ready());
    for (int bw = 0; bw <= 1; ++bw) {
        cfg.bandwidth = bw; cfg.tx_mhz = 145.0025f; cfg.rx_mhz = 145.6010f;
        assert(sa818_cfg_update(&cfg, log, sizeof(log)) == ESP_OK);
        assert(sa818_cfg_load(&last) == ESP_OK);
        char exact[16]; assert(sa818_freq_format(last.tx_mhz, exact, sizeof(exact)));
        assert(!strcmp(exact, "145.0025"));
        assert(sa818_freq_format(last.rx_mhz, exact, sizeof(exact)) && !strcmp(exact, "145.6010"));
        assert(last_group.tx_mhz == cfg.tx_mhz && last_group.rx_mhz == cfg.rx_mhz);
    }
    writes = blob_writes;
    assert(sa818_cfg_apply(&cfg, log, sizeof(log)) == ESP_OK && blob_writes == writes);
    assert(sa818_cfg_reset() == ESP_OK && sa818_cfg_band() == 2 && !sa818_cfg_tx_ready());
    assert(!have_blob && sa818_cfg_load(&last) != ESP_OK && last.tx_mhz == 0);
    puts("PASS: migration, no frequency invention, band/PTT guards, ACK/storage failures, open drain/deferred power, reset");
    return 0;
}
'''


def main():
    scratch = ROOT / '.tmp'
    scratch.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='orl-radio-', dir=scratch) as temp:
        d = Path(temp)
        env = dict(os.environ, TEMP=temp, TMP=temp, TMPDIR=temp)
        (d / 'freertos').mkdir()
        for name in ('sa818_cfg.c', 'sa818_cfg.h', 'sa818_freq.c', 'sa818_freq.h', 'sa818.h', 'ctcss.c', 'ctcss.h'):
            shutil.copy2(MAIN / name, d / name)
        (d / 'stubs.h').write_text(STUBS)
        for name in ('esp_err.h', 'esp_log.h', 'nvs.h', 'nvs_flash.h', 'board_hw.h', 'bridge.h', 'ptt.h', 'voice.h', 'esp_timer.h',
                     'freertos/FreeRTOS.h', 'freertos/semphr.h'):
            (d / name).write_text('#include "stubs.h"\n')
        board = (MAIN / 'board_hw.c').read_text(encoding='utf-8')
        state = board[board.index('static bool s_ptt_tx;'):board.index('/* ------------------------------------------------------------------ safety */')]
        safe = board[board.index('esp_err_t board_safe_state_init('):board.index('/* --------------------------------------------------------------------- LED */')]
        power = board[board.index('void board_sa818_power('):board.index('/* --------------------------------------------------------------------- I2C */')]
        (d / 'board_power.c').write_text('static bool s_amp_on;\n' + state + safe + power, encoding='utf-8')
        (d / 'test.c').write_text(HARNESS, encoding='utf-8')
        exe = d / 'test.exe'
        subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(d),
                        str(d / 'test.c'), str(d / 'sa818_freq.c'), str(d / 'ctcss.c'),
                        '-lm', '-o', str(exe)], check=True, env=env)
        for scenario in ('fresh', 'legacy', 'vhf'):
            subprocess.run([str(exe), scenario], check=True)

        driver = (MAIN / 'sa818.c').read_text(encoding='utf-8')
        group = driver[driver.index('esp_err_t sa818_set_group('):driver.index('esp_err_t sa818_set_volume(')]
        (d / 'driver_test.c').write_text(r'''
#include "stubs.h"
#include "sa818.h"
#include "sa818_freq.h"
static char sent[128];
esp_err_t sa818_cmd(const char *cmd, char *reply, size_t n, int timeout) {
    assert(timeout == 600); snprintf(sent, sizeof(sent), "%s", cmd);
    snprintf(reply, n, "+DMOSETGROUP:0"); return ESP_OK;
}
''' + group + r'''
int main(void) {
    sa818_profile_t p = {.tx_mhz=144.8000f,.rx_mhz=144.7525f,.squelch=3};
    strcpy(p.tx_ctcss,"0008"); strcpy(p.rx_ctcss,"0008");
    char reply[96];
    assert(sa818_set_group(&p,reply,sizeof(reply))==ESP_OK);
    assert(!strcmp(sent,"AT+DMOSETGROUP=0,144.8000,144.7525,0008,3,0008"));
    p.bandwidth=1;p.tx_mhz=438.5025f;p.rx_mhz=438.5125f;
    assert(sa818_set_group(&p,reply,sizeof(reply))==ESP_OK);
    assert(!strcmp(sent,"AT+DMOSETGROUP=1,438.5025,438.5125,0008,3,0008"));
    p.rx_mhz=438.5010f;p.tx_mhz=438.5050f;
    assert(sa818_set_group(&p,reply,sizeof(reply))==ESP_OK);
    assert(!strcmp(sent,"AT+DMOSETGROUP=1,438.5050,438.5010,0008,3,0008"));
    p.tx_mhz=438.5001f;strcpy(sent,"untouched");
    assert(sa818_set_group(&p,reply,sizeof(reply))==ESP_ERR_INVALID_ARG && !strcmp(sent,"untouched"));
    p.tx_mhz=438.50625f;
    assert(sa818_set_group(&p,reply,sizeof(reply))==ESP_ERR_INVALID_ARG);
    puts("PASS: actual UART serializer; exact 1/2.5/5k with four decimals, both bands/bandwidths; 6.25k/100Hz never sent");
}
''', encoding='utf-8')
        exe = d / 'driver_test.exe'
        subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-I',str(d),
                        str(d/'driver_test.c'),str(d/'sa818_freq.c'),'-lm','-o',str(exe)],check=True,env=env)
        subprocess.run([str(exe)],check=True)

        # Exercise the production serializer at the full bank size, including
        # worst-case names, a cross-band entry and non-finite damaged storage.
        channels = (MAIN / 'channels.c').read_text(encoding='utf-8')
        serializer = channels[channels.index('/* Bounded raw storage strings'):]
        (d / 'json_test.c').write_text(r'''
#include "sa818_cfg.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHANNEL_NAME_LEN 24
typedef struct { char name[CHANNEL_NAME_LEN]; sa818_cfg_t cfg; } channel_blob_t;
static channel_blob_t s_all[99];
static int s_count = 99;
void channels_init(void) {}
bool sa818_cfg_compatible(const sa818_cfg_t *c) {
    return sa818_freq_pair_valid(SA818_BAND_UHF, c->tx_mhz, c->rx_mhz);
}
''' + serializer + r'''
int main(void) {
    for (int i = 0; i < 99; ++i) {
        memset(s_all[i].name, 1, 23);
        sa818_cfg_t *c = &s_all[i].cfg;
        c->tx_mhz = c->rx_mhz = i % 2 ? 145.0125f : 438.5f;
        strcpy(c->tx_ctcss, "0000"); strcpy(c->rx_ctcss, "0000");
    }
    s_all[97].cfg.rx_mhz = 438.5f; // Deliberately mixed band.
    s_all[2].cfg.tx_mhz = 438.5025f; s_all[2].cfg.rx_mhz = 438.5010f;
    s_all[98].cfg.tx_mhz = NAN;
    memset(s_all[98].cfg.tx_ctcss, 2, 8); // Deliberately unterminated.
    char *out = malloc(99 * 768 + 4);
    out[99 * 768 + 3] = 42;
    assert(channels_list_json(out, 99 * 768 + 3) == 99);
    assert(out[99 * 768 + 3] == 42);
    puts(out);
    assert(channels_list_json(out, 64) == -1 && !strcmp(out, "[]"));
    free(out); return 0;
}
''', encoding='utf-8')
        exe = d / 'json_test.exe'
        subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(d),
                        str(d / 'json_test.c'), str(d / 'sa818_freq.c'), '-lm',
                        '-o', str(exe)], check=True, env=env)
        import json
        bank = json.loads(subprocess.check_output([str(exe)], text=True))
        assert len(bank) == 99 and [c['i'] for c in bank] == list(range(99))
        assert bank[0]['band'] == 'UHF' and bank[0]['compatible']
        assert bank[1]['band'] == 'VHF' and not bank[1]['compatible']
        assert bank[2]['tx'] == 438.5025 and bank[2]['rx'] == 438.5010 and bank[2]['compatible']
        assert bank[97]['band'] == 'UNSET' and not bank[97]['compatible']
        assert bank[98]['tx'] is None and not bank[98]['compatible']
        assert bank[98]['txt'] == '\x02' * 8
        print('PASS: all 99 channel entries, original numbering, JSON escaping, incompatible/corrupt entries, short buffer guard')


if __name__ == '__main__':
    main()
