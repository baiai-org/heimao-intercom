"""Read-only firmware audit: compile extracted production functions with host substitutes.

No product files are edited. No board, socket, serial port or RF is accessed.
MQTT malformed order cases deliberately inject application frames directly;
they do not claim that TCP reorders a single publisher's ordered stream.
"""
from pathlib import Path
import hashlib
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys

import tempfile
ROOT = Path(__file__).resolve().parents[1]
OUT = None
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / 'tools'))
from test_np_peer_runtime import function
from test_rx_guard import STUBS

COMMON = r'''
#include "rx_guard.c"
#include "ptt.c"
#include "net_rx_order.h"
#define NP_PKT_SAMPLES 320
#define REL_PKT_SAMPLES 320
#define NP_AUDIO_OPUS 80
#define REL_AUDIO_OPUS 80
#define JBUF_SLOTS 128
#define REL_JBUF_SLOTS 128
#define STREAM_IDLE_MS 500
#define REL_STREAM_IDLE_MS 500
#define PLAY_LAG 6
#define REL_PLAY_LAG 6
#define NP_RX_HANG_MS 250
#define REL_RX_HANG_MS 250
#define NP_RX_PTT_TIMEOUT_MS 2000
#define REL_RX_PTT_TIMEOUT_MS 2000
#define NETCOM_MODE_NETP2P 1
#define NETCOM_MODE_RELAY 2
typedef int opus_link_dec_t;
static int decoder;
static opus_link_dec_t *opus_link_dec_new(void){return &decoder;}
static void opus_link_dec_free(opus_link_dec_t *p){(void)p;}
static int opus_link_decode(opus_link_dec_t *p,const uint8_t *data,size_t n,int16_t *out){
 (void)p;(void)data;(void)n;memset(out,0,320*sizeof(*out));return 320;
}
static int64_t now_ms(void){return clock_us/1000;}
static volatile bool s_rx_ptt;
static volatile int64_t s_rx_ptt_ms,s_rx_hang_ms;
static int64_t s_last_play_ms;
static uint16_t wire_epoch = 10;
static bool s_tail_played;
static void peer_lock(void){} static void peer_unlock(void){}
static void rx_lock(void){} static void rx_unlock(void){}
static void netcom_note_rx_ptt(bool ptt,bool timeout){(void)ptt;(void)timeout;}
static void netcom_note_rx_carrier(bool on){(void)on;}
static bool voice_net_tail_active(void){return false;}
static void voice_net_tail_stop(void){}
static bool voice_net_tail_start(void){return false;}
static bool bridge_active(void){return true;}
/* This suite isolates receive order/PTT; real PCM drain is exercised by
 * test_bridge_speech_prefix.py with the actual voice buffer functions. */
static void voice_bridge_network_state(bool feeding,bool rf){(void)feeding;(void)rf;}
static bool voice_bridge_network_pending(void){return false;}
static void voice_bridge_network_keyed(bool keyed){(void)keyed;}
static void radio_led_note_net_rx(int mode){(void)mode;}
static void advance(int ms){clock_us+=(int64_t)ms*1000;}
static rx_guard_status_t guard(void){rx_guard_status_t s;rx_guard_status(&s);return s;}
static void reset_engine(void);
static void seed_stream(unsigned peer,uint16_t head,uint16_t latest);
static unsigned late_count(void);
static void receive(unsigned peer,uint16_t seq,bool on);
static bool pull_audio(void);
'''

