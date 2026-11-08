#!/usr/bin/env python3
"""Execute production PTT, voice lifecycle/TALK, SDR and HTTP control together.

Compile once per real P2P/relay bridge_key_update implementation. Only the
RTOS, devices, codecs and transports are simulated. Never contacts a board.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
MAIN = ROOT / 'firmware/main'


def function(source, name):
    start = re.search(r'^(?:static )?[\w *]+\b' + name + r'\([^;]*?\)\s*\{', source, re.M).start()
    brace = source.index('{', start)
    token = re.compile(r'/\*[\s\S]*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]')
    depth = 0
    for match in token.finditer(source, brace):
        if match[0] == '{':
            depth += 1
        elif match[0] == '}':
            depth -= 1
            if not depth:
                return source[start:match.end()] + '\n'
    raise AssertionError(name)


def no_includes(source):
    return re.sub(r'^#include[^\n]*\n|^#pragma once\n', '', source, flags=re.M)


STUBS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <inttypes.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_SIZE 2
#define ESP_ERR_NO_MEM 3
#define ESP_ERR_INVALID_STATE 4
#define ESP_ERR_TIMEOUT 5
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGE(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define AUDIO_SAMPLE_RATE_HZ 16000
#define AUDIO_PKT_SAMPLES 320
#define AUDIO_PKT_MS 20
#define MALLOC_CAP_SPIRAM 1
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY 1
#define pdMS_TO_TICKS(n) (n)
#define portMUX_INITIALIZER_UNLOCKED 0
typedef int portMUX_TYPE;
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))
#define taskENTER_CRITICAL(m) ((void)(m))
#define taskEXIT_CRITICAL(m) ((void)(m))
typedef struct {int depth; bool recursive;} StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
typedef void *TaskHandle_t;
typedef void *httpd_handle_t;
static SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *s) {s->recursive=false; return s;}
static SemaphoreHandle_t xSemaphoreCreateRecursiveMutexStatic(StaticSemaphore_t *s) {s->recursive=true; return s;}
static SemaphoreHandle_t xSemaphoreCreateMutex(void) {return calloc(1,sizeof(StaticSemaphore_t));}
static int xSemaphoreTake(SemaphoreHandle_t s, int timeout) {
    (void)timeout; assert(s && !s->depth); s->depth++; return pdTRUE;
}
static int xSemaphoreTakeRecursive(SemaphoreHandle_t s, int timeout) {
    (void)timeout; assert(s && s->recursive); s->depth++; return pdTRUE;
}
static void xSemaphoreGive(SemaphoreHandle_t s) {assert(s && s->depth==1); s->depth--;}
static void xSemaphoreGiveRecursive(SemaphoreHandle_t s) {assert(s && s->recursive && s->depth); s->depth--;}
static void *heap_caps_malloc(size_t n, int c) {(void)c; return malloc(n);}
static void heap_caps_free(void *p) {free(p);}
static uint32_t esp_random(void) {return 0xFA001234u;}
static int64_t clock_us=1000000;
static int64_t esp_timer_get_time(void) {return clock_us;}
static bool radio_key, radio_ready=true, amp_on;
#define PIN_SA818_PTT 41
static void board_ptt_assert(bool on) {radio_key=on;}
static int gpio_get_level(int pin) {(void)pin; return !radio_key;}
static bool sa818_cfg_tx_ready(void) {return radio_ready;}
static void board_amp_enable(bool on) {amp_on=on;}
static void vTaskDelay(int ticks);
static void vTaskDelete(void *p) {(void)p;}
static void vTaskDeleteWithCaps(void *p) {(void)p;}
static unsigned uxTaskGetStackHighWaterMark(void *p) {(void)p; return 4096;}
static int xTaskCreateWithCaps(void (*fn)(void *), const char *name, int stack,
    void *arg, int pri, TaskHandle_t *handle, int caps) {
    (void)fn;(void)name;(void)stack;(void)arg;(void)pri;(void)caps; *handle=(void*)1; return pdPASS;
}

#define HTTP_GET 1
#define HTTP_POST 2
#define HTTPD_400_BAD_REQUEST 400
#define HTTPD_WS_TYPE_TEXT 1
#define HTTPD_WS_TYPE_BINARY 2
#define HTTPD_WS_TYPE_CLOSE 8
#define HTTPD_WS_TYPE_PING 9
#define HTTPD_WS_TYPE_PONG 10
typedef struct {size_t len; uint8_t *payload; int type; bool final;} httpd_ws_frame_t;
typedef struct {
    int method, fd, type, error, chunk;
    httpd_handle_t handle;
    size_t content_len, offset;
    const uint8_t *body;
    char uri[256];
    void *sess_ctx;
    void (*free_ctx)(void *);
} httpd_req_t;
static uint8_t response[12000];
static size_t response_len;
static int response_type;
static unsigned ws_sends;
static bool response_ok;
static unsigned httpd_req_get_url_query_len(httpd_req_t *r) {
    const char *q=strchr(r->uri,'?'); return q ? (unsigned)strlen(q+1):0;
}
static esp_err_t httpd_req_get_url_query_str(httpd_req_t *r,char *out,size_t n) {
    const char *q=strchr(r->uri,'?'); if(!q || strlen(q+1)>=n) return ESP_FAIL;
    strcpy(out,q+1); return ESP_OK;
}
static esp_err_t httpd_query_key_value(const char *q,const char *key,char *out,size_t n) {
    while(*q) {
        const char *end=strchr(q,'&'); if(!end) end=q+strlen(q);
        const char *eq=memchr(q,'=',(size_t)(end-q));
        if(eq && (size_t)(eq-q)==strlen(key) && !memcmp(q,key,strlen(key))) {
            size_t len=(size_t)(end-eq-1); if(len>=n) return ESP_FAIL;
            memcpy(out,eq+1,len); out[len]=0; return ESP_OK;
        }
        q=*end ? end+1:end;
    }
    return ESP_FAIL;
}
static int httpd_req_to_sockfd(httpd_req_t *r) {return r->fd;}
static esp_err_t httpd_ws_recv_frame(httpd_req_t *r,httpd_ws_frame_t *f,size_t cap) {
    assert(!cap || r->content_len); // SDK would re-read the header for len == 0
    f->len=r->content_len;f->type=r->type;
    if(!cap) return ESP_OK;
    if(r->content_len>cap) return ESP_ERR_INVALID_SIZE;
    if(f->len) memcpy(f->payload,r->body,f->len);return ESP_OK;
}
static esp_err_t httpd_ws_send_frame(httpd_req_t *r,const httpd_ws_frame_t *f) {
    ++ws_sends;
    (void)r;assert(f->len<=sizeof(response));
    if(f->len)memcpy(response,f->payload,f->len);
    response_len=f->len;response_type=f->type;return ESP_OK;
}
static int httpd_req_recv(httpd_req_t *r,char *out,size_t want) {
    if(r->error && r->offset) return -1;
    size_t n=(++r->chunk%2) ? 3:511;
    if(n>want)n=want;memcpy(out,r->body+r->offset,n);r->offset+=n;return (int)n;
}
static void httpd_resp_set_hdr(httpd_req_t *r,const char *k,const char *v) {(void)r;(void)k;(void)v;}
static void httpd_resp_set_type(httpd_req_t *r,const char *t) {(void)r;(void)t;}
static esp_err_t httpd_resp_send(httpd_req_t *r,const char *data,size_t n) {
    (void)r;assert(n<sizeof(response));if(n)memcpy(response,data,n);response[n]=0;response_len=n;return ESP_OK;
}
static void httpd_resp_send_err(httpd_req_t *r,int status,const char *msg) {(void)r;(void)status;(void)msg;}
static esp_err_t send_json(httpd_req_t *r,const char *s) {return httpd_resp_send(r,s,strlen(s));}
static esp_err_t send_result(httpd_req_t *r,bool ok,const char *e) {(void)e;response_ok=ok;return send_json(r,ok?"{\"ok\":true}":"{\"ok\":false}");}
static const char *esp_err_to_name(int e) {(void)e;return "error";}
typedef struct {void(*callback)(void*);void *arg;const char *name;} esp_timer_create_args_t;
typedef esp_timer_create_args_t *esp_timer_handle_t;
static esp_err_t esp_timer_create(const esp_timer_create_args_t *args,esp_timer_handle_t *h) {*h=malloc(sizeof(**h));**h=*args;return ESP_OK;}
static int esp_timer_start_once(esp_timer_handle_t h,uint64_t us) {(void)h;(void)us;return ESP_OK;}
static int esp_timer_stop(esp_timer_handle_t h) {(void)h;return ESP_OK;}
static int esp_timer_delete(esp_timer_handle_t h) {free(h);return ESP_OK;}
'''

VOICE_ENV = r'''
static const char *voice_TAG="voice";
static voice_mode_t v_mode=VOICE_OFF;
static bool v_stop,s_task_running,s_adc_error,s_release_requested,s_dac_talk,s_rf_also;
static bool s_instant_start,s_bias_have,s_mod_sq_open,s_bridge_inj_amp,s_inject_silent;
static bool s_inject_tx_processed,s_last_bridge_tx_processed;
static bool s_sdr_tail_active,s_sdr_tail_block_pending,s_sdr_net_tx;
static int s_sdr_tail_id,s_sdr_tail_config;
static size_t s_sdr_tail_pos;
static uint32_t s_sdr_tail_token,s_sdr_tail_done_token;
static uint32_t s_inject_token;
static ptt_owner_t s_inject_owner;
static bool s_inj_primed,s_borrow_preview,s_borrow_bridge,s_borrow_sdr,s_borrowed,s_cap_on;
static TaskHandle_t s_task;
static int s_gain=80,s_tx_gain=48,s_net_tx_gain=96,s_bgtx=16,s_tx_log_blocks;
static int s_dc_x1,s_dc_y1,s_amp_settle,s_sq_hold,s_rx_fade_left,s_mod_sq_on_run,s_mod_sq_off_run,s_sq_quiet_ms,s_sq_gpio_run;
static uint32_t s_gate_guard_ms,s_bridge_starve,s_bridge_blocks,s_bridge_trim;
static int s_eq_spk,s_eq_mic;
static int s_rx_band;
static struct {unsigned samples,blocks,underruns;int sample_rate,gain,peak_in;} s_stats;
static int mode=1;
static bool net_ready=true,rf_also,bridge_on;
static unsigned net_samples,net_ends,monitor_creates,talk_creates,inject_races;
static bool queue_network_during_stop;
#define NETCOM_MODE_LOCAL 0
#define GATE_GUARD_MS 500
#define RX_FADE_BLOCKS 20
#define BLOCK_FRAMES 256
#define TAIL_OFF 0
#define TAIL_COUNT 9
static int netcom_mode(void) {return mode;}
static bool netcom_tx_ready(void) {return net_ready;}
static const char *netcom_tx_ready_hint(void) {return "not linked";}
static bool netcom_rf_tx(void) {return rf_also;}
static void netcom_tx_sample(const int16_t *p,size_t n,bool on) {(void)p;if(on)net_samples+=(unsigned)n;}
static void netcom_tx_end(void) {net_ends++;}
static bool bridge_active(void) {return bridge_on;}
static void bgtx_load(void) {}
static void sqgpio_load(void) {}
static bool voice_bufs_alloc(void) {return true;}
static esp_err_t audio_codec_init(void) {return ESP_OK;}
static esp_err_t audio_i2s_start_clock_only(void) {return ESP_OK;}
static bool audio_i2s_clock_running(void) {return true;}
static bool speaker_diag_output_active(void) {return false;}
static esp_err_t speaker_diag_command(const char *op,unsigned ms) {(void)op;(void)ms;return ESP_OK;}
static void eq6_reset_state(void *p) {(void)p;}
static void rx_band_reset(void *p) {(void)p;}
static void eq6_run(void *p,int16_t *b,size_t n) {(void)p;(void)b;(void)n;}
static bool cap_ensure(void) {return true;}
static void monitor_task(void *arg) {(void)arg;}
static void talk_task(void *arg);
static int xTaskCreatePinnedToCore(void (*fn)(void*),const char *name,int stack,void *arg,int pri,TaskHandle_t *h,int core) {
    (void)name;(void)stack;(void)arg;(void)pri;(void)core;
    if(fn==monitor_task)monitor_creates++;else {assert(fn==talk_task);talk_creates++;}
    *h=(void*)1;return pdPASS;
}
/* r11-33 moved the TALK task to a static stack (no heap allocation at PTT). */
#define TALK_STACK_BYTES 6144
typedef uint32_t StackType_t;
typedef struct { uint32_t dummy; } StaticTask_t;
static StackType_t s_talk_stack[TALK_STACK_BYTES / sizeof(StackType_t)];
static StaticTask_t s_talk_tcb;
static TaskHandle_t xTaskCreateStaticPinnedToCore(void (*fn)(void*),const char *name,uint32_t stack,void *arg,int pri,StackType_t *buf,StaticTask_t *tcb,int core) {
    (void)name;(void)stack;(void)arg;(void)pri;(void)buf;(void)tcb;(void)core;
    if(fn==monitor_task)monitor_creates++;else {assert(fn==talk_task);talk_creates++;}
    return (TaskHandle_t)1;
}
static esp_err_t audio_i2s_read(int16_t *out,size_t n,size_t *got) {for(size_t i=0;i<n;i++)out[i]=1234;*got=n;return ESP_OK;}
static esp_err_t audio_i2s_write(const int16_t *out,size_t n) {(void)out;(void)n;return ESP_OK;}
static int clamp16(int n) {return n < -32768 ? -32768 : n>32767 ? 32767:n;}
static int tail_get(void) {return TAIL_OFF;}
static bool tail_should_play(int mode) {(void)mode;return false;}
static size_t tail_samples(int id) {(void)id;return 0;}
static const char *tail_name(int id) {(void)id;return "off";}
static size_t tail_fetch(int id,size_t pos,int16_t *out,size_t n) {(void)id;(void)pos;(void)out;(void)n;return 0;}
'''

HARNESS = r'''
static void vTaskDelay(int ticks) {
    clock_us+=(int64_t)ticks*1000;
    ptt_tick();
    if(!s_on) { s_tx_task=NULL; }
    if(v_stop && s_task_running) {
        if(queue_network_during_stop) {
            // Simulate the other core trying to take TX after admission but
            // before MONITOR has acknowledged exit. Reservation must win.
            assert(!ptt_source_key(PTT_OWNER_NETWORK,1,true)); inject_races++;
        }
        s_task_running=false;
    }
}
static void step(int ms) {clock_us+=(int64_t)ms*1000;ptt_tick();}
static httpd_req_t request(const char *uri) {httpd_req_t r={0};strcpy(r.uri,uri);return r;}
static uint32_t start_browser(uint32_t sid) {
    uint32_t tx=s_tx_id+1;assert(sdr_ptt_request(sid,tx,true));
    assert(radio_key && ptt_owned(PTT_OWNER_SDR,tx) && s_inject_silent);return tx;
}
int main(void) {
    assert(ptt_init()==ESP_OK);
    s_sdr_buf_mux=xSemaphoreCreateMutex(); // same boot allocation as net_http_start
    assert(voice_start(VOICE_MONITOR)==ESP_OK);
    sdr_init();assert(sdr_client_open(99));
    // Exercise real PTT admission while local diagnostics reserve the shared
    // DAC. No test reaches a device: board/RTOS/transport above are simulated.
    assert(ptt_local_audio_begin() && ptt_local_audio_active());
    assert(!ptt_local_audio_begin());
    assert(!ptt_claim(PTT_OWNER_TALK,123));
    assert(!ptt_claim(PTT_OWNER_SDR,123));
    assert(!ptt_claim(PTT_OWNER_NETWORK,1));
    assert(ptt_request_tx(500)==ESP_ERR_INVALID_STATE);
    assert(!ptt_source_key(PTT_OWNER_SDR,123,true));
    ptt_dry_result_t blocked_dry;
    assert(ptt_dry_run(200,&blocked_dry)==ESP_ERR_INVALID_STATE);
    assert(voice_start(VOICE_TALK)==ESP_ERR_INVALID_STATE);
    assert(!sdr_ptt_request(s_session,1,true));
    bridge_on=true;s_rx_ptt=true;s_rx_ptt_ms=now_ms();bridge_key_update(true);
    int16_t diagnostic_input[320]={1};
    voice_bridge_inject(diagnostic_input,320);
    assert(voice_bridge_level()==0 && !radio_key && ptt_tx_count()==0);
    s_rx_ptt=false;s_last_play_ms=0;bridge_on=false;bridge_key_update(false);
    ptt_local_audio_end();assert(!ptt_local_audio_active());
    rf_also=true; // current browser routing obeys the operator RF-also setting
    uint32_t sid=s_session, tx=start_browser(sid);
    int16_t input[8192],out[6000];uint8_t wire[8192];
    for(int i=0;i<8192;i++)input[i]=1234;
    for(size_t i=0;i<sizeof(wire);i++)wire[i]=(uint8_t)(i*71+13);
    bridge_on=true;
    // Actual network engine idle callbacks must never retract browser PTT.
    bridge_key_update(false);assert(radio_key && s_ptt_hold);
    bridge_on=false;bridge_key_update(false);assert(radio_key);
    voice_bridge_inject(input,320);assert(voice_bridge_level()==0);
    assert(sdr_tx_push_bytes(sid,tx,wire,1280));sdr_tx_step();
    assert(voice_bridge_level()==320 && s_tx_samples==320);
    unsigned monitors=monitor_creates;
    httpd_req_t req=request("/api/talk?on=1&confirm=1&secs=5");
    response_ok=true;h_talk(&req);assert(!response_ok && v_mode==VOICE_MONITOR);
    assert(monitor_creates==monitors && radio_key && s_task_running);
    assert(voice_stop()==ESP_ERR_INVALID_STATE);
    voice_monitor_borrow("preview");voice_monitor_release("preview");assert(radio_key && v_mode==VOICE_MONITOR);
    // HTTP odd fragments preserve every sample, including samples split at TCP boundaries.
    (void)sdr_ptt_request(sid,tx,false);tx=start_browser(sid);
    char url[256];snprintf(url,sizeof(url),"/api/sdr_tx?sid=%"PRIu32"&tx=%"PRIu32,sid,tx);
    req=request(url);req.content_len=sizeof(wire);req.body=wire;
    assert(h_sdr_tx(&req)==ESP_OK && req.offset==sizeof(wire));
    assert(ring_take(s_tx_ring,SDR_TX_RING_SAMPLES,&s_tx_head,&s_tx_tail,out,6000)==4096);
    for(int i=0;i<4096;i++)assert((uint16_t)out[i]==(uint16_t)(wire[2*i]|wire[2*i+1]<<8));
    req=request(url);req.content_len=3;req.body=wire;assert(h_sdr_tx(&req)==ESP_FAIL && req.offset==0);
    req.content_len=8194;assert(h_sdr_tx(&req)==ESP_FAIL && req.offset==0);
    req.content_len=1280;req.error=1;assert(h_sdr_tx(&req)==ESP_FAIL);
    (void)sdr_ptt_request(sid,tx,false);assert(!radio_key && voice_bridge_level()==0);
    assert(!sdr_tx_push_bytes(sid,tx,wire,1280));
    assert(!sdr_ptt_request(sid,tx,true)); // cannot resurrect completed TX
    assert(sdr_ptt_request(sid,tx+1,false)); // OFF arriving before ON
    assert(!sdr_ptt_request(sid,tx+1,true));
    uint32_t previous=tx;tx=start_browser(sid);
    assert(!sdr_ptt_request(sid,previous,false));assert(radio_key);
    assert(!sdr_tx_push_bytes(sid,previous,wire,1280));assert(s_tx_head==s_tx_tail);
    // Re-prime uses actual interpolation, which can leave one unread sample.
    voice_sdr_inject(tx,input,4800);assert(voice_bridge_play(out,256)==256);
    int loops=0;while(s_inj_primed && loops++<100)voice_bridge_play(out,256);
    assert(loops<100 && voice_bridge_level()<=1);
    voice_sdr_inject(tx,input,320);assert(voice_bridge_play(out,256)==0);
    voice_sdr_inject(tx,input,4800);assert(voice_bridge_play(out,256)==256);
    // Watchdogs must still run when the injection queue is full. Zero PCM is
    // valid activity; only arrival time and the absolute hold duration matter.
    voice_inject_reset();voice_sdr_inject(tx,input,8000);
    memset(wire,0,1280);assert(sdr_tx_push_bytes(sid,tx,wire,1280));
    clock_us+=1900000;sdr_tx_step();assert(radio_key);
    clock_us+=100001;sdr_tx_step();assert(!radio_key && !s_ptt_hold && voice_bridge_level()==0);
    char state[640];sdr_state_json(state,sizeof(state),true,7);
    assert(strstr(state,"pcm_timeout") && strstr(state,"\"ptt\":false"));
    tx=start_browser(sid);clock_us+=29900000;
    assert(sdr_tx_push_bytes(sid,tx,wire,1280));clock_us+=100001;sdr_tx_step();
    assert(!radio_key && !strcmp(s_end_reason,"hold_limit"));
    // A stale page/session cannot close, key, inject or consume the new page.
    assert(sdr_client_close(sid));assert(!sdr_client_open(0));assert(sdr_client_open(100));
    uint32_t stale=sid;sid=s_session;assert(sid!=stale);
    assert(!sdr_client_close(stale));assert(!sdr_ptt_request(stale,500,true));
    assert(!sdr_tx_push_bytes(stale,500,wire,1280));assert(!sdr_client_open(101));
    for(int i=0;i<8192;i++)input[i]=(int16_t)(i*17-25000);
    // A module squelch close drops queued browser RX noise. Re-opening admits
    // fresh radio audio again without changing the transport or PTT state.
    sdr_rx_push(input,640,true);assert(s_rx_head-s_rx_tail==640);
    sdr_rx_push(input,320,false);assert(s_rx_head==s_rx_tail);
    sdr_rx_push(input,5120,true);snprintf(url,sizeof(url),"/api/sdr_rx?sid=%"PRIu32,sid);req=request(url);
    assert(h_sdr_rx(&req)==ESP_OK && response_len==10240);
    for(int i=0;i<5120;i++){assert(response[2*i]==((uint16_t)input[i]&255));assert(response[2*i+1]==((uint16_t)input[i]>>8));}
    // ESP-IDF clears the URI/method after handshake. Preserve only sess_ctx;
    // using query_sid again here would reject every subsequent WS frame.
    snprintf(url,sizeof(url),"/ws/sdr?sid=%"PRIu32,sid);req=request(url);req.method=HTTP_GET;req.fd=7;req.handle=(void*)1;
    assert(h_ws_sdr(&req)==ESP_OK && req.sess_ctx);
    req.uri[0]=0;req.method=0;req.type=HTTPD_WS_TYPE_TEXT;
    char text[128];snprintf(text,sizeof(text),"ptt1:%"PRIu32":1:42",sid);
    req.body=(uint8_t*)text;req.content_len=strlen(text);assert(h_ws_sdr(&req)==ESP_OK && radio_key);
    assert(strstr((char*)response,"\"id\":42") && strstr((char*)response,"\"ptt\":true"));
    uint8_t frame[1288];memcpy(frame,&sid,4);uint32_t one=1;memcpy(frame+4,&one,4);memcpy(frame+8,wire,1280);
    req.type=HTTPD_WS_TYPE_BINARY;req.body=frame;req.content_len=sizeof(frame);
    assert(h_ws_sdr(&req)==ESP_OK && s_tx_head-s_tx_tail==640);
    req.type=HTTPD_WS_TYPE_PING;req.content_len=0;
    assert(h_ws_sdr(&req)==ESP_OK && response_type==HTTPD_WS_TYPE_PONG && !response_len && radio_key);
    // Socket destruction (TCP error/LRU purge) releases only this connection.
    void *oldctx=req.sess_ctx;req.free_ctx(oldctx);req.sess_ctx=NULL;assert(!radio_key);
    snprintf(url,sizeof(url),"/ws/sdr?sid=%"PRIu32,sid);req=request(url);req.method=HTTP_GET;req.fd=8;req.handle=(void*)1;
    assert(h_ws_sdr(&req)==ESP_OK);tx=start_browser(sid);
    assert(!sdr_ws_owner((void*)2,8,sid));sdr_ws_detach((void*)1,7,sid);assert(radio_key);
    req.uri[0]=0;req.method=0;req.type=HTTPD_WS_TYPE_BINARY;req.content_len=4096;
    assert(h_ws_sdr(&req)==ESP_ERR_INVALID_SIZE && !radio_key);req.free_ctx(req.sess_ctx);
    // New App opts into activity without altering legacy audio/ACK frames.
    snprintf(url,sizeof(url),"/ws/sdr?sid=%"PRIu32"&activity=1",sid);req=request(url);
    req.method=HTTP_GET;req.fd=9;req.handle=(void*)1;
    assert(h_ws_sdr(&req)==ESP_OK && ((sdr_ws_context_t*)req.sess_ctx)->activity);
    uint8_t heartbeat=0;req.uri[0]=0;req.method=0;req.type=HTTPD_WS_TYPE_BINARY;
    req.body=&heartbeat;req.content_len=1;
    unsigned sent=ws_sends;
    assert(h_ws_sdr(&req)==ESP_OK && ws_sends==sent+1 && response_type==HTTPD_WS_TYPE_TEXT);
    assert(strstr((char*)response,"\"type\":\"activity\"") && !radio_key);
    clock_us+=50000;assert(h_ws_sdr(&req)==ESP_OK && ws_sends==sent+1);
    clock_us+=1000000;assert(h_ws_sdr(&req)==ESP_OK && ws_sends==sent+2);
    req.type=HTTPD_WS_TYPE_TEXT;snprintf(text,sizeof(text),"ptt1:%"PRIu32":%"PRIu32":43",sid,s_tx_id+1);
    req.body=(uint8_t*)text;req.content_len=strlen(text);
    assert(h_ws_sdr(&req)==ESP_OK && radio_key && strstr((char*)response,"\"activity\":{") && strstr((char*)response,"\"id\":43"));
    snprintf(text,sizeof(text),"ptt0:%"PRIu32":%"PRIu32":44",sid,s_tx_id);req.content_len=strlen(text);
    assert(h_ws_sdr(&req)==ESP_OK && !radio_key && strstr((char*)response,"\"id\":44"));
    req.free_ctx(req.sess_ctx);
    // TALK resumes normally after browser release; a network task cannot claim
    // while MONITOR is stopping. The actual TALK task still emits network PCM
    // and an end marker, and then restores MONITOR for the SDR borrower.
    rf_also=false;net_samples=0;net_ends=0;
    queue_network_during_stop=true;assert(voice_start(VOICE_TALK)==ESP_OK);queue_network_during_stop=false;
    assert(inject_races && talk_creates==1 && v_mode==VOICE_TALK && !radio_key);
    uint32_t talk=voice_talk_token();assert(talk && ptt_owned(PTT_OWNER_TALK,talk));
    assert(!sdr_ptt_request(sid,tx+1,true));assert(v_mode==VOICE_TALK);
    voice_monitor_borrow("bridge");assert(v_mode==VOICE_TALK);
    assert(!voice_request_talk_end_for(talk+1));
    assert(voice_request_talk_end_for(talk));talk_task((void*)(uintptr_t)talk);
    assert(net_samples==BLOCK_FRAMES && net_ends==1 && v_mode==VOICE_MONITOR);
    assert(!voice_request_talk_end_for(talk));step(60);
    assert(sdr_client_close(sid));
    // Real carrier playback + silence: amplitude never controls PTT. Only the
    // network owner is released by its idle/hang callback; data remains listenable.
    bridge_on=true;s_rx_ptt=true;s_rx_ptt_ms=now_ms();bridge_key_update(false);
    assert(radio_key && ptt_owned(PTT_OWNER_NETWORK,1));
    voice_bridge_inject(input,320);assert(voice_bridge_level()==320);
    assert(voice_start(VOICE_TALK)==ESP_ERR_INVALID_STATE);
    s_rx_ptt=false;s_last_play_ms=0;bridge_key_update(false);assert(!radio_key);
    voice_inject_reset();voice_bridge_inject(input,4800);
    for(int i=0;i<100;i++)voice_bridge_play(out,256);
    assert(voice_bridge_level()==1 && s_inj_primed); // retain r8 network re-prime rule
    voice_inject_reset();bridge_on=false;
    // Actual web TALK (including RF-also), tokened release and captured timer
    // arguments: a prior transmission's callback cannot end a replacement.
    rf_also=true;req=request("/api/talk?on=1&confirm=1&secs=5");
    assert(h_talk(&req)==ESP_OK && radio_key);
    uint32_t oldtalk=voice_talk_token();assert(oldtalk && s_talk_timer);
    talk_timeout_cb((void*)(uintptr_t)talk);assert(!s_release_requested);
    talk_timeout_cb(s_talk_timer->arg);assert(s_release_requested);
    talk_task((void*)(uintptr_t)oldtalk);step(60);assert(!radio_key);
    req=request("/api/talk?on=1&confirm=1&secs=5");assert(h_talk(&req)==ESP_OK && radio_key);
    talk=voice_talk_token();assert(talk!=oldtalk);
    talk_timeout_cb((void*)(uintptr_t)oldtalk);assert(!s_release_requested);
    snprintf(url,sizeof(url),"/api/talk?on=0&tx=%"PRIu32,oldtalk);req=request(url);
    response_ok=true;h_talk(&req);assert(!response_ok && !s_release_requested && radio_key);
    snprintf(url,sizeof(url),"/api/talk?on=0&tx=%"PRIu32,talk);req=request(url);
    h_talk(&req);assert(response_ok && s_release_requested);talk_task((void*)(uintptr_t)talk);step(60);
    ptt_dry_result_t dry;assert(ptt_dry_run(200,&dry)==ESP_OK && dry.ended_idle && !dry.keyings);
    assert(ptt_owner()==PTT_OWNER_NONE && !radio_key);
    printf("PASS %s: actual PTT + voice/TALK + browser + HTTP/WS + network key control, stale IDs, PCM/watchdogs, DAC admission and end marker\n",MODE_NAME);
    free(s_rx_ring);free(s_tx_ring);if(s_talk_timer)esp_timer_delete(s_talk_timer);
    return 0;
}
'''


def build_source(engine):
    ptt = (MAIN / 'ptt.c').read_text(encoding='utf-8').replace('TAG', 'ptt_TAG')
    voice = (MAIN / 'voice.c').read_text(encoding='utf-8')
    assert re.search(
        r'sdr_rx_push\(out, \(size_t\)m,\s*'
        r's_mod_sq_open\s*&&\s*!previewing\s*&&\s*!s_bridge_inj_amp',
        voice,
    ), 'browser RX must follow module squelch'
    voice = re.sub(r'\bs_control\b', 'v_control', voice)
    voice = re.sub(r'\bs_control_storage\b', 'v_control_storage', voice)
    voice = re.sub(r'\bs_control_init\b', 'v_control_init', voice)
    voice = re.sub(r'\bs_control_generation\b', 'v_generation', voice)
    voice = re.sub(r'\bs_mode\b', 'v_mode', voice)
    voice = re.sub(r'\bs_stop\b', 'v_stop', voice).replace('TAG', 'voice_TAG')
    controls = voice[voice.index('static StaticSemaphore_t v_control_storage'):voice.index('/*\n * Talk release requested')]
    ring = voice[voice.index('#define BRIDGE_INJ_SAMPLES'):voice.index('/*\n * SDR: the web-audio TX')]
    funcs = ['voice_bridge_set_silent', 'voice_sdr_begin', 'voice_talk_token', 'voice_request_talk_end_for',
             'voice_request_talk_end', 'voice_stop_locked', 'voice_start_locked', 'voice_start', 'voice_stop',
             'voice_monitor_instant', 'monitor_borrow_locked', 'monitor_release_locked',
             'voice_monitor_borrow', 'voice_monitor_release', 'voice_mode', 'talk_task']
    vcode = controls + function(voice, 'gain_mul') + function(voice, 'tx_gain_apply')
    vcode += function(voice, 'voice_apply_tx_processing') + function(voice, 'voice_apply_net_tx_processing')
    vcode += ring + ''.join(function(voice, name) for name in funcs)
    sdr = (MAIN / 'sdr.c').read_text(encoding='utf-8').replace('TAG', 'sdr_TAG')
    http = (MAIN / 'net_http.c').read_text(encoding='utf-8')
    hcode = 'static const char *TAG="http";\n'
    hcode += 'static uint32_t app_control_generation(void){return 1;}\n'
    hcode += '#define RADIO_ACTIVITY_JSON_SIZE 512\n'
    hcode += 'static uint32_t radio_activity_json(char *b,size_t n){snprintf(b,n,"{\\\"type\\\":\\\"activity\\\",\\\"tx_active\\\":%s}",radio_key?"true":"false");return radio_key;}\n'
    hcode += '#define WEB_TALK_DEF_S 30\n#define WEB_TALK_MAX_S 60\n'
    hcode += 'static esp_timer_handle_t s_talk_timer; static uint32_t s_web_talk_token;\n'
    hcode += ''.join(function(http, n) for n in ['query_int','query_str','sdr_query_id','sdr_reply_json','sdr_reply','h_sdr','h_sdr_state','talk_timeout_cb','h_talk'])
    hcode += http[http.index('#define SDR_WS_PKT'):http.index('static esp_err_t h_ws_sdr')]
    hcode += function(http,'h_ws_sdr')+function(http,'h_sdr_ptt')+function(http,'h_sdr_tx')
    hcode += '#define SDR_HTTP_RX_PKTS 16\n'+function(http,'h_sdr_rx')
    net = (MAIN / engine).read_text(encoding='utf-8')
    prefix = 'NP' if engine=='netp2pnet.c' else 'REL'
    network = f'#define MODE_NAME "{prefix}"\n#define {prefix}_RX_PTT_TIMEOUT_MS 500\n#define {prefix}_RX_HANG_MS 250\n'
    network += 'static bool s_rx_ptt;static int64_t s_rx_ptt_ms,s_last_play_ms;\n'
    network += 'static int64_t now_ms(void){return clock_us/1000;}\nstatic void peer_lock(void){}\nstatic void peer_unlock(void){}\n'
    network += 'static void netcom_note_rx_ptt(bool a,bool b){(void)a;(void)b;}\nstatic void netcom_note_rx_carrier(bool a){(void)a;}\n'
    if prefix=='REL':
        network += 'static void rx_lock(void){}\nstatic void rx_unlock(void){}\n'
    network += function(net,'bridge_key_update')
    return (STUBS + no_includes((MAIN/'ptt.h').read_text(encoding='utf-8')) +
            no_includes((MAIN/'voice.h').read_text(encoding='utf-8')) +
            no_includes((MAIN/'sdr.h').read_text(encoding='utf-8')) +
            no_includes(ptt) + VOICE_ENV + vcode + no_includes(sdr) + hcode + network + HARNESS)


def main():
    env = os.environ.copy()
    env['PATH'] = r'C:\Program Files\mingw64\bin;' + env['PATH']
    gcc = shutil.which('gcc', path=env['PATH'])
    if not gcc:
        raise SystemExit('Host GCC required')
    scratch=ROOT/'.tmp';scratch.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='tx-owner-',dir=scratch) as directory:
        env.update(TEMP=directory,TMP=directory,TMPDIR=directory)
        for engine in ['netp2pnet.c','netrelay.c']:
            source=Path(directory)/'test.c';source.write_text(build_source(engine),encoding='utf-8')
            exe=Path(directory)/'test.exe'
            subprocess.run([gcc,'-std=c11','-Wall','-Wextra','-Werror',
                '-Wno-unused-function','-Wno-unused-variable','-Wno-unused-parameter','-Wno-misleading-indentation',
                str(source),'-lm','-o',str(exe)],check=True,env=env)
            subprocess.run([str(exe)],check=True,env=env,timeout=30)


if __name__=='__main__':
    main()
