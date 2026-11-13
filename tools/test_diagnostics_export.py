"""Production on-demand diagnostic JSON bounds and allocation failure cleanup."""
from pathlib import Path
import json,os,subprocess,tempfile
from test_np_peer_runtime import function
ROOT=Path(__file__).resolve().parents[1]
PREFIX=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <stdatomic.h>
#include "ptt.h"
#include "sa818_cfg.h"
#define ESP_OK 0
#define ESP_FAIL -1
#define HTTPD_500_INTERNAL_SERVER_ERROR 500
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
typedef void *httpd_handle_t;
typedef struct {int unused;} httpd_req_t;
typedef struct {const char *version;} app_desc_t;
static httpd_handle_t s_plain_handle=(void*)1,s_tls_handle=(void*)2;
static atomic_uint s_alloc_failures=UINT32_MAX,s_alloc_size=UINT32_MAX,s_alloc_caps=UINT32_MAX;
static atomic_uint s_https_errors=UINT32_MAX,s_https_opens=UINT32_MAX,s_https_closes=UINT32_MAX;
static atomic_int s_https_error=-123,s_https_tls_error=-30592;
static unsigned allocation,fail_at,alive;
static char response[20000];
static void *tracked_malloc(size_t n){allocation++;if(allocation==fail_at)return NULL;alive++;return malloc(n);}
static void tracked_free(void *p){if(p){assert(alive);alive--;free(p);}}
#define free tracked_free
static void *heap_caps_malloc(size_t n,unsigned caps){assert(caps&MALLOC_CAP_SPIRAM);return tracked_malloc(n);}
static size_t heap_caps_get_free_size(unsigned caps){(void)caps;return UINT32_MAX;}
static size_t heap_caps_get_minimum_free_size(unsigned caps){(void)caps;return UINT32_MAX;}
static size_t heap_caps_get_largest_free_block(unsigned caps){(void)caps;return UINT32_MAX;}
static int64_t esp_timer_get_time(void){return INT64_C(4294967295)*1000;}
static const char *device_identity_id(void){return "orl-test";}
static const app_desc_t *esp_app_get_description(void){static app_desc_t a={"r11-93"};return &a;}
static int httpd_get_client_list(httpd_handle_t h,size_t *n,int *fds){(void)h;assert(*n>=2);*n=2;fds[0]=7;fds[1]=8;return ESP_OK;}
static int httpd_resp_send_err(httpd_req_t *req,int code,const char *text){(void)req;(void)text;assert(code==500);return ESP_FAIL;}
static void httpd_resp_set_hdr(httpd_req_t *req,const char *key,const char *value){(void)req;assert(!strcmp(key,"Cache-Control")&&!strcmp(value,"no-store"));}
static int send_json(httpd_req_t *req,const char *text){(void)req;assert(strlen(text)<sizeof(response));strcpy(response,text);return ESP_OK;}
void sa818_cfg_status(sa818_cfg_status_t *s){*s=(sa818_cfg_status_t){UINT32_MAX,UINT32_MAX,7,UINT32_MAX,-1,true,true,true};}
size_t ptt_trace_snapshot(ptt_trace_event_t *events,size_t capacity,uint32_t *sequence){
 assert(capacity==64);*sequence=UINT32_MAX;for(unsigned i=0;i<64;i++)events[i]=(ptt_trace_event_t){UINT32_MAX,UINT32_MAX-i,65535,65535,UINT32_MAX,UINT32_MAX};return 64;
}
'''
TEST=r'''
int main(void){httpd_req_t req={0};
 for(unsigned i=1;i<=2;i++){allocation=0;fail_at=i;assert(h_diagnostics(&req)==ESP_FAIL&&alive==0);}
 allocation=fail_at=0;assert(h_diagnostics(&req)==ESP_OK&&alive==0);puts(response);return 0;}
'''
def main():
    source=(ROOT/'firmware/main/net_http.c').read_text('utf-8')
    code=PREFIX+''.join(function(source,n) for n in ('radio_config_json','http_clients','h_diagnostics'))+TEST
    scratch=ROOT/'.tmp';scratch.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='diagnostics-',dir=scratch) as temp:
        d=Path(temp);(d/'esp_err.h').write_text('typedef int esp_err_t;\n')
        (d/'test.c').write_text(code,encoding='utf-8');exe=d/'test.exe';env=dict(os.environ,TEMP=temp,TMP=temp,TMPDIR=temp)
        subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-I',str(d),'-I',str(ROOT/'firmware/main'),str(d/'test.c'),'-o',str(exe)],check=True,env=env)
        raw=subprocess.check_output([str(exe)],env=env,text=True);data=json.loads(raw)
        assert len(data['events'])==64 and data['event_sequence']==4294967295
        assert all(len(e)==6 and all(isinstance(x,int) for x in e) for e in data['events'])
        assert data['memory']['internal_free']==4294967295 and data['connections']['last_tls_error']==-30592
        assert set(data)=={'version','device_id','firmware','uptime_ms','memory','connections','rf_config','event_sequence','events'}
        assert len(raw)<12288
        print('PASS: diagnostic export maximum counters/64 events form valid bounded JSON; both PSRAM allocation failures reclaim memory')
if __name__=='__main__':main()
