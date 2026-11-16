"""Real channel temporary-file recovery and relay idle collector, offline."""
from pathlib import Path
import os,shutil,subprocess,tempfile
from test_np_peer_runtime import function
ROOT=Path(__file__).resolve().parents[1];MAIN=ROOT/'firmware/main'
BASE=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
'''
CHANNEL_ENV=r'''
#define CH_PATH "chans.bin"
#define CH_TMP "chans.tmp"
#define CH_MAGIC 0x4F524C43u
#define CH_VERSION 1u
#define CHANNELS_MAX 99
#define CHANNEL_NAME_LEN 24
typedef struct{char name[24];unsigned char cfg[56];}channel_blob_t;
typedef struct{uint32_t magic,version,count,reserved;}chans_hdr_t;
static channel_blob_t rows[99],*s_all=rows;static int s_count,writers;static bool deny_write,rename_failure;
static bool spiffs_up(void){return true;}
static bool device_maintenance_write_begin(void){if(deny_write)return false;++writers;return true;}
static void device_maintenance_write_end(void){assert(writers==1);--writers;}
static int checked_rename(const char*a,const char*b){return rename_failure?-1:rename(a,b);}
#define rename checked_rename
static bool exists(const char*p){FILE*f=fopen(p,"rb");if(!f)return false;fclose(f);return true;}
static void table(const char*p,const char*name,bool short_file){FILE*f=fopen(p,"wb");assert(f);chans_hdr_t h={CH_MAGIC,1,1,0};channel_blob_t r={0};strcpy(r.name,name);fwrite(&h,sizeof(h),1,f);if(!short_file)fwrite(&r,sizeof(r),1,f);assert(!fclose(f));}
'''
CHANNEL_TEST=r'''
int main(void){
 table(CH_PATH,"committed",false);table(CH_TMP,"uncommitted",false);assert(store_load()&&!strcmp(s_all[0].name,"committed")&&!exists(CH_TMP));
 remove(CH_PATH);table(CH_TMP,"recovered",false);assert(store_load()&&exists(CH_PATH)&&!exists(CH_TMP)&&!strcmp(s_all[0].name,"recovered"));
 table(CH_PATH,"short",true);table(CH_TMP,"only good copy",false);rename_failure=true;assert(store_load()&&exists(CH_TMP)&&!strcmp(s_all[0].name,"only good copy"));
 rename_failure=false;assert(store_load()&&exists(CH_PATH)&&!exists(CH_TMP));
 table(CH_PATH,"bad",true);table(CH_TMP,"also bad",true);assert(!store_load()&&exists(CH_PATH)&&exists(CH_TMP));
 strcpy(s_all[0].name,"new setting");s_count=1;deny_write=true;assert(!store_write()&&writers==0);assert(!store_load_path(CH_PATH));
 deny_write=false;assert(store_write()&&writers==0);assert(store_load_path(CH_PATH)&&!strcmp(s_all[0].name,"new setting"));
 puts("PASS storage: valid committed file wins, interrupted-save recovery, failed promotion retains only good copy, corrupt files retained, reboot write gate, balanced writer lifetime");return 0;
}
'''
RELAY_ENV=r'''
#include "net_rx_order.h"
#define REL_RX_PTT_TIMEOUT_MS 2000
static bool locked,s_jbuf_valid[128],s_have_latest,s_have_first,s_play_head_valid;
static uint16_t s_latest,s_play_head;static int64_t s_last_pkt_ms;
typedef int opus_link_dec_t;static int decoder,freed;static opus_link_dec_t*s_opus_dec;
static uint8_t s_opus_dec_mac[6];
typedef struct{net_rx_order_t order;}rel_rx_source_t;static rel_rx_source_t source,*s_stream_source=&source;
static void rx_lock(void){assert(!locked);locked=true;}
static void rx_unlock(void){assert(locked);locked=false;}
static void opus_link_dec_free(opus_link_dec_t*p){assert(locked);if(p){assert(p==&decoder);++freed;}}
'''
RELAY_TEST=r'''
int main(void){
 s_last_pkt_ms=100;s_opus_dec=&decoder;source.order=(net_rx_order_t){.seen=true,.epoch=10,.latest=99,.last_ms=100};net_rx_order_t keep=source.order;
 assert(!netrelay_cleanup_idle(60099,60000)&&s_opus_dec);
 s_have_latest=true;s_latest=99;s_play_head_valid=false;assert(!netrelay_cleanup_idle(100000,60000)&&s_opus_dec);
 s_play_head_valid=true;s_play_head=100;assert(netrelay_cleanup_idle(100000,60000)==1&&!s_opus_dec&&!s_have_latest&&!locked);
 assert(!memcmp(&keep,&source.order,sizeof(keep))&&freed==1);
 for(int i=0;i<1000;i++)assert(!netrelay_cleanup_idle(100000+i,60000));assert(freed==1);
 puts("PASS relay: no cleanup before idle timeout or with pending PCM; decoder released once; replay history and stream ownership preserved");return 0;
}
'''
def run(name,code):
    (ROOT/'.tmp').mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=name+'-',dir=ROOT/'.tmp') as tmp:
        p=Path(tmp);(p/'test.c').write_text(code,encoding='utf-8');env=dict(os.environ,TEMP=tmp,TMP=tmp,TMPDIR=tmp)
        subprocess.run([shutil.which('gcc'),'-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-misleading-indentation','-I',str(MAIN),str(p/'test.c'),'-o',str(p/'test.exe')],check=True,env=env)
        subprocess.run([str(p/'test.exe')],cwd=tmp,check=True,env=env)
def main():
    channels=(MAIN/'channels.c').read_text(encoding='utf-8');relay=(MAIN/'netrelay.c').read_text(encoding='utf-8')
    run('channel-recovery',BASE+CHANNEL_ENV+''.join(function(channels,n) for n in ('store_load_path','store_load','store_write_inner','store_write'))+CHANNEL_TEST)
    run('relay-maintenance',BASE+RELAY_ENV+''.join(function(relay,n) for n in ('relay_reset_stream','netrelay_cleanup_idle'))+RELAY_TEST)
if __name__=='__main__':main()