NP_ENV = r'''
#define NP_MAGIC 0x4F52
#define NP_HDR_LEN 14
#define NP_CHECK_LEN 8
#define NP_CTR_BASE 22
#define NP_AUDIO_LEN 104
#define NP_CTR_KNOCK_LEN 30
#define NP_CTR_PING_LEN 24
#define F_AUDIO 0
#define F_KNOCK 1
#define F_KNOCK_ACK 2
#define F_PING 3
#define F_PONG 4
#define F_BYE 5
typedef struct {uint8_t fam;uint32_t ip;uint8_t ip6[16];uint16_t port;} np_addr_t;
#pragma pack(push,1)
typedef struct {uint16_t magic,room,seq;uint8_t mac[6],flags,type;} np_header_t;
#pragma pack(pop)
typedef struct {
 bool in_use,via_hub,selftest;uint8_t fam;uint32_t ip;uint8_t ip6[16];uint16_t port;
 char name[8];np_addr_t rx_src;int64_t rx_src_ms,last_rx_ms,last_frame_ms;
 bool have_latest,head_valid,have_first;uint16_t latest,head,stream_first;
 uint8_t (*jbuf)[NP_AUDIO_OPUS];uint16_t *jbuf_seq;bool *jbuf_valid;
 opus_link_dec_t *opus_dec;unsigned rx_drop,rx_late,rx_plc,rx_msgs;
 net_rx_order_t order;
} rt_peer_t;
static rt_peer_t s_peer[2];
#define SLOT_MAX 2
static uint8_t audio[2][128][80];static uint16_t seqs[2][128];static bool audio_valid[2][128];
static uint8_t s_my_mac[6];
static unsigned s_drop_short,s_drop_room,s_drop_self,s_drop_psk,s_drop_tomb;
static bool s_access_error;
static uint16_t be16(const uint8_t *p){return (uint16_t)((p[0]<<8)|p[1]);}
static int netcom_room(void){return 1;}
static void psk_compute(uint16_t room,const uint8_t *mac,uint8_t *out){(void)room;(void)mac;memset(out,0,8);}
static bool frame_admitted(const uint8_t *mac,const np_addr_t *src,uint8_t type,uint8_t flags){(void)mac;(void)src;(void)type;(void)flags;return true;}
static rt_peer_t *peer_for_frame(const uint8_t *mac,const np_addr_t *src,uint8_t type,uint8_t flags){(void)src;(void)type;(void)flags;assert(mac[5]>=1&&mac[5]<=2);return &s_peer[mac[5]-1];}
static bool peer_jbuf_alloc(rt_peer_t *p){return p->jbuf!=NULL;}
static void link_peer(rt_peer_t *p,const np_addr_t *src){(void)p;(void)src;}
static bool frame_send_to(rt_peer_t *p,uint8_t type,uint16_t seq,bool keyed,const uint8_t *data){(void)p;(void)type;(void)seq;(void)keyed;(void)data;return true;}
static void handle_control(rt_peer_t *p,const uint8_t *pkt,int n,const np_addr_t *src){(void)p;(void)pkt;(void)n;(void)src;}
static void hub_forward_audio(const uint8_t *mac,const uint8_t *pkt,size_t n,bool keyed){(void)mac;(void)pkt;(void)n;(void)keyed;}
'''
NP_POST = r'''
static void reset_engine(void){
 memset(s_peer,0,sizeof(s_peer));memset(audio,0,sizeof(audio));memset(audio_valid,0,sizeof(audio_valid));memset(seqs,0,sizeof(seqs));
 for(int i=0;i<2;i++){s_peer[i].in_use=true;s_peer[i].jbuf=audio[i];s_peer[i].jbuf_seq=seqs[i];s_peer[i].jbuf_valid=audio_valid[i];}
}
static void seed_stream(unsigned peer,uint16_t head,uint16_t latest){
 rt_peer_t *p=&s_peer[peer];p->head_valid=p->have_latest=p->have_first=true;p->head=head;p->latest=latest;p->stream_first=latest-10;p->last_rx_ms=now_ms();
 p->order=(net_rx_order_t){.seen=true,.ptt=true,.audio=true,.epoch=wire_epoch,.latest=latest,.last_ms=now_ms()};
}
static unsigned late_count(void){return s_peer[0].rx_late;}
static void receive(unsigned peer,uint16_t seq,bool on){
 uint8_t pkt[NP_AUDIO_LEN]={0};np_header_t h={.magic=NP_MAGIC,.room=1,.seq=seq,.flags=on|(wire_epoch?NET_RX_FLAG_EPOCH:0),.type=F_AUDIO};h.mac[5]=peer+1;memcpy(pkt,&h,sizeof(h));pkt[22]=(uint8_t)(wire_epoch>>8);pkt[23]=(uint8_t)wire_epoch;
 np_addr_t src={.fam=4,.port=54321};handle_frame(pkt,sizeof(pkt),&src);
}
static bool pull_audio(void){int16_t out[320];int peak;return peer_jbuf_next(&s_peer[0],out,&peak);}
'''

