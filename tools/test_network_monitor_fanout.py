#!/usr/bin/env python3
"""Exercise both real engine playback functions through the production monitor ring."""
from pathlib import Path
import os,shutil,subprocess,tempfile
from test_np_peer_runtime import function
ROOT=Path(__file__).resolve().parents[1]
PRE=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "monitor_rx.h"
#define NP_PKT_SAMPLES 320
#define REL_PKT_SAMPLES 320
#define NP_PKT_MS 20
#define REL_PKT_MS 20
#define VOICE_MONITOR 1
#define NETCOM_MODE_NETP2P 1
#define NETCOM_MODE_RELAY 2
#define PTT_OWNER_NETWORK 2
#define pdMS_TO_TICKS(n) (n)
static bool s_play_stop,s_amp_on;
static void *s_play_task;
static int current_mode,monitor_on;
static int64_t now_ms(void){return 1000;}
static int voice_mode(void){return monitor_on?VOICE_MONITOR:0;}
static bool audio_i2s_clock_running(void){return true;}
static int audio_i2s_start_clock_only(void){return 0;}
static bool voice_dac_held_by_talk(void){return false;}
static size_t tail_preview_fetch(int16_t *out,size_t n){(void)out;(void)n;return 0;}
static void board_amp_enable(bool on){(void)on;}
static void peer_lock(void){}static void peer_unlock(void){}static void rx_lock(void){}static void rx_unlock(void){}
static int dummy;static void *select_active_peer(void){return &dummy;}
static int16_t expected[320],ring[2048];static monitor_rx_t monitor;
static unsigned injections,writes,packet_reads;
static size_t available_room=8192;
static bool ready_for_direct=true,pre_key_seen;
static bool packet(int16_t *out){++packet_reads;memcpy(out,expected,sizeof(expected));return true;}
static bool peer_jbuf_next(void *p,int16_t *out,int *peak){(void)p;(void)peak;return packet(out);}
static bool rel_jbuf_next(int16_t *out,int *peak){(void)peak;return packet(out);}
static void sdr_rx_network_push(uint8_t mode,const int16_t *pcm,size_t n){assert(mode==current_mode);monitor_rx_network(&monitor,mode,pcm,n,1000000);}
static void voice_bridge_inject(const int16_t *pcm,size_t n){
 assert(pre_key_seen);
 assert(!memcmp(pcm,expected,n*sizeof(*pcm)));++injections;
 // The existing RF-side close is deliberately interleaved with network delivery.
 monitor_rx_rf(&monitor,NULL,0,false,1000001);
}
static void bridge_key_update(bool played){if(!played)pre_key_seen=true;}
static size_t voice_bridge_room(void){return available_room;}
static bool voice_bridge_network_ready(void){return ready_for_direct;}
static void voice_bridge_network_cancel(void){}
static bool ptt_owned(int owner,int token){(void)owner;(void)token;return false;}
static void ptt_bridge_key(bool on){(void)on;}
static bool ptt_bridge_keyed(void){return false;}
static void voice_speaker_apply(int16_t *pcm,size_t n){for(size_t i=0;i<n;i++)pcm[i]/=2;}
static int audio_i2s_write(const int16_t *pcm,size_t n){(void)pcm;(void)n;++writes;s_play_stop=true;return 0;}
static void vTaskDelay(int n){(void)n;s_play_stop=true;}
static void vTaskDeleteWithCaps(void *p){(void)p;}
'''
POST=r'''
int main(void){
 for(int i=0;i<320;i++)expected[i]=(int16_t)(i%2?-200:100);
 for(int mode=1;mode<=2;mode++)for(int branch=0;branch<2;branch++)for(int own=0;own<2;own++){
  current_mode=mode;monitor_on=branch;s_play_stop=false;s_amp_on=false;pre_key_seen=false;
  monitor_rx_init(&monitor,ring,2048);monitor_rx_mode(&monitor,mode);monitor_rx_local_tx(&monitor,own!=0);
  if(mode==1)netp2pnet_play_task(NULL);else relay_play_task(NULL);
  int16_t out[320];size_t n=monitor_rx_take(&monitor,out,320);
  if(own)assert(n==0);else assert(n==320&&!memcmp(out,expected,sizeof(expected)));
 }
 assert(injections==4&&writes==4);
 for(int mode=1;mode<=2;mode++)for(int branch=0;branch<2;branch++){
  current_mode=mode;monitor_on=branch;s_play_stop=false;s_amp_on=false;pre_key_seen=false;
  monitor_rx_init(&monitor,ring,2048);monitor_rx_mode(&monitor,mode);
  available_room=0;ready_for_direct=false;unsigned before=packet_reads;
  if(mode==1)netp2pnet_play_task(NULL);else relay_play_task(NULL);
  int16_t out[320];assert(monitor_rx_take(&monitor,out,320)==0 && packet_reads==before);
 }
 puts("PASS: full PCM ring/unfinished TX lead never advance network playback head; control precedes injection");
 puts("PASS: production P2P+MQTT playback, monitor/direct branches, raw APP copy before RF processing, RF close interleaving, own-APP PTT mute");
}
'''
def main():
 a=(ROOT/'firmware/main/netp2pnet.c').read_text(encoding='utf-8');b=(ROOT/'firmware/main/netrelay.c').read_text(encoding='utf-8')
 code=PRE+function(a,'netp2pnet_play_task')+function(b,'relay_play_task')+POST
 (ROOT/'.tmp').mkdir(exist_ok=True)
 with tempfile.TemporaryDirectory(prefix='fanout-',dir=ROOT/'.tmp') as temp:
  d=Path(temp);(d/'test.c').write_text(code,encoding='utf-8');exe=d/'test.exe';env=dict(os.environ,TEMP=temp,TMP=temp,TMPDIR=temp)
  subprocess.run([shutil.which('gcc'),'-std=c11','-Wall','-Wextra','-Werror','-I',str(ROOT/'firmware/main'),str(d/'test.c'),str(ROOT/'firmware/main/monitor_rx.c'),'-o',str(exe)],check=True,env=env)
  subprocess.run([str(exe)],check=True,timeout=15)
if __name__=='__main__':main()
