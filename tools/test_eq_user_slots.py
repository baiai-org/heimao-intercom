"""Exercise the production EQ write path and slot HTTP handler without a handset."""
import os
from pathlib import Path
import subprocess
import tempfile

from test_control_http import STUBS
from test_device_name import no_includes
from test_tx_ownership import function

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "firmware/main"

STORAGE = r'''
#include <ctype.h>
#include <math.h>
#include "eq6.h"
#define ESP_ERR_INVALID_ARG 3
#define ESP_ERR_INVALID_STATE 4
#define ESP_ERR_NVS_NOT_FOUND 6
#define NVS_NS_VOICE "orl"
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define AUDIO_SAMPLE_RATE_HZ 16000
#define DEVICE_NAME_BYTES 96
#define HTTPD_500_INTERNAL_SERVER_ERROR 500
typedef int nvs_handle_t;
static struct { uint8_t bytes[512]; size_t size; } durable[2], pending[2];
static bool fail_commit;
static int write_key, commits;
static int nvs_open(const char *space, int mode, int *h) { assert(!strcmp(space,"orl")); *h=1; return 0; }
static int nvs_get_blob(int h,const char *key,void *out,size_t *size) {
    int n=strcmp(key,"veq")!=0;
    if(!durable[n].size)return ESP_ERR_NVS_NOT_FOUND;
    assert(*size>=durable[n].size);
    *size=durable[n].size;memcpy(out,durable[n].bytes,*size);return 0;
}
static int nvs_set_blob(int h,const char *key,const void *data,size_t size) {
    write_key=strcmp(key,"veq")!=0;
    assert(size<=sizeof(pending[write_key].bytes));
    pending[write_key].size=size;memcpy(pending[write_key].bytes,data,size);return 0;
}
static int nvs_commit(int h) {
    if(fail_commit)return ESP_FAIL;
    durable[write_key]=pending[write_key];++commits;return ESP_OK;
}
static void nvs_close(int h) {}
static void control_lock(void) {}
static void control_unlock(void) {}
static int s_eq_mux;
#define portENTER_CRITICAL(a) ((void)(a))
#define portEXIT_CRITICAL(a) ((void)(a))
#define ESP_LOGI(...) ((void)0)
static eq6_t s_eq_config[2]={{.flat=true},{.flat=true}};
void voice_eq_snapshot(int db[2][6]);
esp_err_t voice_eq_replace(const int expected[2][6],const int next[2][6]);
static const char *radio_activity_boot_id(void) {return "1234abcd";}
static int httpd_resp_send_err(httpd_req_t *r,int code,const char *message) {r->status=code;return ESP_FAIL;}
'''
CHECKS = r'''
#define ROOT_ARGS "\"boot_id\":\"1234abcd\""
#define SPK "[1,2,3,4,5,6]"
#define MIC "[-1,-2,-3,-4,-5,-6]"
#define FLAT "[0,0,0,0,0,0]"
static httpd_req_t request(const char *data) {
    return (httpd_req_t){.method=data?HTTP_POST:HTTP_GET,.body=data,.content_len=data?strlen(data):0,
        .type="application/json",.device=device_identity_id(),.status=200};
}
static int post(const char *json) {httpd_req_t r=request(json);eq_presets_http(&r);return r.status;}
int main(void) {
    const char *put="{\"op\":\"save\",\"slot\":2,\"revision\":0," ROOT_ARGS ",\"name\":\"  车载  \",\"spk\":" SPK ",\"mic\":" MIC "}";
    const char *apply="{\"op\":\"apply\",\"slot\":2,\"revision\":1," ROOT_ARGS ",\"expected_spk\":" FLAT ",\"expected_mic\":" FLAT "}";
    const char *rename="{\"op\":\"rename\",\"slot\":2,\"revision\":1," ROOT_ARGS ",\"name\":\"夜间\"}";
    httpd_req_t r=request(put);
    r.device="different";eq_presets_http(&r);assert(r.status==409 && commits==0);
    fail_commit=true;assert(post(put)==503&&commits==0);fail_commit=false;
    assert(post(put)==200&&commits==1);assert(post(put)==409&&commits==1);
    eq_store_t record; assert(load(&record)==0&&record.revision==1);
    assert(record.slots[0].name[0]==0&&record.slots[2].name[0]==0);
    assert(!strcmp(record.slots[1].name,"车载")&&record.slots[1].db[1][5]==-6);
    int active[2][6];voice_eq_snapshot(active);assert(active[0][5]==0&&active[1][5]==0);
    fail_commit=true;assert(post(apply)==503&&commits==1);
    voice_eq_snapshot(active);assert(active[0][5]==0&&active[1][5]==0);
    fail_commit=false;assert(post(apply)==200&&commits==2);
    voice_eq_snapshot(active);assert(active[0][5]==6&&active[1][5]==-6);
    eq6_t audio={.flat=true}; eq_sync(&audio,1); assert(audio.db[5]==-6&&!audio.flat);
    memset(s_eq_config,0,sizeof(s_eq_config));voice_eq_load();
    voice_eq_snapshot(active);assert(active[0][5]==6&&active[1][5]==-6);
    assert(post(apply)==409&&commits==2);
    assert(post(rename)==200&&commits==3);
    assert(load(&record)==0&&!strcmp(record.slots[1].name,"夜间")&&record.slots[1].db[0][5]==6);
    assert(post("{\"op\":\"delete\",\"slot\":2,\"revision\":2," ROOT_ARGS "}") == 200);
    assert(load(&record)==0&&!record.slots[1].name[0]&&record.revision==3);
    voice_eq_snapshot(active);assert(active[0][5]==6&&active[1][5]==-6);
    assert(post("{\"op\":\"save\",\"slot\":1,\"revision\":3," ROOT_ARGS ",\"name\":\"bad\\u0000extra\",\"spk\":" SPK ",\"mic\":" MIC "}") == 400);
    r=request(NULL);assert(eq_presets_http(&r)==0);
    cJSON *response=cJSON_Parse(r.response);assert(response);
    assert(cJSON_GetArraySize(cJSON_GetObjectItem(response,"slots"))==3);cJSON_Delete(response);
    const int8_t legacy[6]={1,2,3,-4,-5,-6};
    memcpy(durable[0].bytes,legacy,sizeof(legacy));durable[0].size=sizeof(legacy);voice_eq_load();
    voice_eq_snapshot(active);assert(active[0][2]==3 && active[1][0]==-4 && active[1][2]==-6 && active[1][3]==0);
    puts("PASS: EQ fixed slots, UTF-8, identity, revision and active-curve conflicts, commit failure, paired persistence and legacy EQ");
}
'''