REL_ENV = r'''
#define REL_MAGIC 0x4F52
#define REL_HDR_LEN 14
#define REL_PKT_LEN 94
#pragma pack(push,1)
typedef struct {uint16_t magic,reserved,seq;uint8_t mac[6],flags,reserved2;} rel_header_t;
#pragma pack(pop)
static uint8_t audio[128][80],(*s_jbuf)[REL_AUDIO_OPUS]=audio;
static uint8_t s_jbuf_mac[128][6];
static uint16_t s_jbuf_seq[128],s_latest,s_stream_first,s_play_head;
static bool s_jbuf_valid[128],s_have_latest,s_have_first,s_play_head_valid;
static int64_t s_last_pkt_ms,s_last_rx_ms;
static uint32_t s_rx_msgs,s_rx_dropped,s_ev_badlen,s_ev_total,s_ev_badmagic,s_ev_self,s_ev_ok,s_late,s_plc,s_primes;
static uint16_t s_prime_first,s_prime_play;
static opus_link_dec_t *s_opus_dec;
static uint8_t s_opus_dec_mac[6],s_stream_mac[6],s_mac[6];
typedef struct {uint8_t mac[6];bool used;net_rx_order_t order;} rel_rx_source_t;
static rel_rx_source_t s_rx_sources[16];
static rel_rx_source_t *s_stream_source;
static void peer_seen(const char *id){(void)id;}
typedef struct {int data_len,current_data_offset,total_data_len;char *data;} mqtt_event_t;
'''
REL_POST = r'''
static void reset_engine(void){
 relay_reset_stream();memset(audio,0,sizeof(audio));memset(s_jbuf_seq,0,sizeof(s_jbuf_seq));memset(s_stream_mac,0,sizeof(s_stream_mac));
 s_last_pkt_ms=s_last_rx_ms=0;s_late=0;memset(s_rx_sources,0,sizeof(s_rx_sources));s_stream_source=NULL;
}
static void seed_stream(unsigned peer,uint16_t head,uint16_t latest){
 s_stream_mac[5]=peer+1;s_play_head_valid=s_have_latest=s_have_first=true;s_play_head=head;s_latest=latest;s_stream_first=latest-10;s_last_pkt_ms=now_ms();
 uint8_t mac[6]={0};mac[5]=peer+1;s_stream_source=relay_source(mac);
 s_stream_source->order=(net_rx_order_t){.seen=true,.ptt=true,.audio=true,.epoch=wire_epoch,.latest=latest,.last_ms=now_ms()};
}
static unsigned late_count(void){return s_late;}
static void receive(unsigned peer,uint16_t seq,bool on){
 uint8_t pkt[REL_PKT_LEN]={0};rel_header_t h={.magic=REL_MAGIC,.reserved=wire_epoch,.seq=seq,.flags=on|(wire_epoch?NET_RX_FLAG_EPOCH:0)};h.mac[5]=peer+1;memcpy(pkt,&h,sizeof(h));
 mqtt_event_t event={.data_len=sizeof(pkt),.total_data_len=sizeof(pkt),.data=(char*)pkt};mqtt_event_t *ev=&event;
 switch(1){
'''

