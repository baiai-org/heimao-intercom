"""Production MP3 worker lifetime with RTOS/codec substitutes; no Flash or RF."""
from pathlib import Path
import os, subprocess, tempfile
from test_np_peer_runtime import function

ROOT=Path(__file__).resolve().parents[1]

PREFIX=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_NO_MEM 2
#define ESP_ERR_INVALID_STATE 3
#define ESP_ERR_TIMEOUT 4
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define RATE 16000
#define TAIL_CUSTOM_MAX_SAMPLES 80000
#define CUSTOM_MAX_STORED 80000
#define MINIMP3_MAX_SAMPLES_PER_FRAME 1152
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define pdTRUE 1
#define pdPASS 1
#define pdMS_TO_TICKS(x) (x)
typedef int esp_err_t;
typedef struct { int value; } *SemaphoreHandle_t;
typedef void *TaskHandle_t;
typedef struct { int dummy; } mp3dec_t;
typedef struct { int bitrate_kbps,hz,channels,frame_bytes; } mp3dec_frame_info_t;
static unsigned alive,allocations,fail_alloc,stores,worker_deletes,created;
static bool in_worker,create_fail,timeout_wait,early_finish,store_fail;
static atomic_bool s_storing;
static void (*pending_fn)(void *);
static void *pending_arg,*worker_stack;
static void *tracked_malloc(size_t size) {
 if(++allocations==fail_alloc)return NULL;
 size_t *p=malloc(size+sizeof(size_t));if(!p)return NULL;*p=0x9988;alive++;return p+1;
}
static void tracked_free(void *ptr) { if(!ptr)return;size_t *p=(size_t*)ptr-1;assert(*p==0x9988);*p=0;alive--;free(p); }
static void *tracked_calloc(size_t n,size_t s) { void *p=tracked_malloc(n*s);if(p)memset(p,0,n*s);return p; }
#define malloc tracked_malloc
#define calloc tracked_calloc
#define free tracked_free
static void *heap_caps_malloc(size_t n,unsigned caps) { assert(caps&MALLOC_CAP_SPIRAM);return malloc(n); }
static SemaphoreHandle_t xSemaphoreCreateBinary(void) { return calloc(1,sizeof(struct {int value;})); }
static void vSemaphoreDelete(SemaphoreHandle_t s) { free(s); }
static int xSemaphoreGive(SemaphoreHandle_t s) { s->value++;return pdTRUE; }
static void finish_worker(void) { void (*fn)(void*)=pending_fn;void *arg=pending_arg;pending_fn=NULL;pending_arg=NULL;in_worker=true;fn(arg);in_worker=false; }
static int xTaskCreateWithCaps(void(*fn)(void*),const char *name,int stack,void *arg,int priority,TaskHandle_t *task,unsigned caps) {
 (void)name;(void)priority;assert(stack==24576&&(caps&MALLOC_CAP_SPIRAM));
 if(create_fail)return 0;
 worker_stack=heap_caps_malloc((size_t)stack,caps);if(!worker_stack)return 0;
 pending_fn=fn;pending_arg=arg;*task=(void*)1;created++;if(early_finish)finish_worker();return pdPASS;
}
static void vTaskDeleteWithCaps(TaskHandle_t task) { (void)task;assert(in_worker);free(worker_stack);worker_stack=NULL;worker_deletes++; }
static int xSemaphoreTake(SemaphoreHandle_t s,int ms) { assert(ms==30000);if(timeout_wait)return 0;if(pending_fn)finish_worker();assert(s->value);s->value--;return pdTRUE; }
static void mp3dec_init(mp3dec_t *dec) { (void)dec; }
static int mp3dec_decode_frame(mp3dec_t *d,const uint8_t *src,int bytes,int16_t *frame,mp3dec_frame_info_t *info) {
 (void)d;(void)src;assert(in_worker&&bytes>=4);*info=(mp3dec_frame_info_t){128,16000,1,4};
 for(int i=0;i<1152;i++)frame[i]=100;return 1152;
}
static esp_err_t custom_store_finish(int16_t *pcm,size_t samples) { assert(!in_worker&&samples>=RATE/5);stores++;free(pcm);return store_fail?ESP_ERR_INVALID_STATE:ESP_OK; }
static esp_err_t wav_store(const uint8_t *data,size_t n) { (void)data;(void)n;assert(!in_worker);stores++;return ESP_OK; }
'''

TEST=r'''
static void clean(void) { assert(alive==0&&!atomic_load(&s_storing)&&!pending_fn&&!worker_stack); }
int main(void) {
 uint8_t mp3[40]={0xff,0xe1};
 assert(tail_custom_store(mp3,sizeof(mp3))==ESP_OK);clean();assert(stores==1&&worker_deletes==1);
 early_finish=true;assert(tail_custom_store(mp3,sizeof(mp3))==ESP_OK);clean();early_finish=false;
 store_fail=true;assert(tail_custom_store(mp3,sizeof(mp3))==ESP_ERR_INVALID_STATE);clean();store_fail=false;
 unsigned before=stores;timeout_wait=true;
 assert(tail_custom_store(mp3,sizeof(mp3))==ESP_ERR_TIMEOUT);
 assert(atomic_load(&s_storing)&&pending_fn&&alive);
 assert(tail_custom_store(mp3,sizeof(mp3))==ESP_ERR_INVALID_STATE);
 finish_worker();clean();assert(stores==before);timeout_wait=false;
 create_fail=true;assert(tail_custom_store(mp3,sizeof(mp3))==ESP_ERR_NO_MEM);clean();create_fail=false;
 for(unsigned i=1;i<=8;i++) {
  allocations=0;fail_alloc=i;esp_err_t err=tail_custom_store(mp3,sizeof(mp3));
  assert(err!=ESP_OK);clean();
 }
 fail_alloc=0;allocations=0;
 assert(tail_custom_store(mp3,sizeof(mp3))==ESP_OK);clean();
 puts("PASS: PSRAM worker stack; early/normal completion; caller-only persistence; timeout cancellation and no background write; all allocation failures reclaimed");
 return 0;
}
'''

def main():
    source=(ROOT/'firmware/main/voice_tail.c').read_text('utf-8')
    start=source.index('typedef struct {\n    uint8_t *data;')
    end=source.index('\nesp_err_t tail_custom_store(',start)
    code=PREFIX+function(source,'mp3_frame_samples')+function(source,'resample_mono')+source[start:end]+function(source,'tail_custom_store')+TEST
    scratch=ROOT/'.tmp';scratch.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='mp3-lifecycle-',dir=scratch) as temp:
        d=Path(temp);(d/'test.c').write_text(code,encoding='utf-8');exe=d/'test.exe'
        env=dict(os.environ,TEMP=temp,TMP=temp,TMPDIR=temp)
        subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',str(d/'test.c'),'-o',str(exe)],check=True,env=env)
        subprocess.run([str(exe)],check=True,env=env)

if __name__=='__main__':main()