def main():
    voice = (MAIN / "voice.c").read_text(encoding="utf-8")
    control = (MAIN / "app_control.c").read_text(encoding="utf-8")
    storage = voice[voice.index('#define NVS_KEY_EQ   "veq"'):voice.index("/*\n * Codec DAC volume")]
    test = STUBS + STORAGE + function(control, "control_error") + function(control, "app_read_json")
    test += function((MAIN / "device_name.c").read_text(encoding="utf-8"), "device_name_validate")
    test += function(voice, "eq_sync") + storage
    test += "\n" + no_includes((MAIN / "eq6.c").read_text(encoding="utf-8"))
    test += "\n" + no_includes((MAIN / "eq_presets.c").read_text(encoding="utf-8")) + CHECKS
    cjson = ROOT / "tools/vendor/cJSON"
    with tempfile.TemporaryDirectory(prefix="eq-slots-", dir=ROOT / ".tmp") as td:
        tmp = Path(td)
        source, exe = tmp / "test.c", tmp / "test.exe"
        source.write_text(test, encoding="utf-8")
        env = dict(os.environ, TEMP=td, TMP=td, TMPDIR=td)
        subprocess.run(["gcc", "-std=c11", "-I", str(MAIN), "-I", str(cjson), str(source),
                        str(cjson / "cJSON.c"), "-lm", "-o", str(exe)], env=env, check=True)
        subprocess.run([str(exe)], env=env, check=True)

if __name__ == "__main__":
    main()