TEST = r'''
static void begin(int protection){
 ptt_request_rx();ptt_bridge_key(false);clock_us=1000000;
 rx_guard_context(MODE,false);assert(rx_guard_configure(MODE,protection==700?"1,700,7,1":protection==5000?"1,5000,7,1":"1,10000,7,1")==ESP_OK);
 rx_guard_context(MODE,true);reset_engine();s_rx_ptt=false;s_last_play_ms=0;s_tail_played=false;s_rx_hang_ms=0;
}
static void active(void){s_rx_ptt=true;s_rx_ptt_ms=now_ms();bridge_key_update(false);assert(rf_pin);}
static void finish_valid(void){
 receive(0,101,false);assert(pull_audio());bridge_key_update(true);assert(!s_rx_ptt);advance(251);bridge_key_update(false);
 assert(!rf_pin && guard().blocked==1);
}
int main(void){
 setvbuf(stdout,NULL,_IONBF,0);
 rx_guard_init();ptt_init();
 for(int pi=0;pi<3;pi++){
  int protection=pi==0?700:pi==1?5000:10000;
  begin(protection);seed_stream(0,101,100);active();advance(20);finish_valid();
  assert(guard().remaining_ms==(unsigned)protection);unsigned before=late_count();
  advance(10);receive(0,100,true);assert(late_count()==before+1);assert(!s_rx_ptt);
  unsigned blocked_before=guard().blocked;bridge_key_update(false);
  assert(blocked_before==1 && !rf_pin && guard().blocked==1);
  printf("PASS %s late_ON guard_ms=%d rejected/no_rekey\n",ENGINE,protection);
 }
 begin(700);seed_stream(0,110,109);active();receive(0,105,false);
 assert(late_count()==1 && s_rx_ptt);advance(251);bridge_key_update(false);assert(rf_pin);
 printf("PASS %s late_OFF keeps current PTT\n",ENGINE);

 begin(700);seed_stream(0,101,100);active();advance(20);finish_valid();advance(300);
 receive(0,100,true);assert(late_count()==1 && !s_rx_ptt);bridge_key_update(false);assert(!rf_pin);
 printf("PASS %s old_ON after buffer idle rejected\n",ENGINE);

 begin(700);seed_stream(0,101,100);active();advance(20);finish_valid();advance(10);
 receive(0,102,false);receive(0,103,true);bridge_key_update(false);assert(!rf_pin);
 printf("PASS %s closed epoch rejects newer ON and repeated OFF\n",ENGINE);

 begin(700);seed_stream(0,101,100);active();advance(20);receive(1,1,false);bridge_key_update(false);assert(s_rx_ptt && rf_pin);
 printf("PASS %s other sender OFF does not stop selected sender\n",ENGINE);

 begin(700);seed_stream(0,101,100);active();advance(20);finish_valid();
 for(int k=0;k<100;k++){advance(20);bridge_key_update(false);assert(!rf_pin);}
 printf("PASS %s ordered end releases once\n",ENGINE);

 // New/retired epochs: fast short over drains from its own first frame.
 begin(700);seed_stream(0,101,100);active();advance(20);finish_valid();
 wire_epoch=11;receive(0,102,true);bridge_key_update(false);assert(rf_pin);
 receive(0,103,false);assert(pull_audio());assert(pull_audio());assert(!pull_audio());
 bridge_key_update(true);advance(251);bridge_key_update(false);assert(!rf_pin);
 wire_epoch=10;receive(0,104,true);bridge_key_update(false);assert(!rf_pin);
 wire_epoch=5;receive(0,0,true);bridge_key_update(false);assert(rf_pin);
 printf("PASS %s new epoch/short over/retired epoch/reboot epoch\n",ENGINE);

 // Legacy ordered calls and uint16 wrap remain compatible.
 wire_epoch=0;begin(700);seed_stream(0,65535,65534);active();
 receive(0,65535,true);receive(0,0,false);assert(pull_audio());assert(pull_audio());
 bridge_key_update(true);advance(251);bridge_key_update(false);assert(!rf_pin);
 receive(0,65535,true);bridge_key_update(false);assert(!rf_pin);
 printf("PASS %s legacy ordered wrap and late packet\n",ENGINE);

 // New talkers cannot be latched by duplicate packets extending the watchdog.
 wire_epoch=10;begin(700);seed_stream(0,101,100);active();
 for(int n=0;n<11;n++){advance(200);receive(0,100,true);bridge_key_update(false);}
 advance(251);bridge_key_update(false);
 assert(!rf_pin);
 ptt_trace_event_t trace[64];uint32_t trace_sequence;
 size_t trace_count=ptt_trace_snapshot(trace,64,&trace_sequence);bool found_timeout=false;
 for(size_t i=0;i<trace_count;i++)if(trace[i].kind==PTT_TRACE_NET_TIMEOUT&&trace[i].source==MODE&&trace[i].a==10)found_timeout=true;
 assert(found_timeout);
 printf("PASS %s duplicates do not extend PTT timeout; diagnostic retains timeout cause after active source clears\n",ENGINE);

 // Legacy restart without an OFF: require progress, never one stale packet.
 wire_epoch=0;begin(700);seed_stream(0,1001,1000);active();advance(2501);bridge_key_update(false);
 advance(251);bridge_key_update(false);assert(!rf_pin);
 receive(0,0,true);bridge_key_update(false);assert(!rf_pin);
 advance(20);receive(0,1,true);bridge_key_update(false);assert(!rf_pin);
 advance(20);receive(0,2,true);bridge_key_update(false);assert(rf_pin);
 printf("PASS %s legacy restart requires three progressing frames\n",ENGINE);

 // Exercise repeated genuine overs and replay the just-retired over each time.
 for(unsigned round=0;round<1000;round++){
  wire_epoch=(uint16_t)(100+round*2);begin(700);seed_stream(0,101,100);active();advance(20);finish_valid();
  uint16_t old=wire_epoch;wire_epoch++;
  receive(0,102,true);bridge_key_update(false);assert(rf_pin);
  receive(0,103,false);assert(pull_audio());assert(pull_audio());bridge_key_update(true);
  advance(251);bridge_key_update(false);assert(!rf_pin);
  wire_epoch=old;receive(0,104,true);bridge_key_update(false);assert(!rf_pin);
 }
 printf("PASS %s 1000 over transitions and retired-over replays\n",ENGINE);
 return 0;
}
'''

