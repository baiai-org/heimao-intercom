#!/usr/bin/env python3
"""Compile production peer add/lookup/audio/store/status functions with host substitutes."""
from pathlib import Path
import json,os,re,shutil,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
def function(source,name):
 m=re.search(r'(?m)^(?:static\s+)?(?:inline\s+)?(?:const\s+)?\w+\s*\**\s*\b'+name+r'\([^;]*?\)\s*\{',source)
 if not m:raise RuntimeError(name)
 depth=0;quote=None;escape=False;comment=False;line=False;i=source.index('{',m.start())
 while i<len(source):
  c=source[i];pair=source[i:i+2]
  if line:
   if c=='\n':line=False
  elif comment:
   if pair=='*/':comment=False;i+=1
  elif quote:
   if escape:escape=False
   elif c=='\\':escape=True
   elif c==quote:quote=None
  elif pair=='//':line=True;i+=1
  elif pair=='/*':comment=True;i+=1
  elif c in '\"\'':quote=c
  elif c=='{':depth+=1
  elif c=='}':
   depth-=1
   if not depth:return source[m.start():i+1]+'\n'
  i+=1
 raise RuntimeError('unclosed '+name)
PRE=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
static int inet_aton(const char *s,struct in_addr *a){return inet_pton(AF_INET,s,a)==1;}
#else
#include <arpa/inet.h>
#endif
#include "np_peer_store.h"
#include "np_access.h"
#include "net_rx_order.h"
static const char *inet_ntoa_r(struct in_addr a,char *out,size_t n){return inet_ntop(AF_INET,&a,out,n);}
typedef int esp_err_t;typedef void opus_link_dec_t;typedef int nvs_handle_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_NOT_FOUND 2
#define ESP_FAIL 3
#define ESP_ERR_NO_MEM 4
#define ESP_ERR_INVALID_STATE 5
#define ESP_ERR_NVS_NOT_FOUND 6
#define KEY_ACCESS "np_state1"
#define MALLOC_CAP_SPIRAM 1
#define NETCOM_MODE_NETP2P 1
#define STREAM_IDLE_MS 500
#define PEER_EXPIRE_MS 90000
#define KNOCK_PERIOD_MS 1000
#define PING_PERIOD_MS 10000
#define F_KNOCK 1
#define F_AUDIO 0
#define NP_FLAG_HUB 4u
#define NP_FLAG_FORWARDED 8u
#define NP_FLAG_ROLE 16u
#define NP_HDR_LEN 14
#define NP_AUDIO_LEN 104
#define F_PING 3
#define PEER_MAX NP_PEER_LIMIT
#define SLOT_MAX (PEER_MAX+1)
#define NP_AUDIO_OPUS 80
#define JBUF_SLOTS 128
#define KEY_PEERS "np_peers"
#define KEY_PEERS2 "np_peer2"
#define KEY_PEERS3 "np_peer3"
#define KEY_HUB "nphub"
#define KEY_STUN "np_stun"
#define NVS_NS "orl"
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define F_BYE 5
#define pdMS_TO_TICKS(n) (n)
static void vTaskDelay(int n){(void)n;}
static bool s_started=true;
static void test_log(const char *tag,const char *fmt,...){(void)tag;(void)fmt;}
#define ESP_LOGI(...) test_log(__VA_ARGS__)
#define ESP_LOGW(...) test_log(__VA_ARGS__)
static const char *TAG="test";
static int64_t clock_ms=100000;
static int64_t now_ms(void){return clock_ms;}
static void peer_lock(void){}
static void peer_unlock(void){}
static unsigned allocations,frees;static bool oom;
static void *tracked[32];
static void *heap_caps_malloc(size_t n,int caps){(void)caps;if(oom)return NULL;void *p=malloc(n);if(p && n==128*(80+2+1)){allocations++;for(int i=0;i<32;i++)if(!tracked[i]){tracked[i]=p;break;}}return p;}
static void heap_caps_free(void *p){if(p){for(int i=0;i<32;i++)if(tracked[i]==p){frees++;tracked[i]=NULL;break;}free(p);}}
static void opus_link_dec_free(void *p){free(p);}
static void tomb_clear(const uint8_t *m){(void)m;}
static void radio_led_note_net_rx(int mode){(void)mode;}
static void radio_led_note_net_tx(int mode){(void)mode;}
static bool s_peers_dirty,s_hub_on;
static char s_stun_str[128];
static unsigned char saved[NP_ACCESS_BYTES],legacy[108];static size_t saved_size;
static bool save_fail;
static esp_err_t nvs_open(const char *s,int mode,nvs_handle_t *h){(void)s;(void)mode;*h=1;return 0;}
static void nvs_close(nvs_handle_t h){(void)h;}
static esp_err_t nvs_get_blob(nvs_handle_t h,const char *key,void *out,size_t *n){
 (void)h;const void *data=NULL;size_t size=0;
 if(!strcmp(key,KEY_ACCESS)){data=saved;size=saved_size;}else if(!strcmp(key,KEY_PEERS2)){data=legacy;size=sizeof(legacy);}
 if(!size)return ESP_ERR_NVS_NOT_FOUND;if(*n<size)return ESP_FAIL;memcpy(out,data,size);*n=size;return 0;
}
static esp_err_t nvs_set_blob(nvs_handle_t h,const char *k,const void *data,size_t n){
 (void)h;assert(!strcmp(k,KEY_ACCESS));if(save_fail)return ESP_FAIL;assert(n<=sizeof(saved));memcpy(saved,data,n);saved_size=n;return 0;
}
static esp_err_t nvs_get_u8(nvs_handle_t h,const char *k,uint8_t *v){(void)h;(void)k;*v=1;return 0;}
static esp_err_t nvs_set_u8(nvs_handle_t h,const char *k,uint8_t v){(void)h;(void)k;(void)v;return 0;}
static esp_err_t nvs_get_str(nvs_handle_t h,const char *k,char *out,size_t *n){(void)h;(void)k;assert(*n>20);strcpy(out,"stun.example:3478");return 0;}
static esp_err_t nvs_set_str(nvs_handle_t h,const char *k,const char *s){(void)h;(void)k;(void)s;return 0;}
static esp_err_t nvs_commit(nvs_handle_t h){(void)h;return save_fail?ESP_FAIL:0;}
static const char *esp_err_to_name(esp_err_t e){(void)e;return "storage error";}
static bool stun_set_inner(const char *s){strcpy(s_stun_str,s);return true;}
static uint32_t s_pub_ip,s_drop_short,s_drop_room,s_drop_self,s_drop_tomb,s_drop_psk,s_drop_full,s_bye_rx;
static unsigned s_pub_port=54321;static int s_port_preserving=1,s_last_rtt_ms=50,s_last_send_rc,s_bye_tx;
static int64_t s_last_stun_ms=100000;
static char s_last_del[128];
static bool netp2pnet_has_global_ipv6(void){return true;}
static void json_escape_str_n(const char *s,char *out,size_t cap){
 size_t n=0;for(;*s&&n+2<cap;s++){if(*s=='"'||*s=='\\')out[n++]='\\';out[n++]=*s;}out[n]=0;
}
'''
POST=r'''
static void tomb_clear_addr(const np_addr_t *a){(void)a;}
static void tomb_add(const rt_peer_t *p){(void)p;}
static unsigned pings,knocks,forwards;
static bool frame_send_to(rt_peer_t *p,uint8_t type,uint16_t seq,bool keyed,const uint8_t *pcm){(void)p;(void)seq;(void)keyed;(void)pcm;if(type==F_PING)pings++;if(type==F_KNOCK)knocks++;return true;}
static int peer_send_raw(rt_peer_t *p,const uint8_t *data,int len){(void)p;assert(data[0]==0x42&&len==104);assert(data[12]&NP_FLAG_FORWARDED);forwards++;return len;}
static void peers_save(void);
static np_access_record_t s_denied[NP_DENY_LIMIT];
static size_t s_denied_count;static bool s_access_error;

