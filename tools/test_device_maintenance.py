"""Production maintenance policy, durable peer labels and service lifecycle, on host."""
from pathlib import Path
import os, shutil, subprocess, tempfile
from test_tx_ownership import function
ROOT=Path(__file__).resolve().parents[1];MAIN=ROOT/'firmware/main'
def plain(name):
    return '\n'+'\n'.join(line for line in (MAIN/name).read_text(encoding='utf-8').splitlines() if not line.startswith('#include') and line!='#pragma once')+'\n'
BASE=r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 3
#define ESP_ERR_INVALID_STATE 4
#define ESP_ERR_NO_MEM 5
#define ESP_ERR_NVS_NOT_FOUND 6
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
#define portMAX_DELAY 0
typedef int esp_err_t,nvs_handle_t,StaticSemaphore_t,SemaphoreHandle_t;
static bool fail_open,fail_set,fail_commit,fail_alloc;static int commits,writes;
static unsigned char durable[8192],pending[8192];static size_t durable_n,pending_n;
static int xSemaphoreCreateMutexStatic(void*p){(void)p;return 1;}
static int xSemaphoreTake(int p,int t){(void)t;assert(p);return 1;}
static void xSemaphoreGive(int p){assert(p);}
static void *heap_caps_malloc(size_t n,int caps){(void)caps;return fail_alloc?NULL:malloc(n);}
static void *heap_caps_calloc(size_t n,size_t size,int caps){(void)caps;return fail_alloc?NULL:calloc(n,size);}
static void heap_caps_free(void*p){free(p);}
static unsigned heap_caps_get_free_size(int caps){return caps==MALLOC_CAP_INTERNAL?40000:4000000;}
static unsigned heap_caps_get_largest_free_block(int caps){(void)caps;return 16000;}
static int nvs_open(const char*ns,int mode,int*h){(void)ns;if(fail_open)return 50;if(!durable_n&&mode==0)return ESP_ERR_NVS_NOT_FOUND;*h=1;return 0;}
static int nvs_get_blob(int h,const char*k,void*out,size_t*n){(void)h;(void)k;if(!durable_n)return ESP_ERR_NVS_NOT_FOUND;assert(*n>=durable_n);memcpy(out,durable,durable_n);*n=durable_n;return 0;}
static int nvs_set_blob(int h,const char*k,const void*data,size_t n){(void)h;(void)k;if(fail_set)return 51;assert(n<=sizeof(pending));memcpy(pending,data,n);pending_n=n;++writes;return 0;}
static int nvs_commit(int h){(void)h;if(fail_commit)return 52;memcpy(durable,pending,pending_n);durable_n=pending_n;++commits;return 0;}
static void nvs_close(int h){(void)h;}
'''
POLICY_TEST=r'''
#define MONDAY_UTC 1795363200LL /* Monday 00:00 in Beijing. */
int main(void) {
    maintenance_config_t c = {.version=2, .cleanup_enabled=1, .restart_minute=240};
    maintenance_policy_t p;
    assert(maintenance_config_valid(&c));
    c.restart_enabled = 1;
    assert(!maintenance_config_valid(&c));
    c.restart_days = 0x80;
    assert(!maintenance_config_valid(&c));
    c.restart_days = 1;
    c.restart_minute = 1440;
    assert(!maintenance_config_valid(&c));
    c.restart_minute = 0;
    assert(maintenance_config_valid(&c));

    maintenance_policy_reset(&p, 0);
    assert(!maintenance_policy_cleanup_due(&p, &c, 0, false));
    assert(!maintenance_policy_cleanup_due(&p, &c, 300000, true));
    assert(!maintenance_policy_cleanup_due(&p, &c, 300001, false));
    assert(!maintenance_policy_cleanup_due(&p, &c, 360000, false));
    assert(maintenance_policy_cleanup_due(&p, &c, 360001, false));

    assert(maintenance_next_restart(&c, MONDAY_UTC - 1, 0) == MONDAY_UTC);
    assert(maintenance_next_restart(&c, MONDAY_UTC, 0) == MONDAY_UTC + 7 * 86400);
    assert(maintenance_restart_due(&c, MONDAY_UTC - 1, MONDAY_UTC, 1000, 0));
    assert(!maintenance_restart_due(&c, 0, MONDAY_UTC, 1000, 0));
    assert(!maintenance_restart_due(&c, MONDAY_UTC - 1, MONDAY_UTC + 600, 1000, 0));
    assert(!maintenance_restart_due(&c, MONDAY_UTC - 600, MONDAY_UTC, 1000, 0));
    assert(!maintenance_restart_due(&c, MONDAY_UTC + 1, MONDAY_UTC, 1000, 0));
    assert(!maintenance_restart_due(&c, MONDAY_UTC - 1, MONDAY_UTC + 1, 2000, MONDAY_UTC / 60));

    c.restart_minute = 23 * 60 + 59;
    c.restart_days |= 1 << 6;
    assert(maintenance_next_restart(&c, MONDAY_UTC - 120, 0) == MONDAY_UTC - 60);
    assert(maintenance_restart_due(&c, MONDAY_UTC - 61, MONDAY_UTC - 60, 1000, 0));
    assert(maintenance_next_restart(&c, MONDAY_UTC - 60, 0) == MONDAY_UTC + 86340);
    assert(maintenance_restart_due(&c, MONDAY_UTC + 86339, MONDAY_UTC + 86340, 1000, 0));
    c.restart_enabled = 0;
    assert(!maintenance_next_restart(&c, MONDAY_UTC, 0));
    puts("PASS weekly schedule: China midnight and week rollover, single/multiple days, large clock corrections, durable deduplication, and independent idle cleanup");
    return 0;
}
'''
LABEL_ENV=r'''
static bool device_maintenance_write_begin(void){return true;}
static void device_maintenance_write_end(void){}
'''
LABEL_TEST=r'''
static np_access_record_t ref(unsigned n){np_access_record_t r={0};r.peer.family=4;r.peer.address[0]=192;r.peer.address[1]=0;r.peer.address[2]=2;r.peer.address[3]=(uint8_t)n;r.peer.port=54321;strcpy(r.peer.name,"old");return r;}
int main(void){
 assert(peer_labels_init()==0);np_access_record_t active[16];for(int i=0;i<16;i++)active[i]=ref(i+1);
 char text[97];peer_labels_get(&active[0],text);assert(!*text);
 assert(peer_labels_set(&active[0],"山顶中继",0,active,16)==0);assert(peer_labels_revision()==1);
 peer_labels_get(&active[0],text);assert(!strcmp(text,"山顶中继"));
 int before=commits;assert(peer_labels_set(&active[0],"山顶中继",1,active,16)==0&&before==commits);
 assert(peer_labels_set(&active[0],"wrong",0,active,16)==ESP_ERR_INVALID_STATE);
 fail_commit=true;assert(peer_labels_set(&active[0],"unsaved",1,active,16)!=0);fail_commit=false;
 peer_labels_get(&active[0],text);assert(!strcmp(text,"山顶中继")&&peer_labels_revision()==1);
 np_access_record_t learned=active[0];learned.mac[5]=9;peer_labels_bind(&active[0],&learned);active[0]=learned;
 assert(peer_labels_flush()==0);free(s_labels);s_labels=NULL;s_mutex=0;assert(peer_labels_init()==0);
 active[0].peer.address[3]=99;active[0].peer.port=60000;peer_labels_get(&active[0],text);assert(!strcmp(text,"山顶中继"));
 np_access_record_t other=active[0];other.mac[5]=8;peer_labels_get(&other,text);assert(!*text);
 char max[98]={0};for(int i=0;i<24;i++)memcpy(max+i*4,"\xf0\x9f\x90\xb1",4);
 assert(peer_labels_set(&active[0],max,1,active,16)==0);strcat(max,"x");assert(peer_labels_set(&active[0],max,2,active,16)==ESP_ERR_INVALID_ARG);
 for(int i=1;i<16;i++){char n[20];snprintf(n,sizeof(n),"peer %d",i);assert(peer_labels_set(&active[i],n,(uint32_t)(i+1),active,16)==0);}
 active[1]=ref(100);assert(peer_labels_set(&active[1],"replacement",17,active,16)==0);
 fail_alloc=true;assert(peer_labels_set(&active[1],"oom",18,active,16)==ESP_ERR_NO_MEM);fail_alloc=false;
 assert(peer_labels_set(&active[0],"",18,active,16)==0);peer_labels_get(&active[0],text);assert(!*text);
 before=commits;assert(peer_labels_flush()==0&&commits==before);
 assert(peer_labels_reset()==0);peer_labels_get(&active[1],text);assert(!*text);
 puts("PASS labels: 16 peers, UTF-8 limits, durable reload, failed commit rollback, stale revision, identity/NAT binding, shared-address identity isolation, departed-label reclamation, OOM, clear and no periodic writes");return 0;
}
'''
SERVICE_ENV=r'''
#define RTC_NOINIT_ATTR
#define ESP_RST_SW 3
#define PTT_IDLE 0
#define PTT_OWNER_NONE 0
#define VOICE_TALK 2
#define pdMS_TO_TICKS(x) (x)
static int64_t clock_ms, wall_utc, last_slot_saved, pending_slot;
static bool station_busy, storing, lock_available=true, preparing_fails, boot_held;
static bool board_boot_button_pressed(void) {return boot_held;}
static int boots, latches, clean_calls, flushes, prepared, rf_off;
maintenance_clock_t maintenance_clock_get(void) {
    return (maintenance_clock_t){.utc=wall_utc,.state=wall_utc?"synced":"waiting"};
}
static int maintenance_get_blob(int h, const char *key, void *out, size_t *size) {
    if ((!strcmp(key,"config2") && durable_n==12) || (!strcmp(key,"config1") && durable_n!=12)) return ESP_ERR_NVS_NOT_FOUND;
    return nvs_get_blob(h,key,out,size);
}
static int nvs_get_i64(int h, const char *key, int64_t *value) {
    (void)h;assert(!strcmp(key,"last_slot"));
    if(!last_slot_saved) return ESP_ERR_NVS_NOT_FOUND;
    *value=last_slot_saved;return ESP_OK;
}
static int nvs_set_i64(int h, const char *key, int64_t value) {
    (void)h;assert(!strcmp(key,"last_slot"));pending_slot=value;return ESP_OK;
}
static int maintenance_commit(int h) {
    const int result=nvs_commit(h);
    if (!result && pending_slot) last_slot_saved=pending_slot;
    pending_slot=0;
    return result;
}
#define nvs_get_blob maintenance_get_blob
#define nvs_commit maintenance_commit
static int factory_reset_prepare(void) { ++prepared;return preparing_fails?77:ESP_OK; }
static void vTaskDelay(int delay) { assert(delay==50); }
static void board_sa818_power(bool on) { assert(!on && latches>rf_off);++rf_off; }

