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
int main(void){
 maintenance_config_t c={.version=1,.cleanup_enabled=1,.restart_hours=24};maintenance_policy_t p;
 assert(maintenance_config_valid(&c));c.restart_hours=0;assert(!maintenance_config_valid(&c));c.restart_hours=169;assert(!maintenance_config_valid(&c));c.restart_hours=24;
 maintenance_policy_reset(&p,&c,0);assert(!p.next_restart_ms);
 assert(maintenance_policy_tick(&p,&c,0,false,false)==0);
 assert(maintenance_policy_tick(&p,&c,59999,false,true)==0);
 assert(maintenance_policy_tick(&p,&c,60000,false,true)==MAINTENANCE_CLEANUP);
 assert(maintenance_policy_tick(&p,&c,360000,true,false)==0);
 assert(maintenance_policy_tick(&p,&c,360001,false,false)==0);
 assert(maintenance_policy_tick(&p,&c,420000,false,false)==0);
 assert(maintenance_policy_tick(&p,&c,420001,false,false)==MAINTENANCE_CLEANUP);
 c.cleanup_enabled=0;c.restart_enabled=1;c.restart_hours=1;maintenance_policy_reset(&p,&c,1000);
 assert(maintenance_policy_tick(&p,&c,3601000,true,false)==0&&p.restart_waiting);
 assert(maintenance_policy_tick(&p,&c,7201000,true,false)==0);
 assert(maintenance_policy_tick(&p,&c,7201001,false,false)==0);
 assert(maintenance_policy_tick(&p,&c,7261000,false,false)==0);
 assert(maintenance_policy_tick(&p,&c,7261001,false,false)==MAINTENANCE_RESTART);
 c.restart_enabled=0;maintenance_policy_reset(&p,&c,7261002);assert(!p.restart_waiting&&!p.next_restart_ms);
 assert(maintenance_policy_tick(&p,&c,9000000,false,false)==0);
 c.restart_enabled=1;c.restart_hours=168;maintenance_policy_reset(&p,&c,INT64_C(5000000000));assert(p.next_restart_ms==INT64_C(5604800000));
 puts("PASS policy: defaults, bounds, 60-second idle, busy postponement, cancellation, reboot reset, 64-bit long uptime");return 0;
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
static int64_t clock_ms;static bool station_busy,storing,lock_available=true;static int boots,latches,clean_calls,flushes;
static int64_t recent_activity_us;
static int64_t ptt_last_activity_us(void){return recent_activity_us;}
static int64_t esp_timer_get_time(void){return clock_ms*1000;}
static int esp_reset_reason(void){return 0;}
static bool app_control_generation_begin(unsigned g){(void)g;return lock_available;}
static void app_control_end(void){}
static int ptt_get_state(void){return station_busy?1:0;}
static bool board_ptt_is_asserted(void){return station_busy;}
static int ptt_owner(void){return 0;}
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
int main(void){
 assert(device_maintenance_init()==0);maintenance_config_t c;device_maintenance_config(&c);assert(c.cleanup_enabled&&!c.restart_enabled&&c.restart_hours==24);
 c.restart_enabled=1;c.restart_hours=1;fail_commit=true;assert(device_maintenance_set(&c,0)!=0);fail_commit=false;
 device_maintenance_config(&c);assert(!c.restart_enabled&&c.revision==0);
 c.restart_enabled=1;c.restart_hours=1;assert(device_maintenance_set(&c,0)==0);assert(device_maintenance_set(&c,0)==ESP_ERR_INVALID_STATE);
 device_maintenance_tick();clock_ms=59999;assert(device_maintenance_cleanup()==ESP_ERR_INVALID_STATE);
 clock_ms=60000;assert(device_maintenance_cleanup()==0&&clean_calls==1);char json[1500];device_maintenance_json(json,sizeof(json));assert(strstr(json,"\"released_buffer_bytes\":1334"));
 recent_activity_us=clock_ms*1000;clock_ms+=1;assert(device_maintenance_cleanup()==ESP_ERR_INVALID_STATE); // sub-tick activity is not missed
 station_busy=true;clock_ms=3600000;device_maintenance_tick();assert(!device_maintenance_reboot_pending()&&s_policy.restart_waiting&&!boots);
 station_busy=false;clock_ms=3600001;device_maintenance_tick();clock_ms=3660000;device_maintenance_tick();assert(!device_maintenance_reboot_pending());
 clock_ms=3660001;device_maintenance_tick();assert(device_maintenance_reboot_pending());
 device_maintenance_config(&c);c.restart_enabled=0;assert(device_maintenance_set(&c,1)==0&&!device_maintenance_reboot_pending());clock_ms+=2000;device_maintenance_tick();assert(!boots);
 assert(device_maintenance_write_begin());assert(device_maintenance_reboot(true)==ESP_ERR_INVALID_STATE);device_maintenance_write_end();
 storing=true;assert(device_maintenance_reboot(true)==ESP_ERR_INVALID_STATE);storing=false;
 station_busy=true;assert(device_maintenance_reboot(true)==0);assert(!device_maintenance_write_begin());assert(device_maintenance_reboot(true)==0);
 clock_ms+=1499;device_maintenance_tick();assert(!boots);clock_ms+=1;device_maintenance_tick();assert(boots==1&&latches==1&&s_rtc_kind==1);
 puts("PASS service: default settings, durable config failure, conflict, idle cleanup accounting, deferred schedule, cancellation after arm, upload/conversion protection, deduplicated manual reboot, no new writes, unkey before reboot");return 0;
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
    labels=plain('np_peer_store.h')+plain('np_access.h')+plain('np_access.c')+plain('peer_labels.h')
    labels+=function((MAIN/'device_name.c').read_text(encoding='utf-8'),'device_name_validate')
    labels=labels.replace('DEVICE_NAME_BYTES','96')
    run('labels',BASE+LABEL_ENV+labels+plain('peer_labels.c')+LABEL_TEST)
    run('service',BASE+policy+SERVICE_ENV+plain('device_maintenance.c')+SERVICE_TEST)
if __name__=='__main__':main()
