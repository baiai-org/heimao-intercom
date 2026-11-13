#!/usr/bin/env python3
"""Compile production rx_guard.c AND ptt.c; no board, network or RF access."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
STUBS = r'''
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_FAIL 3
#define NVS_READONLY 0
#define NVS_READWRITE 1
static void test_log(const char *tag,const char *fmt,...){(void)tag;(void)fmt;}
#define ESP_LOGI(...) test_log(__VA_ARGS__)
#define ESP_LOGW(...) test_log(__VA_ARGS__)
#define ESP_LOGE(...) test_log(__VA_ARGS__)
#define portMAX_DELAY 0
#define portMUX_INITIALIZER_UNLOCKED 0
#define taskENTER_CRITICAL(p) ((void)(p))
#define taskEXIT_CRITICAL(p) ((void)(p))
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
#define pdMS_TO_TICKS(n) (n)
#define PIN_SA818_PTT 12
typedef int portMUX_TYPE;
typedef int StaticSemaphore_t;
typedef int *SemaphoreHandle_t;
typedef int nvs_handle_t;
static int64_t clock_us = 1000000;
static bool rf_pin;
static esp_err_t save_error;
static unsigned writes;
static unsigned char saved[64];
static size_t saved_size;
static int64_t esp_timer_get_time(void) {return clock_us;}
static SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *p){return p;}
static SemaphoreHandle_t xSemaphoreCreateRecursiveMutexStatic(StaticSemaphore_t *p){return p;}
static int xSemaphoreTake(SemaphoreHandle_t p,int t){(void)p;(void)t;return 1;}
static int xSemaphoreGive(SemaphoreHandle_t p){(void)p;return 1;}
static int xSemaphoreTakeRecursive(SemaphoreHandle_t p,int t){return xSemaphoreTake(p,t);}
static int xSemaphoreGiveRecursive(SemaphoreHandle_t p){return xSemaphoreGive(p);}
static void vTaskDelay(int ms){clock_us+=(int64_t)ms*1000;}
static void board_ptt_assert(bool on){rf_pin=on;}
static int gpio_get_level(int pin){(void)pin;return !rf_pin;}
static bool sa818_cfg_tx_ready(void){return true;}
static esp_err_t nvs_open(const char *ns,int mode,nvs_handle_t *h){(void)ns;(void)mode;*h=1;return ESP_OK;}
static esp_err_t nvs_get_blob(nvs_handle_t h,const char *key,void *out,size_t *n){
    (void)h;(void)key;if(!saved_size)return ESP_FAIL;assert(*n>=saved_size);memcpy(out,saved,saved_size);*n=saved_size;return ESP_OK;
}
static esp_err_t nvs_set_blob(nvs_handle_t h,const char *key,const void *data,size_t n){
    (void)h;(void)key;++writes;if(save_error)return save_error;assert(n<=sizeof(saved));memcpy(saved,data,n);saved_size=n;return ESP_OK;
}
static esp_err_t nvs_commit(nvs_handle_t h){(void)h;return save_error;}
static void nvs_close(nvs_handle_t h){(void)h;}
'''
HARNESS = r'''
#include "rx_guard.c"
#include "ptt.c"
static rx_guard_status_t status(void){rx_guard_status_t v;rx_guard_status(&v);return v;}
static void advance_ms(int ms){clock_us+=(int64_t)ms*1000;}
static void idle(void){ptt_request_rx();ptt_bridge_key(false);advance_ms(51);ptt_tick();}
static void network_over(void){assert(ptt_source_key(PTT_OWNER_NETWORK,1,true));assert(rf_pin);advance_ms(20);ptt_bridge_key(false);assert(!rf_pin);}
int main(void){
    rx_guard_init();ptt_init();rx_guard_context(1,true);
    assert(!status().p2p.enabled && status().p2p.delay_ms==2000);
    network_over();assert(rx_guard_blocked()==0); // opt-in default
    const char *bad[]={"", "1,0,7,7", "2,1,7,7", "1,60001,7,7", "1,1,0,7", "1,1,7,0", "1,1,8,7",
        "1,1,256,7", "1,1,7,256", "1,-1,7,7", "1,+1,7,7", "1,1.5,7,7", "1,1,7,7,0", "1,1,7,7x", "1,1,7", "1,1,7,", "1,9999999999999999999,7,7"};
    const unsigned before=writes;
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i)assert(rx_guard_configure(1,bad[i])==ESP_ERR_INVALID_ARG);
    assert(rx_guard_configure(0,"1,1,7,7")==ESP_ERR_INVALID_ARG && writes==before);
    assert(rx_guard_configure(1,"1,1234,7,7")==ESP_OK);
    network_over();const int64_t ended=clock_us;
    assert(status().remaining_ms==1234 && rx_guard_blocked()==7);
    ptt_bridge_key(false);advance_ms(1233);assert(status().remaining_ms==1); // repeated OFF never extends
    clock_us=ended+1234000;assert(rx_guard_blocked()==0); // exact expiry, no wait for COS
    assert(rx_guard_blocked_at(ended+1233999)==7); // a block begun during inhibit is discarded
    // A second physical TX interrupts the old timer and starts a fresh one.
    network_over();advance_ms(500);assert(ptt_source_key(PTT_OWNER_NETWORK,1,true));assert(rx_guard_blocked()==0);
    advance_ms(800);ptt_bridge_key(false);assert(status().remaining_ms==1234);
    // Current deadline/output mask are frozen, disabling is immediate.
    assert(rx_guard_configure(1,"1,1,1,1")==ESP_OK);assert(status().remaining_ms==1234 && status().blocked==7);
    assert(rx_guard_configure(1,"0,1,1,1")==ESP_OK);assert(rx_guard_blocked()==0);
    assert(rx_guard_configure(1,"1,1,1,1")==ESP_OK);assert(rx_guard_blocked()==0); // no retrospective arm
    network_over();assert(rx_guard_blocked()==1);clock_us+=999;assert(rx_guard_blocked()==1);clock_us++;assert(!rx_guard_blocked());
    // A failed persistent save never changes live configuration.
    save_error=ESP_FAIL;assert(rx_guard_configure(1,"1,5000,7,7")==ESP_FAIL);save_error=0;assert(status().p2p.delay_ms==1);
    // Separate modes and output masks survive reload. No timer is carried across contexts.
    assert(rx_guard_configure(2,"1,60000,2,6")==ESP_OK);rx_guard_init();assert(status().relay.delay_ms==60000);
    rx_guard_context(2,true);network_over();assert(!rx_guard_blocked()); // source filter
    assert(ptt_source_key(PTT_OWNER_SDR,11,true));advance_ms(500);ptt_release(PTT_OWNER_SDR,11);
    assert(status().remaining_ms==60000 && rx_guard_blocked()==6);
    rx_guard_context(1,true);assert(!rx_guard_blocked());network_over();assert(rx_guard_blocked()==1);
    rx_guard_context(1,false);assert(!rx_guard_blocked());network_over();assert(!rx_guard_blocked());
    rx_guard_context(0,true);network_over();assert(!rx_guard_blocked());
    rx_guard_context(1,true);assert(rx_guard_configure(1,"1,37,4,7")==ESP_OK);
    idle();ptt_set_rf_keying(false);assert(ptt_request_tx(100)==ESP_OK);advance_ms(30);ptt_tick();assert(!rf_pin);
    ptt_request_rx();assert(!rx_guard_blocked()); // logical TX must not arm
    idle();ptt_set_rf_keying(true);assert(ptt_request_tx(100)==ESP_OK);ptt_request_rx();assert(!rx_guard_blocked()); // cancelled PREPARE
    assert(ptt_request_tx(100)==ESP_OK);advance_ms(30);ptt_tick();assert(rf_pin);advance_ms(20);ptt_request_rx();
    assert(!rf_pin && status().remaining_ms==37); // actual TALK release
    idle();assert(ptt_request_tx(100)==ESP_OK);advance_ms(30);ptt_tick();advance_ms(100);ptt_tick();
    assert(!rf_pin && status().remaining_ms==37); // TX deadline release
    idle();assert(ptt_request_tx(1000)==ESP_OK);advance_ms(30);ptt_tick();advance_ms(251);ptt_tick();
    assert(!rf_pin && status().remaining_ms==37); // stalled PTT watchdog
    idle();assert(rx_guard_configure(1,"1,17,1,7")==ESP_OK);assert(ptt_source_key(PTT_OWNER_NETWORK,1,true));
    advance_ms(30000);ptt_tick();assert(!rf_pin && status().remaining_ms==17); // bridge watchdog
    idle();assert(ptt_claim(PTT_OWNER_SDR,50));ptt_release(PTT_OWNER_SDR,50);assert(!rx_guard_blocked()); // never keyed
    assert(rx_guard_configure(1,"1,2000,7,7")==ESP_OK);
    ptt_dry_result_t dry;
    assert(ptt_dry_run(200,&dry)==ESP_OK && dry.keyings==0 && !rf_pin && !rx_guard_blocked());
    ptt_trace_event_t events[64];uint32_t sequence;
    for(unsigned i=0;i<70;i++){advance_ms(1);ptt_trace_note(PTT_TRACE_COS_ON,1,i,200);}
    assert(ptt_trace_snapshot(events,64,&sequence)==64);
    for(unsigned i=0;i<64;i++){
        assert(events[i].a==i+6&&events[i].b==200&&events[i].kind==PTT_TRACE_COS_ON);
        if(i)assert(events[i].ms>=events[i-1].ms&&events[i].sequence==events[i-1].sequence+1);
    }
    assert(events[63].sequence==sequence);
    puts("PASS: bounded event history retains latest 64 records in chronological order");
    puts("PASS: actual PTT edges; default/off; 1ms/exact expiry; modes; masks; repeated OFF/TX; frozen config; strict parser; persistence; failures; logical/cancelled TX; watchdogs");
}
'''


def main():
    scratch = ROOT / ".tmp"
    scratch.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="rx-guard-", dir=scratch) as temp:
        directory = Path(temp)
        (directory / "freertos").mkdir()
        for name in ("rx_guard.c", "rx_guard.h", "ptt.c", "ptt.h"):
            shutil.copy2(ROOT / "firmware/main" / name, directory / name)
        (directory / "stubs.h").write_text(STUBS, encoding="utf-8")
        for name in ("esp_err.h", "esp_timer.h", "esp_log.h", "board_hw.h", "sa818_cfg.h", "nvs.h",
                     "freertos/FreeRTOS.h", "freertos/semphr.h", "freertos/task.h"):
            (directory / name).write_text('#include "stubs.h"\n', encoding="utf-8")
        (directory / "test.c").write_text(HARNESS, encoding="utf-8")
        exe = directory / "test.exe"
        env = dict(os.environ, TEMP=temp, TMP=temp, TMPDIR=temp)
        subprocess.run([shutil.which("gcc"), "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-variable",
                        "-I", temp, str(directory / "test.c"), "-o", str(exe)], check=True, env=env)
        subprocess.run([str(exe)], check=True, timeout=15)


if __name__ == "__main__":
    main()