static int64_t recent_activity_us;
static int64_t ptt_last_activity_us(void){return recent_activity_us;}
static int64_t esp_timer_get_time(void){return clock_ms*1000;}
static int esp_reset_reason(void){return 0;}
static bool app_control_generation_begin(unsigned g){(void)g;return lock_available;}
static void app_control_end(void){}
static int ptt_get_state(void){return station_busy?1:0;}
static bool board_ptt_is_asserted(void){return station_busy;}
static int ptt_owner(void){return 0;}
static int ptt_lock_depth;
static bool update_blocked;
static void ptt_control_lock(void){++ptt_lock_depth;}
static void ptt_control_unlock(void){assert(ptt_lock_depth>0);--ptt_lock_depth;}
static void ptt_maintenance_block(bool enable){assert(ptt_lock_depth>0);update_blocked=enable;}
static bool ptt_local_audio_active(void){return false;}
static int voice_mode(void){return 0;}
static bool voice_rf_carrier_present(void){return false;}
static bool voice_network_busy(void){return false;}
static size_t voice_bridge_level(void){return 0;}
static bool voice_sdr_tail_active(void){return false;}
static bool tail_preview_active(void){return false;}
static bool speaker_diag_output_active(void){return false;}
static bool tail_store_busy(void){return storing;}
static size_t netp2pnet_cleanup_idle(int64_t n,int64_t d,unsigned*x){(void)n;assert(d==60000);*x=1;++clean_calls;return 1234;}
static unsigned netrelay_cleanup_idle(int64_t n,int64_t d){(void)n;(void)d;return 1;}
static size_t net_http_cleanup_idle(void){return 100;}
static int peer_labels_flush(void){++flushes;return 0;}
static void ptt_prepare_reboot(void){++latches;station_busy=false;}
static int sdr_set_enabled(bool e){assert(!e);return 0;}
static int voice_stop(void){return 0;}
static void board_amp_enable(bool e){assert(!e);}
static void esp_restart(void){++boots;}
'''
SERVICE_TEST=r'''
#define MONDAY_UTC 1795363200LL
int main(void) {
    const uint32_t legacy[3]={1,6,0x01000018}; /* 24 h, cleanup off, interval enabled */
    memcpy(durable,legacy,sizeof(legacy));durable_n=sizeof(legacy);
    assert(device_maintenance_init()==0);
    maintenance_config_t c;
    device_maintenance_config(&c);
    assert(!c.cleanup_enabled && !c.restart_enabled && c.revision==6 && c.restart_minute==240);
    c.cleanup_enabled=1;c.restart_enabled=1;c.restart_days=1;c.restart_minute=0;
    fail_commit=true;assert(device_maintenance_set(&c,6)!=0);fail_commit=false;
    device_maintenance_config(&c);
    assert(!c.restart_enabled && c.revision==6);
    c.cleanup_enabled=1;c.restart_enabled=1;c.restart_days=1;c.restart_minute=0;
    assert(device_maintenance_set(&c,6)==0 && device_maintenance_set(&c,6)==ESP_ERR_INVALID_STATE);
    device_maintenance_tick();
    clock_ms=59999;assert(device_maintenance_cleanup()==ESP_ERR_INVALID_STATE);
    clock_ms=60000;assert(device_maintenance_cleanup()==0 && clean_calls==1);
    char json[1500];device_maintenance_json(json,sizeof(json));
    assert(strstr(json,"\"released_buffer_bytes\":1334"));
    recent_activity_us=clock_ms*1000;clock_ms++;assert(device_maintenance_cleanup()==ESP_ERR_INVALID_STATE);

    wall_utc=MONDAY_UTC;clock_ms+=1000;device_maintenance_tick();
    assert(!boots && !s_due_slot); /* first synchronization inside the minute */
    wall_utc=MONDAY_UTC-1;clock_ms+=1000;device_maintenance_tick();
    station_busy=true;lock_available=false;
    wall_utc++;clock_ms+=1000;device_maintenance_tick();
    assert(s_due_slot && !boots); /* crossing tracked during an OTA/write lock */
    lock_available=true;
    assert(device_maintenance_write_begin());
    wall_utc++;clock_ms+=1000;device_maintenance_tick();assert(!boots);
    device_maintenance_write_end();
    fail_commit=true;wall_utc++;clock_ms+=1000;device_maintenance_tick();
    assert(s_restart_save_failed && !boots && !last_slot_saved);
    fail_commit=false;
    wall_utc+=5;clock_ms+=5000;device_maintenance_tick();
    assert(boots==1 && latches==1 && rf_off==1 && s_rtc_kind==REBOOT_SCHEDULED);
    assert(last_slot_saved==MONDAY_UTC/60);

    /* Simulate power loss and persistent re-load after the scheduled reboot. */
    s_lock=0;s_reboot_kind=0;s_previous_utc=0;s_last_slot=0;
    assert(device_maintenance_init()==0 && s_last_slot==MONDAY_UTC/60);
    wall_utc=MONDAY_UTC-1;clock_ms+=1000;device_maintenance_tick();
    wall_utc++;clock_ms+=1000;device_maintenance_tick();assert(boots==1 && !s_due_slot);

    wall_utc=MONDAY_UTC+7*86400-1;clock_ms+=1000;device_maintenance_tick();
    storing=true;
    wall_utc++;clock_ms+=1000;device_maintenance_tick();assert(s_due_slot && boots==1);
    assert(device_maintenance_reboot()==ESP_ERR_INVALID_STATE);
    assert(device_maintenance_factory_reset()==ESP_ERR_INVALID_STATE);
    device_maintenance_config(&c);c.restart_enabled=0;
    assert(device_maintenance_set(&c,7)==0 && !s_due_slot);
    storing=false;
    assert(device_maintenance_write_begin());
    assert(device_maintenance_factory_reset()==ESP_ERR_INVALID_STATE);
    device_maintenance_write_end();
    station_busy=true;
    assert(!device_maintenance_update_begin() && !update_blocked);
    station_busy=false;
    boot_held=true;
    assert(!device_maintenance_update_begin());
    boot_held=false;
    assert(device_maintenance_update_begin() && update_blocked);
    assert(!device_maintenance_write_begin());
    assert(device_maintenance_factory_reset()==ESP_ERR_INVALID_STATE);
    assert(device_maintenance_reboot()==ESP_ERR_INVALID_STATE);
    device_maintenance_tick();
    assert(boots==1);
    device_maintenance_update_end(false);
    assert(device_maintenance_write_begin() && !device_maintenance_update_begin());
    device_maintenance_write_end();
    preparing_fails=true;assert(device_maintenance_factory_reset()==77 && !device_maintenance_reboot_pending());
    preparing_fails=false;
    assert(device_maintenance_factory_reset()==0 && device_maintenance_factory_reset()==0 && prepared==2);
    assert(!device_maintenance_write_begin() && device_maintenance_reboot()==ESP_ERR_INVALID_STATE);
    assert(device_maintenance_set(&c,8)==ESP_ERR_INVALID_STATE);
    clock_ms+=1499;device_maintenance_tick();assert(boots==1);
    boot_held=true;clock_ms++;device_maintenance_tick();assert(boots==1);
    boot_held=false;clock_ms++;device_maintenance_tick();
    assert(boots==2 && latches==2 && rf_off==2 && s_rtc_kind==REBOOT_FACTORY);
    s_lock=0;s_reboot_kind=0;s_previous_utc=0;update_blocked=false;
    assert(device_maintenance_init()==0);
    assert(device_maintenance_update_begin() && update_blocked);
    device_maintenance_update_end(true);
    assert(device_maintenance_reboot_pending() && !device_maintenance_write_begin());
    assert(device_maintenance_factory_reset()==ESP_ERR_INVALID_STATE);
    clock_ms+=1499;device_maintenance_tick();assert(boots==2);
    clock_ms++;device_maintenance_tick();assert(boots==3 && s_rtc_kind==REBOOT_UPDATE);
    puts("PASS service: migration, cleanup, weekly restart, exclusive online flash, safe factory and OTA reboot");
    return 0;
}
'''
CLOCK_ENV=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef int esp_err_t,portMUX_TYPE;
#define ESP_OK 0
#define ESP_LOGW(...) ((void)0)
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
struct timeval {int64_t tv_sec,tv_usec;};
typedef struct {
    const char *servers[2];
    void (*sync_cb)(struct timeval *value);
    bool wait_for_sync;
} esp_sntp_config_t;
#define ESP_SNTP_SERVER_LIST(...) {__VA_ARGS__}
#define ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(n,list) {.servers=list,.wait_for_sync=true}
static int errors,init_count,restart_count;
static int64_t monotonic_us, system_utc;
static void (*sync_callback)(struct timeval *);
static esp_err_t esp_netif_sntp_init(esp_sntp_config_t *config) {
    assert(config->servers[0]&&config->servers[1]&&!config->wait_for_sync&&config->sync_cb);
    ++init_count;
    if (errors) return errors;
    sync_callback=config->sync_cb;return ESP_OK;
}
static esp_err_t esp_netif_sntp_start(void) {++restart_count;return errors;}
static int64_t esp_timer_get_time(void) {return monotonic_us;}
static int64_t fake_time(void *out) {assert(!out);return system_utc;}
#define time fake_time
'''

