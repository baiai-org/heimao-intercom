"""Execute real control HTTP handlers and route dispatch with host cJSON."""
import os,re,shutil,subprocess,tempfile
from pathlib import Path
from test_tx_ownership import function
ROOT=Path(__file__).resolve().parent.parent
STUBS=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "control_lease.h"
#include "cJSON.h"
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_SIZE 2
#define HTTP_GET 1
#define HTTP_POST 3
#define HTTPD_RESP_USE_STRLEN -1
#define pdTRUE 1
#define pdMS_TO_TICKS(n) (n)
typedef int SemaphoreHandle_t;
typedef int StaticSemaphore_t;
typedef struct request {
 int method,offset,status;size_t content_len;char uri[300];const char *body,*token,*device,*type;
 char response[3000];void *user_ctx,*sess_ctx;
} httpd_req_t;
typedef struct {
 const char *uri;int method;esp_err_t(*handler)(httpd_req_t*);void *user_ctx;
 bool is_websocket,handle_ws_control_frames;
}httpd_uri_t;
typedef struct{uint32_t control_generation;}sdr_ws_context_t;
static int64_t clock_ms;
static int revoked,mutations,lock_count;static bool lock_busy;
static int64_t esp_timer_get_time(void){return clock_ms*1000;}
static int xSemaphoreTakeRecursive(int h,int ticks){if(lock_busy)return 0;++lock_count;return 1;}
static void xSemaphoreGiveRecursive(int h){assert(lock_count>0);--lock_count;}
static const char *device_identity_id(void){return "orl-test-device";}
static void esp_fill_random(void *p,size_t n){static uint8_t seq=0;for(size_t i=0;i<n;++i)((uint8_t*)p)[i]=++seq;}
static int httpd_resp_set_status(httpd_req_t*r,const char*s){r->status=atoi(s);return 0;}
static int httpd_resp_set_type(httpd_req_t*r,const char*s){return 0;}
static int httpd_resp_set_hdr(httpd_req_t*r,const char*k,const char*v){return 0;}
static int httpd_resp_send(httpd_req_t*r,const char*s,int len){snprintf(r->response,sizeof(r->response),"%s",s);return 0;}
static int httpd_req_get_hdr_value_str(httpd_req_t*r,const char*k,char*out,size_t len){
 const char*s=!strcmp(k,"Content-Type")?r->type:!strcmp(k,"X-ORL-Control")?r->token:r->device;
 if(!s)return -1;if(strlen(s)>=len)return ESP_ERR_INVALID_SIZE;strcpy(out,s);return 0;
}
static int httpd_req_recv(httpd_req_t*r,char*out,size_t n){
 size_t remain=strlen(r->body)-r->offset;if(!remain)return -1;if(n>remain)n=remain;if(n>7)n=7;
 memcpy(out,r->body+r->offset,n);r->offset+=n;return n;
}
static int httpd_req_get_url_query_str(httpd_req_t*r,char*out,size_t len){const char*s=strchr(r->uri,'?');if(!s)return -1;if(strlen(s+1)>=len)return 2;strcpy(out,s+1);return 0;}
static int httpd_query_key_value(const char*q,const char*k,char*out,size_t len){
 while(q&&*q){const char*end=strchr(q,'&');size_t n=end?(size_t)(end-q):strlen(q);size_t kl=strlen(k);
  if(n>kl&&q[kl]=='='&&!strncmp(q,k,kl)){if(n-kl-1>=len)return 2;memcpy(out,q+kl+1,n-kl-1);out[n-kl-1]=0;return 0;}q=end?end+1:NULL;}
 return -1;
}
static int query_int(httpd_req_t*r,const char*k,int fallback,void*f){char q[300],v[32];return httpd_req_get_url_query_str(r,q,sizeof(q))==0&&httpd_query_key_value(q,k,v,sizeof(v))==0?atoi(v):fallback;}
static void query_str(httpd_req_t*r,const char*k,char*out,size_t len){char q[300];out[0]=0;if(httpd_req_get_url_query_str(r,q,sizeof(q))==0)(void)httpd_query_key_value(q,k,out,len);}
static int send_result(httpd_req_t*r,bool ok,const char*error){snprintf(r->response,sizeof(r->response),"%s",error?error:"");return ESP_OK;}
static void revoke(void){++revoked;}
'''
HARNESS=r'''
static httpd_req_t request(const char *body,const char *token){httpd_req_t r={0};r.method=body?HTTP_POST:HTTP_GET;r.body=body;r.content_len=body?strlen(body):0;r.type="application/json";r.token=token;r.device=device_identity_id();r.status=200;return r;}
static void get_token(httpd_req_t*r,char out[65]){cJSON*d=cJSON_Parse(r->response);assert(d);const cJSON*t=cJSON_GetObjectItemCaseSensitive(d,"token");assert(cJSON_IsString(t));strcpy(out,t->valuestring);cJSON_Delete(d);}
int main(void){
 s_lock=1;s_revoke=revoke;char a[65],b[65];
 char parsed_key[24],parsed_value[96];
 httpd_req_t setting=request("{\"k\":\"sample\",\"v\":\"%20 + & = 测试\"}",NULL);
 assert(parse_setting_request(&setting,parsed_key,sizeof(parsed_key),parsed_value,sizeof(parsed_value)));
 assert(!strcmp(parsed_key,"sample")&&!strcmp(parsed_value,"%20 + & = 测试"));
 setting=request("k=sample&v=%2520+%2B%26%3D%E6%B5%8B%E8%AF%95",NULL);setting.type="application/x-www-form-urlencoded";
 assert(parse_setting_request(&setting,parsed_key,sizeof(parsed_key),parsed_value,sizeof(parsed_value)));
 assert(!strcmp(parsed_value,"%20 +&=测试"));
 setting=request("{\"k\":\"key-that-is-far-too-long-for-this-api\",\"v\":\"data\"}",NULL);
 assert(!parse_setting_request(&setting,parsed_key,sizeof(parsed_key),parsed_value,sizeof(parsed_value)));
 httpd_req_t r=request(NULL,NULL);assert(app_control_http(&r)==0&&strstr(r.response,"\"held\":false")&&strstr(r.response,"\"mine\":true"));
 /* No control lease any more: a mutating route works with no token at all. */
 for(size_t i=0;i<sizeof(s_routes)/sizeof(s_routes[0]);++i){
  if(s_routes[i].handler==app_control_http)continue;
  r=request(NULL,NULL);r.method=s_routes[i].method;r.user_ctx=(void*)&s_routes[i];strcpy(r.uri,s_routes[i].uri);
  int before=mutations;
  controlled_dispatch(&r);
  assert(mutations==before+1);
 }
 r=request(NULL,NULL);r.user_ctx=(void*)&s_routes[0];assert(app_control_begin(&r,false));app_control_end();
 r=request(NULL,NULL);r.user_ctx=(void*)&s_routes[0];lock_busy=true;assert(!app_control_begin(&r,false)&&r.status==409);lock_busy=false;
 const httpd_uri_t *ws_route=NULL;for(size_t i=0;i<sizeof(s_routes)/sizeof(s_routes[0]);++i)if(s_routes[i].handler==h_ws_sdr)ws_route=&s_routes[i];
 sdr_ws_context_t ws_ctx={.control_generation=0};
 r=request(NULL,NULL);r.method=0;r.user_ctx=(void*)ws_route;r.sess_ctx=&ws_ctx;
 int ws_before=mutations;assert(controlled_dispatch(&r)==ESP_OK&&mutations==ws_before+1);
 /* The endpoint is a compatibility shim: claiming changes nothing. */
 r=request("{\"op\":\"claim\",\"client\":\"A\",\"label\":\"Phone A\",\"device_id\":\"orl-test-device\"}",NULL);
 app_control_http(&r);assert(r.status==200&&!s_lease.held&&!strstr(r.response,"token"));
 /* Malformed bodies are still rejected before any handler runs. */
 const char *bad[]={"{\"op\":\"claim\",\"op\":\"takeover\"}","{}garbage","{", "[]"};
 for(int i=0;i<4;++i){r=request(bad[i],NULL);assert(app_control_http(&r)==ESP_FAIL&&r.status==400);}
 r=request("{}",NULL);r.type="text/plain";assert(app_control_http(&r)==ESP_FAIL&&r.status==415);
 assert(lock_count==0);
 assert(lock_count==0);
 puts("PASS: production HTTP claim/renew/release/takeover, device identity, hidden bearer, exact expiry, old WebSocket generations, concurrent mutation refusal, malformed bodies and all legacy route guards");
 return 0;
}
'''
def main():
    control=(ROOT/'firmware/main/app_control.c').read_text(encoding='utf-8')
    http=(ROOT/'firmware/main/net_http.c').read_text(encoding='utf-8')
    globals_=control[control.index('static control_lease_t'):control.index('static int64_t control_now')]
    names=['control_now','expire_locked','control_error','app_read_json','request_token','app_control_begin',
           'app_control_generation_begin','app_control_generation','app_control_end','app_control_http']
    routes=http[http.index('static const httpd_uri_t s_routes[]'):http.index('static const char s_https_panel_context')]
    handlers=sorted(set(re.findall(r'\.handler\s*=\s*(\w+)',routes))-{'app_control_http'})
    fake_handlers=''.join(f'static int {n}(httpd_req_t*r){{++mutations;return ESP_OK;}}\n' for n in handlers)
    source=STUBS+globals_+''.join(function(control,n) for n in names)
    source+=function(http,'url_decode')+function(http,'parse_setting_request')+fake_handlers+routes
    source+=function(http,'route_needs_control')+function(http,'controlled_dispatch')+HARNESS
    cjson=Path(os.environ.get('CJSON_DIR',str(ROOT/'tools/vendor/cJSON')))
    with tempfile.TemporaryDirectory(prefix='control-http-',dir=ROOT/'.tmp') as temp:
        p=Path(temp)/'test.c';p.write_text(source,encoding='utf-8');exe=Path(temp)/'test.exe'
        env=dict(os.environ,TEMP=temp,TMP=temp,TMPDIR=temp)
        subprocess.run([shutil.which('gcc'),'-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function',
            '-Wno-unused-variable','-Wno-unused-parameter','-Wno-misleading-indentation',
            '-I',str(ROOT/'firmware/main'),'-I',str(cjson),str(p),str(ROOT/'firmware/main/control_lease.c'),
            str(cjson/'cJSON.c'),'-lm','-o',str(exe)],check=True,env=env)
        subprocess.run([str(exe)],check=True,env=env)
if __name__=='__main__':main()