def mqtt_data_case(source):
    start = source.index('case MQTT_EVENT_DATA: {')
    body = source[start + len('case MQTT_EVENT_DATA: '):]
    wrapped = 'static void case_body(void) ' + body
    extracted = function(wrapped, 'case_body')
    return 'case 1: ' + extracted[extracted.index('{'):]

def run():
    sources = {name:(ROOT / 'firmware/main' / name).read_text(encoding='utf-8') for name in ('netp2pnet.c','netrelay.c')}
    hashes = {name:hashlib.sha256((ROOT/'firmware/main'/name).read_bytes()).hexdigest() for name in sources}
    for name in ('rx_guard.c','rx_guard.h','ptt.c','ptt.h','net_rx_order.h'):
        shutil.copy2(ROOT/'firmware/main'/name, OUT/name)
        hashes[name]=hashlib.sha256((OUT/name).read_bytes()).hexdigest()
    (OUT/'freertos').mkdir(exist_ok=True)
    (OUT/'stubs.h').write_text(STUBS,encoding='utf-8')
    for name in ('esp_err.h','esp_timer.h','esp_log.h','board_hw.h','sa818_cfg.h','nvs.h','freertos/FreeRTOS.h','freertos/semphr.h','freertos/task.h'):
        (OUT/name).write_text('#include "stubs.h"\n',encoding='utf-8')
    env=dict(os.environ,TEMP=str(OUT),TMP=str(OUT),TMPDIR=str(OUT),PYTHONIOENCODING='utf-8')
    results=[]
    gcc=shutil.which('gcc')
    for engine,source in sources.items():
        if engine=='netp2pnet.c':
            code=COMMON+NP_ENV+''.join(function(source,f) for f in ('peer_reset_stream','jbuf_audio','handle_frame','peer_jbuf_next','select_active_peer','bridge_key_update'))+NP_POST
            mode=1
        else:
            code=COMMON+REL_ENV+''.join(function(source,f) for f in ('relay_source','relay_reset_stream','relay_store_packet','rel_jbuf_next','bridge_key_update'))+REL_POST+mqtt_data_case(source)+'\n}\n}\n'
            code+='static bool pull_audio(void){int16_t out[320];int peak;return rel_jbuf_next(out,&peak);}\n'
            mode=2
        code=f'#define MODE {mode}\n#define ENGINE "{engine}"\n'+code+TEST
        cpath=OUT/(engine+'.audit.c');exe=OUT/(engine+'.audit.exe')
        cpath.write_text(code,encoding='utf-8')
        command=[gcc,'-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-variable','-Wno-unused-function','-Wno-misleading-indentation','-I',str(OUT),str(cpath),'-o',str(exe)]
        compile_result=subprocess.run(command,capture_output=True,text=True,env=env)
        (OUT/(engine+'.compile.log')).write_text(compile_result.stdout+compile_result.stderr,encoding='utf-8')
        if compile_result.returncode:
            print(compile_result.stderr)
            raise SystemExit(compile_result.returncode)
        run=subprocess.run([str(exe)],capture_output=True,text=True,env=env,timeout=15)
        (OUT/(engine+'.run.log')).write_text(run.stdout+run.stderr,encoding='utf-8')
        print(run.stdout,run.stderr,end='')
        results.append({'engine':engine,'compile_exit':compile_result.returncode,'run_exit':run.returncode,'command':command,'output':run.stdout+run.stderr})
        if run.returncode:
            (OUT/'results.json').write_text(json.dumps({'hashes':hashes,'results':results},indent=2),encoding='utf-8')
            raise SystemExit(run.returncode)
    (OUT/'results.json').write_text(json.dumps({'hashes':hashes,'results':results,'scope':'Extracted production C with substitute RTOS/GPIO/codec/auth/peer lookup. Both network receive/PTT paths, real rx_guard.c and ptt.c. No hardware/network. MQTT disorder is deliberate injection, not a transport claim.'},indent=2),encoding='utf-8')

def main():
    global OUT
    if os.environ.get('NET_RX_TEST_OUTPUT'):
        OUT=Path(os.environ['NET_RX_TEST_OUTPUT']).resolve();OUT.mkdir(parents=True,exist_ok=True)
        run();return
    scratch=ROOT/'.tmp';scratch.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='net-rx-order-',dir=scratch) as temp:
        OUT=Path(temp)
        assert OUT.resolve().parent == scratch.resolve()
        run()

if __name__=='__main__':main()