'''
HARNESS=r'''
int main(void){
#ifdef _WIN32
 WSADATA w;assert(!WSAStartup(MAKEWORD(2,2),&w));
#endif
 // Real loader must migrate all four IPv4/IPv6 entries AND restore hub/STUN.
 for(int i=0;i<4;i++){uint8_t *p=legacy+i*27;sprintf((char*)p,"old%d",i);p[8]=i%2?6:4;p[9]=0x20;p[24]=i+1;p[25]=0xd4;p[26]=0x31;}
 peers_load();assert(s_peers_dirty&&s_hub_on&&!strcmp(s_stun_str,"stun.example:3478"));
 assert(s_peer[1].fam==6&&s_peer[1].ip6[15]==2&&s_peer[0].fam==4);assert(!allocations);
 peers_save();assert(!s_peers_dirty && saved_size==136 && saved[0]==1);
 for(int i=0;i<SLOT_MAX;i++)peer_clear(&s_peer[i]);
 // Empty names/addresses are not included; fill sixteen configured devices.
 for(int i=0;i<16;i++){char name[8],ip[32];sprintf(name,"p%d",i);sprintf(ip,"192.0.2.%d",i+1);assert(netp2pnet_peer_add(name,ip,54321)==0);}
 assert(!allocations);rt_peer_t before[16];memcpy(before,s_peer,sizeof(before));
 assert(netp2pnet_peer_add("new","192.0.2.99",54321)==ESP_ERR_NOT_FOUND);
 assert(!memcmp(before,s_peer,sizeof(before))); // no silent replacement
 np_addr_t src={.fam=4,.port=54321};struct in_addr ip;inet_aton("192.0.2.99",&ip);src.ip=ip.s_addr;
 const uint8_t mac[6]={1,2,3,4,5,6};assert(!peer_ensure(mac,&src));assert(!memcmp(before,s_peer,sizeof(before)));
 // Updating an existing peer is allowed even when all sixteen slots are used.
 assert(netp2pnet_peer_add("updated","192.0.2.1",54321)==0);
 uint8_t audio[80]={0x12};oom=true;jbuf_audio(&s_peer[0],1,audio,80,true,0);assert(!allocations&&s_peer[0].rx_drop==1);oom=false;
 for(int i=0;i<16;i++){jbuf_audio(&s_peer[i],1,audio,80,true,0);assert(s_peer[i].jbuf_valid[1]&&s_peer[i].jbuf[1][0]==0x12);}
 assert(allocations==16);assert(saved_size==4+16*33);
 // Simulated liveness for all sixteen slots, then expiry and re-link.
 for(int i=0;i<16;i++){s_peer[i].linked=true;s_peer[i].last_frame_ms=clock_ms;s_peer[i].mac[5]=i+1;}
 engine_tick_timers();assert(pings==16);
 for(int tick=0;tick<600;tick++){clock_ms+=10001;for(int i=0;i<16;i++)s_peer[i].last_frame_ms=clock_ms;engine_tick_timers();}
 assert(pings==16*601);
 clock_ms+=90001;engine_tick_timers();assert(knocks==16);
 clock_ms+=55;for(int i=0;i<16;i++){assert(!s_peer[i].linked);link_peer(&s_peer[i],NULL);assert(s_peer[i].linked&&s_peer[i].rtt_ms==55);}
 uint8_t packet[104]={0x42};s_hub_on=true;hub_forward_audio(s_peer[0].mac,packet,sizeof(packet),true);assert(forwards==15);
 s_hub_on=false;hub_forward_audio(s_peer[0].mac,packet,sizeof(packet),true);assert(forwards==15);
 s_hub_on=true;packet[12]=NP_FLAG_FORWARDED;hub_forward_audio(s_peer[0].mac,packet,sizeof(packet),true);assert(forwards==15);packet[12]=0;
 // Maximal printable names/IPv6/counters exercise the actual JSON serializer.
 for(int i=0;i<16;i++){rt_peer_t *p=&s_peer[i];p->fam=6;memset(p->ip6,0xee,16);p->ip6[15]=i;p->linked=true;strcpy(p->name,"\"\"\"\"\"\"\"");p->rx_msgs=p->tx_msgs=p->rx_drop=UINT32_MAX;p->rtt_ms=2147483647;}
 struct {char json[5120];uint8_t guard[16];} b;memset(&b,0xa5,sizeof(b));netp2pnet_status_json(b.json,sizeof(b.json));
 for(unsigned i=0;i<16;i++)assert(b.guard[i]==0xa5);
 printf("{%s}\n",b.json);
 save_fail=true;peers_save();assert(s_peers_dirty);save_fail=false;peers_save();assert(!s_peers_dirty);
 // Repeat actual delete/re-add/audio allocation; capacity and memory stay bounded.
 for(int cycle=0;cycle<1000;cycle++){
  assert(netp2pnet_peer_del(15)==ESP_OK);assert(allocations-frees==15);
  char block_id[56];np_access_id(&s_denied[s_denied_count-1],block_id,sizeof(block_id));
  assert(netp2pnet_unblock(block_id)==ESP_OK);
  assert(netp2pnet_peer_add_confirmed("fresh","192.0.2.99",54321,true)==ESP_OK);
  jbuf_audio(&s_peer[15],1,audio,80,true,0);assert(allocations-frees==16);
 }
 peer_audio_free(&s_peer[0]);assert(!s_peer[0].jbuf&&!s_peer[0].jbuf_seq&&!s_peer[0].jbuf_valid);
 peer_reset_stream(&s_peer[0]);jbuf_audio(&s_peer[0],2,audio,80,true,0);assert(s_peer[0].jbuf_valid[2]);
 for(int i=0;i<SLOT_MAX;i++)peer_clear(&s_peer[i]);assert(allocations==frees);
 peers_load();assert(s_peer[15].in_use&&s_peer[14].fam==6&&!s_peers_dirty&&!s_peer[15].jbuf);
 for(int i=0;i<SLOT_MAX;i++)peer_clear(&s_peer[i]);
 // Three-node star: Hub transport and original speaker must be separate rows.
 saved_size=0;s_hub_on=false;s_peers_dirty=false;
 assert(netp2pnet_peer_add("bad","0.0.0.0",54321)==ESP_ERR_INVALID_ARG);
 assert(netp2pnet_peer_add("bad","::",54321)==ESP_ERR_INVALID_ARG);
 assert(netp2pnet_peer_add("","192.0.2.9",54321)==ESP_ERR_INVALID_ARG);
 assert(netp2pnet_peer_add("hub","192.0.2.9",54321)==0);
 np_addr_t transport={.fam=4,.port=54321};inet_aton("192.0.2.9",&ip);transport.ip=ip.s_addr;
 const uint8_t hub_mac[6]={1,2,3,4,5,9},member_mac[6]={1,2,3,4,5,8};
 rt_peer_t *hub=peer_for_frame(hub_mac,&transport,F_KNOCK,NP_FLAG_ROLE|NP_FLAG_HUB);
 assert(hub==&s_peer[0]&&hub->hub&&!hub->via_hub);link_peer(hub,&transport);
 hub->rx_src=transport;hub->rx_src_ms=clock_ms;hub->last_frame_ms=clock_ms;
 rt_peer_t *member=peer_for_frame(member_mac,&transport,F_AUDIO,NP_FLAG_FORWARDED);
 assert(member&&member!=hub&&member->via_hub&&!peer_addr_set(member));member->last_frame_ms=clock_ms;
 assert(!strcmp(peer_state_str(member),"linked"));
 peers_save();assert(saved_size==37); // only the real hub is stored, never a fake 0.0.0.0 endpoint
 save_fail=true;assert(netp2pnet_hub_set(true)==ESP_FAIL&&!s_hub_on&&s_peers_dirty);
 save_fail=false;assert(netp2pnet_hub_set(false)==ESP_OK&&!s_hub_on&&!s_peers_dirty);
 char route_json[5120];netp2pnet_status_json(route_json,sizeof(route_json));
 assert(strstr(route_json,"\"route\":\"hub\",\"via\":\"hub\""));
 assert(strstr(route_json,"\"ip\":\"\""));
 unsigned old_pings=pings,old_knocks=knocks;engine_tick_timers();
 assert(pings<=old_pings+1 && knocks==old_knocks);
 hub->hub=false;hub->linked=false;hub->last_frame_ms=clock_ms-90001;
 assert(hub_quiet_ms()==0); // ordinary peer expiry is not "hub offline"
 hub->hub=true;assert(hub_quiet_ms()>90000);
 engine_tick_timers();assert(!member->linked&&!strcmp(peer_state_str(member),"expired"));
 // A real direct handshake promotes a discovered member to a real endpoint.
 np_addr_t direct={.fam=4,.port=54321};inet_aton("192.0.2.8",&ip);direct.ip=ip.s_addr;
 member=peer_for_frame(member_mac,&direct,F_KNOCK,NP_FLAG_ROLE);
 assert(member&&!member->via_hub&&member->ip==direct.ip);
 for(int i=0;i<SLOT_MAX;i++)peer_clear(&s_peer[i]);

 // Persistent deletion: offline peer, 5+ minutes, reboot, manual retry and address changes.
 const uint8_t bm[6]={2,3,4,5,6,7},cm[6]={2,3,4,5,6,8};
 assert(netp2pnet_peer_add("B","192.0.2.42",54321)==ESP_OK);
 np_addr_t baddr={.fam=4,.port=54321};inet_aton("192.0.2.42",&ip);baddr.ip=ip.s_addr;
 rt_peer_t *bpeer=peer_ensure(bm,&baddr);assert(bpeer);peers_save();
 assert(netp2pnet_peer_del(0)==ESP_OK&&s_denied_count==1);
 save_fail=true;assert(netp2pnet_peer_add_confirmed("B","192.0.2.42",54321,true)!=ESP_OK);
 assert(s_denied_count==1&&!s_peer[0].in_use);save_fail=false;
 assert(!frame_admitted(bm,&baddr,F_KNOCK,2)); // remote "manual" bit cannot restore
 clock_ms+=86400000;
 baddr.port=32100;inet_aton("198.51.100.42",&ip);baddr.ip=ip.s_addr;
 assert(!frame_admitted(bm,&baddr,F_KNOCK,2));
 assert(frame_admitted(cm,&baddr,F_KNOCK,2)); // shared IP is not a blacklist
 memset(s_denied,0,sizeof(s_denied));s_denied_count=0;peers_load();
 assert(s_denied_count==1&&!frame_admitted(bm,&baddr,F_AUDIO,0));
 np_addr_t b6={.fam=6,.port=54321};b6.ip6[0]=0x20;b6.ip6[15]=0x42;
 assert(!frame_admitted(bm,&b6,F_KNOCK,2));
 // Hub-origin denial does not ban its transport or unrelated members.
 assert(netp2pnet_peer_add("hub","192.0.2.8",54321)==ESP_OK);
 const uint8_t hm[6]={2,1,1,1,1,1};np_addr_t hs={.fam=4,.port=54321};inet_aton("192.0.2.8",&ip);hs.ip=ip.s_addr;
 rt_peer_t *hp=peer_ensure(hm,&hs);hp->linked=true;
 assert(!frame_admitted(bm,&hs,F_AUDIO,NP_FLAG_FORWARDED));
 assert(frame_admitted(cm,&hs,F_AUDIO,NP_FLAG_FORWARDED));
 char bid[56];np_access_id(&s_denied[0],bid,sizeof(bid));
 save_fail=true;assert(netp2pnet_unblock(bid)!=ESP_OK&&s_denied_count==1);
 save_fail=false;assert(netp2pnet_unblock(bid)==ESP_OK&&s_denied_count==0);
 assert(frame_admitted(bm,&b6,F_KNOCK,2));
 // Known MAC survives active-peer persistence even if offline after reboot.
 peers_save();for(int i=0;i<SLOT_MAX;i++)peer_clear(&s_peer[i]);peers_load();
 assert(!memcmp(s_peer[0].mac,hm,6));
 save_fail=true;assert(netp2pnet_peer_del(0)!=ESP_OK&&s_peer[0].in_use&&s_denied_count==0);
 save_fail=false;
 // Legacy unknown identity restricts only exact endpoint, not a shared IP.
 assert(netp2pnet_peer_add("old","192.0.2.42",54321)==ESP_OK);
 assert(netp2pnet_peer_del(1)==ESP_OK);
 baddr.port=54321;inet_aton("192.0.2.42",&ip);baddr.ip=ip.s_addr;
 assert(!frame_admitted(bm,&baddr,F_KNOCK,2));baddr.port++;
 assert(frame_admitted(bm,&baddr,F_KNOCK,2));
 assert(netp2pnet_peer_add("old","192.0.2.42",54321)==ESP_ERR_INVALID_STATE);
 assert(netp2pnet_peer_add_confirmed("old","192.0.2.42",54321,true)==ESP_OK);
 assert(s_denied_count==0);
 // Full policy never silently evicts; delete fails without altering the peer.
 s_denied_count=NP_DENY_LIMIT;assert(netp2pnet_peer_del(0)==ESP_ERR_NO_MEM&&s_peer[0].in_use);
 s_denied_count=0;
 s_access_error=true;assert(!frame_admitted(cm,&baddr,F_KNOCK,0));
 assert(netp2pnet_peer_del(0)==ESP_ERR_INVALID_STATE);s_access_error=false;
 // Maximum denied response, escaped names, IPv6 and stable IDs: no truncation.
 for(int i=0;i<NP_DENY_LIMIT;i++){
  np_access_record_t *r=&s_denied[i];memset(r,0,sizeof(*r));strcpy(r->peer.name,"\"\"\"\"\"\"\"");
  r->peer.family=6;memset(r->peer.address,0xee,16);r->peer.address[15]=i+1;r->peer.port=65535;
  if(i<32){r->mac[0]=2;r->mac[5]=i+1;}
 }s_denied_count=NP_DENY_LIMIT;
 char *json=malloc(16384);assert(netp2pnet_denied_json(json,16384)==ESP_OK);printf("%s\n",json);free(json);
 char small[50];assert(netp2pnet_denied_json(small,sizeof(small))==ESP_ERR_NO_MEM);
 np_access_state_t *all=calloc(1,sizeof(*all)),*decoded=calloc(1,sizeof(*decoded));
 all->denied_count=NP_DENY_LIMIT;memcpy(all->denied,s_denied,sizeof(s_denied));
 all->peer_count=NP_PEER_LIMIT;
 for(int i=0;i<NP_PEER_LIMIT;i++){all->peers[i]=all->denied[i];}
 uint8_t *wire=malloc(NP_ACCESS_BYTES);assert(np_access_encode(all,wire,NP_ACCESS_BYTES)==NP_ACCESS_BYTES);
 assert(np_access_decode(wire,NP_ACCESS_BYTES,decoded)&&decoded->denied_count==64&&decoded->peer_count==16);
 assert(!np_access_decode(wire,NP_ACCESS_BYTES-1,decoded));wire[1]=17;
 assert(!np_access_decode(wire,NP_ACCESS_BYTES,decoded));wire[1]=16;wire[2]=65;
 assert(!np_access_decode(wire,NP_ACCESS_BYTES,decoded));wire[2]=64;wire[3]=32;
 assert(!np_access_decode(wire,NP_ACCESS_BYTES,decoded));wire[3]=33;
 memset(wire+4,'X',8);assert(!np_access_decode(wire,NP_ACCESS_BYTES,decoded));
 free(wire);free(all);free(decoded);
 s_denied_count=0;for(int i=0;i<SLOT_MAX;i++)peer_clear(&s_peer[i]);assert(allocations==frees);
 // A corrupt new state must never fall back to permissive legacy addresses.
 saved[0]=99;s_access_error=false;peers_load();assert(s_access_error&&!s_peer[0].in_use);
 puts("PASS: actual 16-peer admission/full rejection/incoming lookup/lazy allocation/cleanup/NVS migration; hub origin/transport separation, indirect no-ping/no-NVS, promotion, accurate hub expiry and single-hop forwarding");
}
'''
def main():
 source=(ROOT/'firmware/main/netp2pnet.c').read_text(encoding='utf-8')
 types=''
 for name in ('np_addr_t','rt_peer_t'):
  end=source.index('} '+name+';')+len('} '+name+';');start=source.rfind('typedef struct {',0,end);types+=source[start:end]+'\n'
 names=['be32','put_be32','peer_record','denied_find','denied_address','denied_remove','np_addr_set','peer_addr_set','peer_take_addr','peer_addr_text','peer_by_mac','peer_free_slot','peer_by_source','peer_audio_free','peer_clear','peer_jbuf_alloc','peer_ensure','peer_for_frame','peer_reset_stream','jbuf_audio','link_peer','engine_tick_timers','hub_quiet_ms','peer_state_str','hub_forward_audio','frame_admitted','peers_load','peers_save_ex','peers_save','netp2pnet_hub_set','netp2pnet_peer_add_confirmed','netp2pnet_peer_add','netp2pnet_peer_del','netp2pnet_unblock','netp2pnet_denied_json','netp2pnet_status_json']
 code=PRE+types+'\nstatic rt_peer_t s_peer[SLOT_MAX];\n'+POST+''.join(function(source,n) for n in names)+HARNESS
 (ROOT/'.tmp').mkdir(exist_ok=True)
 with tempfile.TemporaryDirectory(prefix='np-runtime-',dir=ROOT/'.tmp') as temp:
  d=Path(temp);(d/'test.c').write_text(code,encoding='utf-8');exe=d/'test.exe';env=dict(os.environ,TEMP=temp,TMP=temp,TMPDIR=temp)
  command=[shutil.which('gcc'),'-D_DEFAULT_SOURCE','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-unused-variable','-Wno-unused-function','-Wno-misleading-indentation','-I',str(ROOT/'firmware/main'),str(d/'test.c'),str(ROOT/'firmware/main/np_peer_store.c'),str(ROOT/'firmware/main/np_access.c'),'-o',str(exe)]
  if os.name=='nt':command+=['-lws2_32']
  subprocess.run(command,check=True,env=env)
  lines=subprocess.check_output([str(exe)],text=True,timeout=20).splitlines()
  denied=json.loads(lines[-2]);assert denied['count']==64 and len(denied['entries'])==64
  state=json.loads(lines[0])['np2p'];assert state['peer_limit']==16 and state['peer_count']==16 and len(state['peers'])==16
  assert state['jbuf_bytes']==16*128*(80+2+1)
  print(lines[-1])
if __name__=='__main__':main()