CLOCK_TEST=r'''
#include <string.h>
int main(void) {
    maintenance_clock_t clock=maintenance_clock_get();
    assert(!strcmp(clock.state,"waiting")&&!clock.utc&&!clock.last_sync_utc);
    errors=1;maintenance_clock_network_ready();assert(init_count==1&&!restart_count);
    errors=0;maintenance_clock_network_ready();assert(init_count==2&&sync_callback);
    maintenance_clock_network_ready();assert(init_count==2&&restart_count==1);
    struct timeval sample={.tv_sec=1700000000LL};
    sync_callback(&sample);assert(!maintenance_clock_get().utc);
    sample.tv_sec=system_utc=1795075200LL;monotonic_us=1000;sync_callback(&sample);
    clock=maintenance_clock_get();
    assert(!strcmp(clock.state,"synced")&&clock.utc==system_utc&&clock.last_sync_utc==system_utc);
    monotonic_us+=2LL*60*60*1000000;
    system_utc+=2*60*60;
    assert(!strcmp(maintenance_clock_get().state,"holdover"));
    monotonic_us+=22LL*60*60*1000000;
    system_utc+=22*60*60;
    clock=maintenance_clock_get();
    assert(!strcmp(clock.state,"stale")&&!clock.utc&&clock.last_sync_utc==sample.tv_sec);
    sample.tv_sec=system_utc;sync_callback(&sample);
    assert(!strcmp(maintenance_clock_get().state,"synced"));
    puts("PASS board clock: invalid first time, SNTP initialization retry, reconnect, holdover, 24-hour expiry and resync");
    return 0;
}
'''
def run(name,source):
    env=dict(os.environ);env['PATH']=r'C:\Program Files\mingw64\bin;'+env.get('PATH','')
    (ROOT/'.tmp').mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='maintenance-'+name+'-',dir=ROOT/'.tmp') as temp:
        src=Path(temp)/'case.c';src.write_text(source,encoding='utf-8');exe=Path(temp)/'case.exe'
        env.update(TEMP=temp,TMP=temp,TMPDIR=temp)
        subprocess.run([shutil.which('gcc',path=env['PATH']),'-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable',str(src),'-lm','-o',str(exe)],check=True,env=env)
        subprocess.run([str(exe)],check=True,env=env)
def main():
    policy=plain('maintenance_policy.h')+plain('maintenance_policy.c')
    run('policy',BASE+policy+POLICY_TEST)
    run('clock',CLOCK_ENV+plain('maintenance_clock.h')+plain('maintenance_clock.c')+CLOCK_TEST)
    labels=plain('np_peer_store.h')+plain('np_access.h')+plain('np_access.c')+plain('peer_labels.h')
    labels+=function((MAIN/'device_name.c').read_text(encoding='utf-8'),'device_name_validate')
    labels=labels.replace('DEVICE_NAME_BYTES','96')
    run('labels',BASE+LABEL_ENV+labels+plain('peer_labels.c')+LABEL_TEST)
    run('service',BASE+policy+plain('maintenance_clock.h')+SERVICE_ENV+plain('device_maintenance.c')+SERVICE_TEST)
if __name__=='__main__':main()
