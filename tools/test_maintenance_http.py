"""Real new JSON handlers: identity, stale requests, strict parsing and partial success."""
from pathlib import Path
import os,shutil,subprocess,tempfile
from test_control_http import STUBS
from test_np_peer_runtime import function
ROOT=Path(__file__).resolve().parents[1];MAIN=ROOT/'firmware/main'
def plain(name):return '\n'+'\n'.join(x for x in (MAIN/name).read_text(encoding='utf-8').splitlines() if not x.startswith('#include') and x!='#pragma once')+'\n'
ENV=r'''
#include <math.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif
#define ESP_ERR_INVALID_ARG 3
#define ESP_ERR_INVALID_STATE 4
#define ESP_ERR_NOT_FOUND 5
#define ESP_ERR_NOT_SUPPORTED 6
#define HTTPD_500_INTERNAL_SERVER_ERROR 500
static int changes,reboots,factory_resets,cleanups,renames,adds;static int operation_error;static bool added_partial;
static int httpd_resp_send_err(httpd_req_t*r,int status,const char*t){r->status=status;return httpd_resp_send(r,t,-1);}
static const char*radio_activity_boot_id(void){return "boot-1";}
static void device_maintenance_json(char*out,size_t n){snprintf(out,n,"{\"version\":2,\"ready\":true,\"revision\":0}");}
static int device_maintenance_set(const maintenance_config_t*c,uint32_t revision){assert(c->version==2&&c->restart_minute<1440&&c->restart_days<=127);(void)revision;++changes;return operation_error;}
static int device_maintenance_cleanup(void){++cleanups;return operation_error;}
static int device_maintenance_reboot(void){++reboots;return operation_error;}
static int device_maintenance_factory_reset(void){++factory_resets;return operation_error;}
static uint32_t peer_labels_revision(void){return 7;}
static int netp2pnet_peer_rename(const char*id,const char*label,uint32_t rev){assert(!strcmp(id,"m-001122334455"));assert(strlen(label)<=96);(void)rev;++renames;return operation_error;}
static int netp2pnet_peer_add_named(const char*label,const char*ip,uint16_t port,bool restore,uint32_t rev,bool*added){(void)label;(void)ip;(void)port;(void)restore;(void)rev;++adds;*added=added_partial;return operation_error;}
'''
TEST=r'''
static httpd_req_t req(const char*body){httpd_req_t r={0};r.method=body?HTTP_POST:HTTP_GET;r.body=body;r.content_len=body?strlen(body):0;r.type="application/json";r.device=device_identity_id();r.status=200;return r;}
int main(void){
 httpd_req_t r=req(NULL);assert(maintenance_http(&r)==0);cJSON*j=cJSON_Parse(r.response);assert(!strcmp(cJSON_GetObjectItem(j,"device_id")->valuestring,device_identity_id()));assert(!strcmp(cJSON_GetObjectItem(j,"boot_id")->valuestring,"boot-1"));cJSON_Delete(j);
 r=req("{\"op\":\"set\",\"revision\":0,\"restart_days\":21,\"restart_minute\":270,\"cleanup_enabled\":true,\"restart_enabled\":false}");r.device="other";maintenance_http(&r);assert(r.status==409&&!changes);
 r=req("{\"op\":\"set\",\"revision\":0,\"restart_days\":21,\"restart_minute\":270,\"cleanup_enabled\":true,\"restart_enabled\":false}");maintenance_http(&r);assert(r.status==200&&changes==1);
 const char*bad[]={"{\"op\":\"set\",\"revision\":0,\"restart_days\":128,\"restart_minute\":270,\"cleanup_enabled\":true,\"restart_enabled\":false}",
 "{\"op\":\"set\",\"revision\":0,\"restart_days\":21,\"restart_minute\":1440,\"cleanup_enabled\":true,\"restart_enabled\":false}",
 "{\"op\":\"set\",\"revision\":0.5,\"restart_days\":21,\"restart_minute\":270,\"cleanup_enabled\":true,\"restart_enabled\":false}",
 "{\"op\":\"set\",\"revision\":0,\"restart_days\":21,\"restart_minute\":270,\"cleanup_enabled\":1,\"restart_enabled\":false}",
 "{\"op\":\"set\",\"revision\":0,\"restart_days\":21,\"restart_minute\":270.5,\"cleanup_enabled\":true,\"restart_enabled\":false}",
 "{\"op\":\"set\",\"revision\":0,\"restart_days\":0,\"restart_minute\":270,\"cleanup_enabled\":true,\"restart_enabled\":true}",
 "{\"op\":\"set\",\"revision\":0,\"restart_hours\":24,\"cleanup_enabled\":true,\"restart_enabled\":true}"};
 for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++){r=req(bad[i]);maintenance_http(&r);assert(r.status==400&&changes==1);}
 r=req("{\"op\":\"reboot\",\"boot_id\":\"old-boot\"}");maintenance_http(&r);assert(r.status==409&&!reboots);
 r=req("{\"op\":\"reboot\",\"boot_id\":\"boot-1\"}");maintenance_http(&r);assert(r.status==200&&reboots==1);
 r=req("{\"op\":\"factory_reset\",\"boot_id\":\"boot-1\"}");r.device="other";maintenance_http(&r);assert(r.status==409&&!factory_resets);
 r=req("{\"op\":\"factory_reset\",\"boot_id\":\"old-boot\"}");maintenance_http(&r);assert(r.status==409&&!factory_resets);
 operation_error=ESP_ERR_NOT_SUPPORTED;r=req("{\"op\":\"factory_reset\",\"boot_id\":\"boot-1\"}");maintenance_http(&r);assert(r.status==503&&factory_resets==1);
 operation_error=0;r=req("{\"op\":\"factory_reset\",\"boot_id\":\"boot-1\"}");maintenance_http(&r);assert(r.status==200&&factory_resets==2);
 operation_error=ESP_ERR_INVALID_STATE;r=req("{\"op\":\"cleanup\"}");maintenance_http(&r);assert(r.status==409&&cleanups==1);operation_error=0;
 char ip[64];uint16_t port;assert(endpoint_parse("[2001:db8::1]:65535",ip,&port)&&port==65535);assert(endpoint_parse("2001:db8::1",ip,&port)&&port==54321);
 assert(endpoint_parse("192.0.2.1:54321",ip,&port)&&port==54321);
 const char*badaddr[]={"192.0.2.1:0","192.0.2.1:65536","192.0.2.1:-1","192.0.2.1:1x","192.0.2.1:","[2001:db8::1]extra","bad", "[::1]:99999999999999999999999"};
 for(unsigned i=0;i<sizeof(badaddr)/sizeof(*badaddr);i++)assert(!endpoint_parse(badaddr[i],ip,&port));
 r=req("{\"op\":\"rename\",\"id\":\"m-001122334455\",\"name\":\"山顶中继\",\"revision\":7}");peer_labels_http(&r);assert(r.status==200&&renames==1);
 r=req("{\"op\":\"rename\",\"id\":\"m-001122334455\",\"name\":\"bad\\u0000name\",\"revision\":7}");assert(peer_labels_http(&r)==ESP_FAIL&&r.status==400&&renames==1);
 r=req("{\"op\":\"rename\",\"op\":\"add\",\"name\":\"x\",\"revision\":7}");assert(peer_labels_http(&r)==ESP_FAIL&&r.status==400);
 added_partial=true;operation_error=77;r=req("{\"op\":\"add\",\"name\":\"<山顶>\\\"\\\\\",\"address\":\"[2001:db8::1]:54321\",\"restore\":true,\"revision\":7}");peer_labels_http(&r);assert(r.status==503&&adds==1);j=cJSON_Parse(r.response);assert(cJSON_IsFalse(cJSON_GetObjectItem(j,"ok"))&&cJSON_IsTrue(cJSON_GetObjectItem(j,"peer_added")));cJSON_Delete(j);
 puts("PASS HTTP: identity and boot guards, malformed/duplicate/NUL JSON, exact numeric/boolean bounds, IPv4/IPv6 ports, safe JSON and honest partial add failure");return 0;
}
'''
def main():
    control=(MAIN/'app_control.c').read_text(encoding='utf-8');device=(MAIN/'device_name.c').read_text(encoding='utf-8')
    code=STUBS+plain('maintenance_policy.h')+ENV+function(device,'device_name_validate').replace('DEVICE_NAME_BYTES','96')
    code+=function(control,'control_error')+function(control,'app_read_json')+plain('maintenance_http.c')+TEST
    cjson=ROOT/'tools/vendor/cJSON';(ROOT/'.tmp').mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='maintenance-http-',dir=ROOT/'.tmp') as tmp:
        p=Path(tmp);(p/'test.c').write_text(code,encoding='utf-8');env=dict(os.environ,TEMP=tmp,TMP=tmp,TMPDIR=tmp)
        cmd=[shutil.which('gcc'),'-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable','-Wno-unused-parameter','-Wno-misleading-indentation','-I',str(MAIN),'-I',str(cjson),str(p/'test.c'),str(cjson/'cJSON.c'),'-lm','-o',str(p/'test.exe')]
        if os.name=='nt':cmd+=['-lws2_32']
        subprocess.run(cmd,env=env,check=True);subprocess.run([str(p/'test.exe')],env=env,check=True)
if __name__=='__main__':main()
