"""Exercise production Wi-Fi driver orchestration and NVS APIs with fakes."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
from test_tx_ownership import function
ROOT=Path(__file__).resolve().parent.parent
STUBS=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "wifi_policy.h"
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NVS_NOT_FOUND 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_NO_MEM 3
#define ESP_ERR_INVALID_ARG 4
#define ESP_ERR_TIMEOUT 5
#define ESP_ERR_NOT_FOUND 6
#define ESP_ERR_INVALID_SIZE 7
#define NVS_READWRITE 1
#define NVS_READONLY 0
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define BIT0 1
#define BIT1 2
#define BIT2 4
#define BIT3 8
#define BIT4 16
#define BIT5 32
#define portMUX_INITIALIZER_UNLOCKED 0
#define WIFI_AUTH_OPEN 0
#define WIFI_IF_STA 0
#define WIFI_IF_AP 1
#define WIFI_MODE_STA 1
#define WIFI_MODE_APSTA 3
#define CONFIG_ESP_WIFI_ENABLE_WPA3_SAE 0
#define IPSTR "%u.%u.%u.%u"
#define IP2STR(a) 10u,10u,1u,178u
#define pdMS_TO_TICKS(n) (n)
#define pdTRUE 1
#define MALLOC_CAP_SPIRAM 1
#define DNS_SERVER_CONFIG_SINGLE(a,b) 1
typedef int nvs_handle_t;
typedef int esp_netif_t;
typedef int StaticSemaphore_t;
typedef int SemaphoreHandle_t;
typedef int TaskHandle_t;
typedef int portMUX_TYPE;
typedef void* dns_server_handle_t;
typedef int dns_server_config_t;
typedef struct {uint32_t addr;} ip_addr_t;
typedef struct {ip_addr_t ip,gw;} esp_netif_ip_info_t;
typedef struct {uint8_t ssid[33]; int rssi,authmode;} wifi_ap_record_t;
typedef struct {struct{uint8_t ssid[32],password[64];int threshold_unused;struct{int authmode;}threshold;struct{bool capable;}pmf_cfg;}sta;
 struct{uint8_t ssid[32];int ssid_len,channel,max_connection,authmode;}ap;}wifi_config_t;
typedef struct {bool show_hidden;struct{struct{int min,max;}active;}scan_time;}wifi_scan_config_t;
static int64_t clock_ms;
static bool associated,ip_ready,scanning,have_blob,nvs_failure;
static bool radio_stopped;
static wifi_profile_store_t disk,pending_disk;
static bool pending_blob;
static char legacy_ssid[33],legacy_pass[65],legacy_host[64];
static int connect_calls,scan_calls,driver_mode=WIFI_MODE_STA;
static wifi_ap_record_t scan_records[32];static uint16_t scan_count;
static wifi_config_t driver_config;
static int64_t esp_timer_get_time(void){return clock_ms*1000;}
static int xSemaphoreTake(int h,int t){return 1;}
static void xSemaphoreGive(int h){}
static int nvs_open(const char*n,int mode,nvs_handle_t*h){*h=1;pending_blob=false;return ESP_OK;}
static void nvs_close(int h){}
static int nvs_get_str(int h,const char*k,char*out,size_t*len){
 const char *s=!strcmp(k,"wssid")?legacy_ssid:!strcmp(k,"wpass")?legacy_pass:legacy_host;
 if(!*s)return ESP_ERR_NVS_NOT_FOUND;if(strlen(s)+1>*len)return ESP_ERR_INVALID_SIZE;
 strcpy(out,s);*len=strlen(s)+1;return ESP_OK;
}
static int nvs_get_blob(int h,const char*k,void*out,size_t*len){if(!have_blob)return ESP_ERR_NVS_NOT_FOUND;if(*len<sizeof(disk))return ESP_ERR_INVALID_SIZE;memcpy(out,&disk,sizeof(disk));*len=sizeof(disk);return ESP_OK;}
static int nvs_set_blob(int h,const char*k,const void*v,size_t len){if(nvs_failure)return ESP_ERR_NO_MEM;assert(len==sizeof(disk));memcpy(&pending_disk,v,len);pending_blob=true;return ESP_OK;}
static int nvs_commit(int h){if(nvs_failure)return ESP_ERR_NO_MEM;if(pending_blob){disk=pending_disk;have_blob=true;pending_blob=false;}return ESP_OK;}
static int nvs_erase_key(int h,const char*k){if(!strcmp(k,"wssid"))legacy_ssid[0]=0;if(!strcmp(k,"wpass"))legacy_pass[0]=0;return ESP_OK;}
static int esp_wifi_sta_get_ap_info(wifi_ap_record_t *a){if(!associated)return ESP_ERR_NOT_FOUND;memset(a,0,sizeof(*a));memcpy(a->ssid,driver_config.sta.ssid,32);return ESP_OK;}
static int esp_wifi_set_mode(int mode){if(mode==WIFI_MODE_APSTA)assert(!associated);driver_mode=mode;return ESP_OK;}
static int esp_wifi_set_config(int iface,const wifi_config_t*c){if(iface==WIFI_IF_STA)driver_config=*c;return ESP_OK;}
static int esp_wifi_connect(void){assert(driver_mode==WIFI_MODE_STA);++connect_calls;return ESP_OK;}
static int esp_wifi_disconnect(void){associated=ip_ready=false;return ESP_OK;}
static int esp_wifi_scan_start(const wifi_scan_config_t*c,bool block){assert(!block);assert(!scanning);scanning=true;++scan_calls;return ESP_OK;}
static int esp_wifi_scan_stop(void){scanning=false;return ESP_OK;}
static int esp_wifi_clear_ap_list(void){return ESP_OK;}
static int esp_wifi_scan_get_ap_records(uint16_t*n,wifi_ap_record_t*a){*n=scan_count;memcpy(a,scan_records,*n*sizeof(*a));scanning=false;return ESP_OK;}
static int esp_wifi_stop(void){radio_stopped=true;return ESP_OK;}
static esp_netif_t *esp_netif_create_default_wifi_ap(void){static int n;return &n;}
static int esp_netif_set_default_netif(void*n){return ESP_OK;}
static int esp_netif_dhcpc_start(void*n){return ESP_OK;}
static int esp_netif_get_ip_info(void*n,esp_netif_ip_info_t*i){memset(i,0,sizeof(*i));i->ip.addr=ip_ready?1:0;return ESP_OK;}
static void* start_dns_server(void*c){return (void*)1;}
static void stop_dns_server(void*d){}
static int mdns_hostname_set(const char*s){return ESP_OK;}
static void ip6_ensure_linklocal(void){}
static void announce_gateway(int64_t n){}
static const char*esp_err_to_name(int e){return "fake";}
static const char*device_identity_id(void){return "orl-001122334455";}
'''
HARNESS=r'''
static void reset(int64_t now){
 memset(s_manager,0,sizeof(*s_manager));s_manager->store.version=1;s_manager->store.next_id=1;
 wifi_policy_init(&s_manager->policy,now);strcpy(s_manager->scan_json,"[]");
 s_mutex=1;s_netif=(void*)1;s_started=true;s_connected=s_ap_on=false;
 s_scan_running=s_scan_requested=s_request_ap=s_request_disconnect=s_scan_discard=false;
 s_debug_stop_pending=s_debug_radio_off=s_storage_fault=false;
 s_apply_id=0;s_apply_at=s_hold_until=s_next_scan=s_disconnect_at=0;s_last_scan=-1;
 s_ap_suppressed=s_request_ap_close=false;s_config_until=s_setup_until=0;s_setup_owner[0]=0;
 associated=ip_ready=scanning=radio_stopped=false;connect_calls=scan_calls=scan_count=0;
 clock_ms=now;have_blob=nvs_failure=false;legacy_ssid[0]=legacy_pass[0]=legacy_host[0]=0;
}
static uint32_t save(const char*ssid,bool enabled,int priority){uint32_t id=0;assert(net_wifi_profile_save(&id,ssid,"test-only-secret",enabled,priority,NULL)==ESP_OK);return id;}
static void tick(int64_t now,uint32_t events){clock_ms=now;manager_step(events,now);}
static void visible(const char *a,const char*b){
 scan_count=0;memset(scan_records,0,sizeof(scan_records));
 if(a){strcpy((char*)scan_records[scan_count].ssid,a);scan_records[scan_count++].rssi=-50;}
 if(b){strcpy((char*)scan_records[scan_count].ssid,b);scan_records[scan_count++].rssi=-70;}
}
int main(void){
 s_manager=calloc(1,sizeof(*s_manager));reset(0);
 assert(load_profiles()==ESP_OK&&!strcmp(s_hostname,"heimao"));reset(0);
 strcpy(legacy_ssid,"legacy");strcpy(legacy_pass,"test-only-secret");
 assert(load_profiles()==ESP_OK&&!strcmp(s_hostname,"orl"));reset(0);
 strcpy(legacy_ssid,"legacy");strcpy(legacy_pass,"test-only-secret");strcpy(legacy_host,"legacy-host");
 assert(load_profiles()==ESP_OK && have_blob && s_manager->store.profiles[0].id==1);
 assert(!strcmp(s_manager->store.profiles[0].password,legacy_pass));
 assert(!strcmp(s_manager->store.hostname,"legacy-host"));
 memset(&s_manager->store,0,sizeof(s_manager->store));assert(load_profiles()==ESP_OK);
 assert(s_manager->store.next_id==2); /* reboots do not duplicate imported profiles */
 uint32_t id=1;assert(net_wifi_profile_save(&id,NULL,NULL,false,7,"NEW-HOST")==ESP_OK);
 assert(!strcmp(s_manager->store.profiles[0].password,"test-only-secret"));
 assert(!strcmp(disk.hostname,"new-host"));
 nvs_failure=true;assert(net_wifi_profile_forget(1)==ESP_ERR_NO_MEM);
 assert(s_manager->store.profiles[0].id==1);nvs_failure=false;
 assert(net_wifi_profile_forget(1)==ESP_OK);
 memset(&s_manager->store,0,sizeof(s_manager->store));assert(load_profiles()==ESP_OK);
 assert(!s_manager->store.profiles[0].id); /* valid empty store overrides rollback credentials */
 reset(0);uint32_t a=save("A",true,0),b=save("B",true,1),c=save("C",true,0);
 tick(0,0);assert(s_scan_running && !s_ap_on);
 visible("A",NULL);tick(1000,EV_SCAN);assert(connect_calls==1 && !strcmp(s_ssid,"A"));
 associated=ip_ready=true;tick(2000,EV_IP|EV_ASSOC);assert(s_connected && s_manager->policy.offline_since==-1);
 tick(70000,0);assert(s_connected && connect_calls==1 && !s_ap_on && scan_calls==1);
 associated=ip_ready=false;tick(71000,EV_DISC);
 assert(s_manager->policy.offline_since==71000 && s_scan_running);
 visible("B","C");tick(72000,EV_SCAN);assert(!strcmp(s_ssid,"B"));
 tick(73000,EV_DISC);assert(!strcmp(s_ssid,"C")); /* failed B cools; C proceeds */
 associated=ip_ready=true;tick(74000,EV_IP);assert(s_connected);
 char json[8192];assert(net_wifi_profiles_json(json,sizeof(json))==ESP_OK);
 assert(!strstr(json,"test-only-secret") && strstr(json,"has_password"));
 /* A stale LOST_IP arriving after a new GOT_IP cannot break a healthy link. */
 tick(75000,EV_IP|EV_LOST_IP|EV_DISC);assert(s_connected);
 assert(net_wifi_profile_forget(c)==ESP_OK);tick(76000,0);assert(s_connected);
 tick(76500,0);assert(!s_connected && s_hold_until);
 tick(77250,0);assert(wifi_profiles_find(&s_manager->store,c)<0);
 reset(0);a=save("A",true,0);b=save("B",true,0);
 s_last_scan=55000;wifi_policy_seen(&s_manager->policy,0,-40);tick(55000,0);
 assert(s_manager->policy.attempt_until==60000);
 associated=true;ip_ready=false;tick(60000,0);assert(!associated && s_manager->policy.recovery_ap);
 tick(60750,0);assert(s_ap_on);assert(s_manager->policy.offline_since==0);
 tick(62250,0);assert(s_scan_running);
 visible("B",NULL);tick(63000,EV_SCAN);assert(!s_ap_on && !strcmp(s_ssid,"B"));
 tick(64000,EV_DISC);assert(s_ap_on);assert(s_manager->policy.candidates[1].retry_at==94000);
 /* Explicit request waits for its ACK delay, preempts auto recovery once,
  * and can target a saved profile whose auto-join is disabled. */
 id=b;assert(net_wifi_profile_save(&id,NULL,NULL,false,0,NULL)==ESP_OK);
 assert(net_wifi_profile_connect(b,1500)==ESP_OK);tick(65000,0);assert(s_ap_on);
 tick(65500,0);assert(s_hold_until);tick(66250,0);
 assert(!s_apply_id && !s_ap_on && s_manager->policy.active==1);
 associated=ip_ready=true;tick(67000,EV_IP);assert(s_connected);
 assert(net_wifi_debug_radio_off_schedule()==ESP_OK);tick(68500,0);
 assert(radio_stopped && s_debug_radio_off);tick(1000000,0);assert(!s_ap_on && !s_connected);
 assert(net_wifi_profile_connect(a,1500)==ESP_ERR_INVALID_STATE);
 /* A connected phone gets a grace period. App holds pause automatic scans
  * and saved-network rejoin, while explicit scans remain available. */
 reset(0);a=save("A",true,0);s_manager->policy.recovery_ap=true;
 assert(ap_start()==ESP_OK);s_next_scan=0;s_last_scan=0;
 wifi_policy_seen(&s_manager->policy,0,-40);tick(0,EV_AP_JOIN);
 assert(s_ap_on && !s_scan_running && s_config_until==30000);
 assert(net_wifi_setup_session("android-A",false)==ESP_OK);
 assert(s_setup_until==60000 && !s_config_until);
 tick(15000,0);assert(s_ap_on && !s_scan_running && !connect_calls);
 assert(net_wifi_setup_session("android-B",false)==ESP_ERR_INVALID_STATE);
 assert(net_wifi_setup_session("android-B",true)==ESP_ERR_INVALID_STATE);
 s_scan_requested=true;tick(15001,0);assert(s_scan_running);
 visible("A",NULL);tick(16000,EV_SCAN);assert(s_ap_on && !connect_calls);
 assert(net_wifi_setup_session("android-A",false)==ESP_OK && s_setup_until==76000);
 assert(net_wifi_profiles_json(json,sizeof(json))==ESP_OK && strstr(json,"\"setup_hold_ms\":60000"));
 assert(net_wifi_setup_session("android-A",true)==ESP_OK);
 tick(16001,0);assert(!s_ap_on && connect_calls==1); /* release resumes known Wi-Fi */
 assert(net_wifi_setup_session("android-A",false)==ESP_ERR_INVALID_STATE);
 /* An absent App cannot hold recovery forever; the next saved candidate
  * is eligible precisely at expiry, even at zero/64-bit clock boundaries. */
 reset(0);a=save("A",true,0);s_manager->policy.recovery_ap=true;assert(ap_start()==ESP_OK);
 assert(net_wifi_setup_session("android-A",false)==ESP_OK);
 s_last_scan=59999;wifi_policy_seen(&s_manager->policy,0,-40);
 tick(59999,0);assert(s_ap_on && !connect_calls);
 tick(60000,0);assert(!s_ap_on && connect_calls==1 && !s_setup_owner[0]);
 /* Explicit connect supersedes a hold only after its ACK delay. Late
  * heartbeat/association events cannot revoke the user's handoff. */
 reset(0);a=save("A",true,0);s_manager->policy.recovery_ap=true;assert(ap_start()==ESP_OK);
 assert(net_wifi_setup_session("android-A",false)==ESP_OK);
 assert(net_wifi_profile_connect(a,1500)==ESP_OK && !s_setup_until);
 assert(net_wifi_setup_session("android-A",false)==ESP_ERR_INVALID_STATE);
 tick(1499,EV_AP_JOIN);assert(s_ap_on && !connect_calls && !s_config_until);
 tick(1500,0);tick(2250,0);assert(!s_ap_on && connect_calls==1);
 reset(0);a=save("A",true,0);assert(net_wifi_clear()==ESP_OK);
 assert(!s_manager->store.profiles[0].id && have_blob);
 puts("PASS: production manager migration/reboot, atomic save failure, hostname, forgotten profiles, asynchronous scans, all saved candidates, IP-only success, healthy Wi-Fi retention, bounded AP recovery, AP/STA exclusion, manual ACK delay and explicit radio-off");
 free(s_manager);return 0;
}
'''
def main():
    source=(ROOT/'firmware/main/net_wifi.c').read_text(encoding='utf-8')
    declarations=source[source.index('static const char *TAG'):source.index('static int64_t wifi_now')]
    names=['wifi_now','wifi_lock','wifi_unlock','wifi_escape','persist_profiles','load_profiles',
           'ap_stop','ap_start','disconnect_for_switch','begin_candidate','auto_profiles','collect_scan','manager_step',
           'valid_host','net_wifi_profile_save','net_wifi_profile_connect','net_wifi_profile_can_connect',
           'net_wifi_profile_forget','net_wifi_clear','net_wifi_setup_session','net_wifi_debug_radio_off_schedule','net_wifi_profiles_json']
    code=STUBS+declarations+''.join(function(source,n) for n in names)+HARNESS
    with tempfile.TemporaryDirectory(prefix='wifi-manager-',dir=ROOT/'.tmp') as tmp:
        p=Path(tmp)/'test.c';p.write_text(code,encoding='utf-8');exe=Path(tmp)/'test.exe'
        env=dict(os.environ,TEMP=tmp,TMP=tmp,TMPDIR=tmp)
        subprocess.run([shutil.which('gcc'),'-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function',
                        '-Wno-unused-variable','-Wno-unused-parameter','-Wno-misleading-indentation',
                        '-I',str(ROOT/'firmware/main'),str(p),str(ROOT/'firmware/main/wifi_policy.c'),'-o',str(exe)],check=True,env=env)
        subprocess.run([str(exe)],check=True,env=env)
if __name__=='__main__':main()
