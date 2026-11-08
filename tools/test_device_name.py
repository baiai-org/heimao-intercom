"""Run production UTF-8 validation, NVS transaction and name HTTP handler on host."""
import os, shutil, subprocess, tempfile
from pathlib import Path
from test_tx_ownership import function
from test_control_http import STUBS
ROOT=Path(__file__).resolve().parent.parent
ENV=r'''
#include <math.h>
#define ESP_ERR_INVALID_ARG 3
#define ESP_ERR_INVALID_STATE 4
#define ESP_ERR_NO_MEM 5
#define ESP_ERR_NVS_NOT_FOUND 6
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define portMAX_DELAY 0
typedef int nvs_handle_t;
static unsigned char durable[200],pending[200];static size_t durable_size,pending_size;
static bool fail_open,fail_set,fail_commit;
static int saves,mdns_updates;
static int xSemaphoreCreateMutexStatic(StaticSemaphore_t*p){return 1;}
static int xSemaphoreTake(int h,int ms){assert(h);return 1;}
static void xSemaphoreGive(int h){assert(h);}
static int nvs_open(const char*ns,int mode,int*h){assert(!strcmp(ns,"hm_identity"));if(fail_open)return 50;if(!durable_size&&mode==0)return ESP_ERR_NVS_NOT_FOUND;*h=1;return 0;}
static int nvs_get_blob(int h,const char*k,void*out,size_t*n){if(!durable_size)return ESP_ERR_NVS_NOT_FOUND;assert(*n>=durable_size);memcpy(out,durable,durable_size);*n=durable_size;return 0;}
static int nvs_set_blob(int h,const char*k,const void*value,size_t n){if(fail_set)return 51;memcpy(pending,value,n);pending_size=n;return 0;}
static int nvs_commit(int h){if(fail_commit)return 52;memcpy(durable,pending,pending_size);durable_size=pending_size;++saves;return 0;}
static void nvs_close(int h){}
static void net_wifi_name_changed(void){++mdns_updates;}
static const char*esp_err_to_name(int e){return "storage-error";}
static int send_json(httpd_req_t*r,const char*s){return httpd_resp_send(r,s,-1);}
'''
HARNESS=r'''
static void reboot(void){s_lock=0;s_storage=0;s_name=(device_name_t){.format=1};assert(device_name_init()==ESP_OK);}
static httpd_req_t request(const char*body){httpd_req_t r={0};r.method=HTTP_POST;r.body=body;r.content_len=strlen(body);r.type="application/json";r.status=200;return r;}
int main(void){
 assert(device_name_validate("车载主台"));assert(device_name_validate(""));
 assert(!device_name_validate("bad\nname"));assert(!device_name_validate("bad\xc2\x85name"));
 assert(!device_name_validate("\xc0\x80"));assert(!device_name_validate("\xed\xa0\x80"));
 assert(!device_name_validate("\xf4\x90\x80\x80"));assert(!device_name_validate("\xf0\x9f"));
 char edge[101]={0};for(int i=0;i<24;i++)memcpy(edge+4*i,"\xf0\x9f\x90\xb1",4);
 assert(strlen(edge)==96&&device_name_validate(edge));strcat(edge,"x");assert(!device_name_validate(edge));
 reboot();device_name_t out;device_name_get(&out);assert(out.revision==0&&!out.name[0]);
 assert(device_name_set("车载主台",0,&out)==0&&out.revision==1);reboot();device_name_get(&out);assert(!strcmp(out.name,"车载主台"));
 assert(device_name_set("changed",0,&out)==ESP_ERR_INVALID_STATE&&!strcmp(out.name,"车载主台"));
 int before=saves;assert(device_name_set("车载主台",1,&out)==0&&saves==before);
 fail_open=true;assert(device_name_set("no",1,&out)!=0&&out.revision==1);fail_open=false;
 fail_set=true;assert(device_name_set("no",1,&out)!=0&&out.revision==1);fail_set=false;
 fail_commit=true;assert(device_name_set("no",1,&out)!=0&&out.revision==1);fail_commit=false;
 reboot();device_name_get(&out);assert(out.revision==1&&!strcmp(out.name,"车载主台"));
 httpd_req_t r=request("{\"custom_name\":\"  <台> \\\" & \\\\  \",\"name_revision\":1}");
 assert(h_device_name(&r)==0&&r.status==200&&mdns_updates==1);
 cJSON*o=cJSON_Parse(r.response);assert(o);assert(!strcmp(cJSON_GetObjectItem(o,"custom_name")->valuestring,"<台> \" & \\"));cJSON_Delete(o);
 r=request("{\"custom_name\":\"next\",\"name_revision\":1}");h_device_name(&r);assert(r.status==409&&mdns_updates==1);
 r=request("{\"custom_name\":\"bad\\u0000name\",\"name_revision\":2}");assert(h_device_name(&r)==ESP_FAIL&&r.status==400);
 r=request("{\"custom_name\":\"bad\\nname\",\"name_revision\":2}");h_device_name(&r);assert(r.status==400);
 r=request("{\"custom_name\":\"\",\"name_revision\":2}");h_device_name(&r);assert(r.status==200);reboot();device_name_get(&out);assert(out.revision==3&&!out.name[0]);
 r=request("{\"custom_name\":\"a\",\"custom_name\":\"b\",\"name_revision\":3}");assert(h_device_name(&r)==ESP_FAIL&&r.status==400);
 r=request("{\"custom_name\":\"a\",\"name_revision\":3.5}");h_device_name(&r);assert(r.status==400);
 puts("PASS: UTF-8 boundaries, exact byte/character limits, durable reboot, NVS open/write/commit failures, revision conflicts, HTTP canonical readback/escaping, clear, duplicate keys and embedded NUL");
 return 0;
}
'''
def no_includes(s):return '\n'.join(l for l in s.splitlines() if not l.startswith('#include') and l!='#pragma once')
def main():
    main=ROOT/'firmware/main';http=(main/'net_http.c').read_text(encoding='utf-8');control=(main/'app_control.c').read_text(encoding='utf-8')
    source=STUBS+ENV+no_includes((main/'device_name.h').read_text())+no_includes((main/'device_name.c').read_text())
    source+=function(control,'control_error')+function(control,'app_read_json')
    source+=function(http,'json_escape_str')+function(http,'h_device_name')+HARNESS
    cjson=Path(os.environ.get('CJSON_DIR',str(ROOT/'tools/vendor/cJSON')))
    env=dict(os.environ);env['PATH']=r'C:\Program Files\mingw64\bin;'+env['PATH']
    with tempfile.TemporaryDirectory(prefix='device-name-',dir=ROOT/'.tmp') as tmp:
        p=Path(tmp)/'test.c';p.write_text(source,encoding='utf-8');exe=Path(tmp)/'test.exe'
        env.update(TEMP=tmp,TMP=tmp,TMPDIR=tmp)
        subprocess.run([shutil.which('gcc',path=env['PATH']),'-std=c11','-Wall','-Wextra','-Werror',
            '-Wno-unused-function','-Wno-unused-variable','-Wno-unused-parameter','-Wno-misleading-indentation',
            '-I',str(main),'-I',str(cjson),str(p),str(cjson/'cJSON.c'),'-lm','-o',str(exe)],check=True,env=env)
        subprocess.run([str(exe)],check=True,env=env)
if __name__=='__main__':main()
