"""Exercise the actual AP-only setup-session handler, without network or radio I/O."""
import os, shutil, subprocess, tempfile
from pathlib import Path
from test_control_http import ROOT, STUBS
from test_tx_ownership import function

EXTRA = r'''
#define ESP_ERR_INVALID_ARG 4
#define ESP_ERR_INVALID_STATE 5
#define AF_INET 2
#define AF_INET6 10
#define CONFIG_LWIP_IPV6 1
typedef unsigned socklen_t;
struct sockaddr { int family; };
struct sockaddr_storage { int ss_family; unsigned char space[64]; };
struct sockaddr_in { int sin_family; struct { unsigned s_addr; } sin_addr; };
struct sockaddr_in6 { int sin6_family; struct { unsigned char s6_addr[16]; } sin6_addr; };
static bool ap_on=true,ap_interface=true,socket_error;
static int socket_kind=4;
static int calls,result; static bool released; static char owner[65];
static int httpd_req_to_sockfd(httpd_req_t *r){return 1;}
static int getsockname(int fd,struct sockaddr *out,socklen_t *n){
 if(socket_kind==4){struct sockaddr_in *a=(void*)out;a->sin_family=AF_INET;a->sin_addr.s_addr=ap_interface?1:2;}
 else {struct sockaddr_in6 *a=(void*)out;a->sin6_family=AF_INET6;
  memset(a->sin6_addr.s6_addr,0,16);a->sin6_addr.s6_addr[10]=a->sin6_addr.s6_addr[11]=socket_kind==6?0xff:0;
  unsigned address=ap_interface?1:2;memcpy(a->sin6_addr.s6_addr+12,&address,4);}
 return socket_error?-1:0;
}
static unsigned inet_addr(const char *s){return 1;}
static bool net_wifi_setup_ap_active(void){return ap_on;}
static const char *net_wifi_ap_ip(void){return "192.168.4.1";}
static int net_wifi_setup_session(const char *s,bool release){
 ++calls;released=release;snprintf(owner,sizeof(owner),"%s",s);return result;
}
static const char *esp_err_to_name(int err){return "manager rejected request";}
static int send_json(httpd_req_t *r,const char *s){return httpd_resp_send(r,s,-1);}
'''
HARNESS = r'''
static httpd_req_t req(const char *body){httpd_req_t r={0};r.method=HTTP_POST;r.body=body;
 r.content_len=strlen(body);r.type="application/json";r.device=device_identity_id();r.status=200;return r;}
int main(void){
 const char *renew="{\"op\":\"renew\",\"session\":\"android-A\"}";
 httpd_req_t r=req(renew);h_setup_session(&r);
 assert(r.status==200 && calls==1 && !released && !strcmp(owner,"android-A") && strstr(r.response,"60000"));
 r=req("{\"op\":\"release\",\"session\":\"android-A\"}");h_setup_session(&r);
 assert(r.status==200 && calls==2 && released);
 ap_interface=false;r=req(renew);h_setup_session(&r);assert(r.status==409 && calls==2);ap_interface=true;
 ap_on=false;r=req(renew);h_setup_session(&r);assert(r.status==409 && calls==2);ap_on=true;
 socket_error=true;r=req(renew);h_setup_session(&r);assert(r.status==409 && calls==2);socket_error=false;
 r=req(renew);r.device="another-handset";h_setup_session(&r);assert(r.status==409 && calls==2);
 r=req(renew);r.device=NULL;h_setup_session(&r);assert(r.status==409 && calls==2);
 r=req("{\"op\":\"connect\",\"session\":\"android-A\"}");h_setup_session(&r);assert(r.status==400 && calls==2);
 r=req("{\"op\":\"renew\",\"session\":123}");h_setup_session(&r);assert(r.status==400 && calls==2);
 r=req("{}");h_setup_session(&r);assert(r.status==400 && calls==2);
 r=req("{\"op\":\"renew\",\"op\":\"release\"}");h_setup_session(&r);assert(r.status==400 && calls==2);
 result=ESP_ERR_INVALID_STATE;r=req(renew);h_setup_session(&r);assert(r.status==409 && calls==3);
 result=ESP_ERR_INVALID_ARG;r=req(renew);h_setup_session(&r);assert(r.status==400 && calls==4);
 result=ESP_OK;socket_kind=6;r=req(renew);h_setup_session(&r);assert(r.status==200 && calls==5);
 ap_interface=false;r=req(renew);h_setup_session(&r);assert(r.status==409 && calls==5);ap_interface=true;
 socket_kind=7;r=req(renew);h_setup_session(&r);assert(r.status==409 && calls==5);
 puts("PASS: IPv4 and dual-stack AP interface, handset identity, renew/release, malformed JSON, manager rejection; no radio control lease required");
}
'''

def main():
    control=(ROOT/'firmware/main/app_control.c').read_text(encoding='utf-8')
    http=(ROOT/'firmware/main/net_http.c').read_text(encoding='utf-8')
    source=STUBS+EXTRA+function(control,'control_error')+function(control,'app_read_json')
    source+=function(http,'setup_local_address')+function(http,'h_setup_session')+HARNESS
    cjson=Path(os.environ.get('CJSON_DIR',str(ROOT/'tools/vendor/cJSON')))
    with tempfile.TemporaryDirectory(prefix='setup-http-',dir=ROOT/'.tmp') as tmp:
        p=Path(tmp)/'test.c';p.write_text(source,encoding='utf-8');exe=Path(tmp)/'test.exe'
        env=dict(os.environ,TEMP=tmp,TMP=tmp,TMPDIR=tmp)
        subprocess.run([shutil.which('gcc'),'-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function',
            '-Wno-unused-variable','-Wno-unused-parameter','-Wno-misleading-indentation',
            '-I',str(cjson),'-I',str(ROOT/'firmware/main'),str(p),str(cjson/'cJSON.c'),'-lm','-o',str(exe)],check=True,env=env)
        subprocess.run([str(exe)],check=True,env=env)

if __name__=='__main__':main()
