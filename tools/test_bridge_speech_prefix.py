"""Real network receive/PTT + voice injection lifecycle, with host codec/GPIO.
No board or network access. Tests first/last samples and drain/key timing.
"""
from pathlib import Path
import json,os,shutil,subprocess,tempfile
from test_np_peer_runtime import function
from test_rx_guard import STUBS
import test_net_rx_order as order
ROOT=Path(__file__).resolve().parents[1]

VOICE=r'''
#define BRIDGE_INJ_SAMPLES 8192
#define NET_TX_LEAD_US 300000LL
#define NET_DRAIN_LIMIT_US 2000000LL
static int16_t s_bridge_inj[BRIDGE_INJ_SAMPLES];
static volatile size_t s_bridge_inj_head,s_bridge_inj_tail;
static int s_bridge_inj_mux,s_bgtx=16;
static float s_inj_frac;
static bool s_inj_primed,s_inject_tx_processed,s_last_bridge_tx_processed,s_inject_silent;
static bool s_net_feeding,s_net_rf_required,s_net_rf_keyed,s_net_write_pending;
static int64_t s_net_ready_us,s_net_close_us;
static uint32_t s_inject_token,s_bridge_starve,s_bridge_trim;
static ptt_owner_t s_inject_owner;
void voice_inject_reset(void);size_t voice_bridge_level(void);size_t voice_bridge_take(int16_t*,size_t);
void voice_bridge_network_state(bool,bool);void voice_bridge_network_keyed(bool);
bool voice_bridge_network_pending(void);bool voice_bridge_network_ready(void);
void voice_bridge_network_written(void);void voice_bridge_network_cancel(void);
static uint16_t wire_pcm;
'''

TEST=r'''
static uint16_t tx_seq;
static void fixture(void){
 ptt_bridge_key(false);voice_bridge_network_cancel();voice_inject_reset();reset_engine();
 advance(100);rx_guard_context(MODE,false);assert(rx_guard_configure(MODE,"1,700,7,1")==ESP_OK);
 rx_guard_context(MODE,true);s_rx_ptt=false;s_last_play_ms=0;wire_epoch=10;tx_seq=100;
}
static void clip(int frames,int gap){
 advance(gap);wire_epoch++;int first=-1,last=-1;size_t count=0;int first_key=-1,first_play=-1;
 bool was_key=rf_pin,ended=false;int delivered=0;
 for(int t=0;t<5000;t++){
  if(t%20==0 && delivered<frames){wire_pcm=(uint16_t)(1000+delivered);receive(0,tx_seq++,true);delivered++;}
  else if(t==frames*20){wire_pcm=9000;receive(0,tx_seq++,false);ended=true;}
  if(t%5==0){
   bridge_key_update(false);
   if(!was_key && rf_pin && first_key<0)first_key=t;
   was_key=rf_pin;
   bool have=false;
   if(t%20==0 && voice_bridge_room()>=320){int16_t pcm[320];have=engine_take(pcm);if(have)voice_bridge_inject(pcm,320);}
   bridge_key_update(have);
   if(!was_key && rf_pin && first_key<0)first_key=t;
   was_key=rf_pin;
  }
  if(t%16==0){
   int16_t pcm[256];size_t n=voice_bridge_play(pcm,256);
   if(n){
    assert(rf_pin && first_key>=0 && t-first_key>=300);
    if(first<0){first=pcm[0];first_play=t;}last=pcm[n-1];count+=n;
   }
   voice_bridge_network_written();
  }
  if(ended && first>=0 && !rf_pin && !voice_bridge_level() && !voice_bridge_network_pending()){
   assert(first==1000 && last==9000 && count>320);
   assert(!s_inj_primed);
   assert(first_play-first_key>=300);
   return;
  }
  advance(1);
 }
 assert(!"over failed to drain/release");
}
static void burst(int frames,int end_at){
 advance(30000);wire_epoch++;
 for(int i=0;i<frames;i++){wire_pcm=(uint16_t)(1000+i);receive(0,tx_seq++,true);}
 if(!end_at){wire_pcm=9000;receive(0,tx_seq++,false);}
 int first=-1,last=-1;bool saw_key=false;
 for(int t=0;t<5000;t++){
  if(end_at && t==end_at){wire_pcm=9000;receive(0,tx_seq++,false);}
  if(t%5==0){
   bridge_key_update(false);saw_key|=rf_pin;bool have=false;
   if(t%20==0 && voice_bridge_room()>=320){int16_t pcm[320];have=engine_take(pcm);if(have)voice_bridge_inject(pcm,320);}
   bridge_key_update(have);
  }
  if(t%16==0){int16_t pcm[256];size_t n=voice_bridge_play(pcm,256);if(n){assert(rf_pin);if(first<0)first=pcm[0];last=pcm[n-1];}voice_bridge_network_written();}
  if(saw_key && !rf_pin && !voice_bridge_level()){assert(first==1000 && last==9000);return;}
  advance(1);
 }
 assert(!"batch failed to drain/release");
}
int main(void){
 setvbuf(stdout,NULL,_IONBF,0);rx_guard_init();ptt_init();fixture();
 int gaps[]={0,1000,5000,30000};int lengths[]={1,2,3,14,15,50};
 for(unsigned g=0;g<4;g++)for(unsigned n=0;n<6;n++)clip(lengths[n],gaps[g]);
 printf("PASS %s 24 real receive/voice clips: first+last samples, 300ms physical lead, short overs, 0/1/5/30s gaps\n",ENGINE);
 burst(32,0);burst(64,0);burst(64,800);
 printf("PASS %s 640/1280ms startup batches and 800ms late EOF drain in full\n",ENGINE);
 for(int i=0;i<1000;i++)clip(1+i%3,0);
 printf("PASS %s 1000 consecutive short overs: no residue, no head/tail loss\n",ENGINE);

 // The final sample and final I2S block both keep the transmitter reserved.
 fixture();int16_t one=777,out[256];voice_bridge_inject(&one,1);
 bridge_key_update(false);assert(rf_pin);advance(300);
 assert(voice_bridge_play(out,256)==1 && out[0]==777 && voice_bridge_level()==0);
 assert(voice_bridge_network_pending());advance(300);bridge_key_update(false);assert(rf_pin);
 voice_bridge_network_written();bridge_key_update(false);advance(251);bridge_key_update(false);
 assert(!rf_pin && !voice_bridge_network_pending());
 printf("PASS %s last interpolation sample and I2S completion hold RF\n",ENGINE);

 // A stopped consumer gets a bounded drain; stream cancel never erases SDR.
 fixture();int16_t buf[320];for(int i=0;i<320;i++)buf[i]=500;
 voice_bridge_inject(buf,320);bridge_key_update(false);assert(rf_pin);advance(2001);
 bridge_key_update(false);advance(251);bridge_key_update(false);assert(!rf_pin && voice_bridge_level()==0);
 assert(ptt_claim(PTT_OWNER_SDR,55));s_inject_owner=PTT_OWNER_SDR;s_inject_token=55;
 s_bridge_inj_head=320;s_bridge_inj_tail=0;voice_bridge_network_cancel();assert(voice_bridge_level()==320);
 ptt_release(PTT_OWNER_SDR,55);voice_inject_reset();
 printf("PASS %s bounded drain and SDR isolation\n",ENGINE);
 return 0;
}
'''

RF_ENV=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define OUT_MAX 258
#define RX_GUARD_OUT_NET 1
#define RX_GUARD_OUT_SDR 2
#define RX_GUARD_OUT_SPK 4
static int16_t s_rf_prefix[OUT_MAX],net[2048],phone[2048];
static size_t s_rf_prefix_n,net_n,phone_n;
static uint8_t s_rf_prefix_blocked;
static uint8_t s_rf_prefix_mode,current_mode=1;
static uint8_t netcom_mode(void){return current_mode;}
static int64_t s_rf_prefix_us;
static bool s_mod_sq_open;
static int s_sq_gpio=40,s_sq_gpio_run,s_net_rx_gain=16;
static void bridge_rx_inhibit(void){}
static void bridge_rx_tap(const int16_t *p,size_t n,bool open,bool enabled){if(open&&enabled){memcpy(net+net_n,p,n*2);net_n+=n;}}
static void sdr_rx_push(const int16_t *p,size_t n,bool open){if(open){memcpy(phone+phone_n,p,n*2);phone_n+=n;}}
'''
RF_TEST=r'''
static void reset(void){net_n=phone_n=s_rf_prefix_n=0;s_mod_sq_open=false;s_sq_gpio_run=1;}
int main(void){
 int16_t a[256],b[256];for(int i=0;i<256;i++){a[i]=1000+i;b[i]=2000+i;}
 reset();rf_forward_block(a,256,true,0,1000000);assert(!net_n&&!phone_n);
 s_mod_sq_open=true;s_sq_gpio_run=2;rf_forward_block(b,256,true,0,1016000);
 assert(net_n==512 && phone_n==512 && !memcmp(net,a,512) && !memcmp(net+256,b,512));
 assert(!memcmp(phone,net,1024));
 // Exercise the actual final DAC-mute conditional independently of fanout.
 bool rf_audio=true,speaker_settling=true;uint8_t rx_blocked=0;int16_t out[256];int m=256;
 memcpy(out,b,sizeof(out));SPEAKER_MUTE
 for(int i=0;i<256;i++)assert(out[i]==0);
 assert(net[0]==1000 && phone[0]==1000);
 puts("PASS hardware COS prefix and speaker mute preserve network/phone first samples");

 reset();rf_forward_block(a,256,true,1,1000000);s_mod_sq_open=true;
 rf_forward_block(b,256,true,0,1016000);assert(net_n==256 && net[0]==2000 && phone_n==512);
 reset();rf_forward_block(a,256,true,2,1000000);s_mod_sq_open=true;
 rf_forward_block(b,256,true,0,1016000);assert(phone_n==256 && phone[0]==2000 && net_n==512);
 reset();rf_forward_block(a,256,true,0,1000000);s_mod_sq_open=true;
 rf_forward_block(b,256,true,3,1016000);assert(!phone_n&&!net_n);
 puts("PASS capture-time/current guard masks prevent prefix replay across protection");

 reset();rf_forward_block(a,256,true,0,1000000);rf_forward_block(b,256,false,0,1016000);
 s_mod_sq_open=true;rf_forward_block(b,256,true,0,1032000);assert(net_n==256&&net[0]==2000);
 reset();rf_forward_block(a,256,true,0,1000000);s_mod_sq_open=true;
 rf_forward_block(b,256,true,0,1100000);assert(net_n==256&&net[0]==2000);
 reset();rf_forward_block(a,256,true,0,1000000);s_mod_sq_open=true;current_mode=2;
 rf_forward_block(b,256,true,0,1016000);assert(net_n==256&&net[0]==2000);
 puts("PASS TX/preview discontinuity and stale COS candidate discard old prefix");
 return 0;
}
'''

def body(source,needle):
    brace=source.index('{',source.index(needle))
    f=function('static void take(void) '+source[brace:],'take')
    return f[f.index('{'):]

def run(out):
    voice=(ROOT/'firmware/main/voice.c').read_text(encoding='utf-8')
    common=order.COMMON
    for stub in (
        'static void voice_bridge_network_state(bool feeding,bool rf){(void)feeding;(void)rf;}\n',
        'static bool voice_bridge_network_pending(void){return false;}\n',
        'static void voice_bridge_network_keyed(bool keyed){(void)keyed;}\n'):
        assert stub in common;common=common.replace(stub,'')
    common=common.replace('(void)p;(void)data;(void)n;memset(out,0,320*sizeof(*out));return 320;',
        '(void)p;(void)n;int16_t value=data?(int16_t)(data[0]|(data[1]<<8)):0;for(int i=0;i<320;i++)out[i]=value;return 320;')
    funcs=('clamp16','inject_locked','voice_inject_reset','voice_bridge_network_state','voice_bridge_network_keyed',
        'voice_bridge_network_pending','voice_bridge_network_ready','voice_bridge_network_written','voice_bridge_network_cancel',
        'voice_bridge_room','voice_bridge_inject','voice_bridge_level','voice_bridge_take_resampled','voice_bridge_play','voice_bridge_take')
    voice_code=VOICE+''.join(function(voice,n) for n in funcs)
    for name in ('rx_guard.c','rx_guard.h','ptt.c','ptt.h','net_rx_order.h'):
        shutil.copy2(ROOT/'firmware/main'/name,out/name)
    (out/'freertos').mkdir(exist_ok=True);(out/'stubs.h').write_text(STUBS,encoding='utf-8')
    for name in ('esp_err.h','esp_timer.h','esp_log.h','board_hw.h','sa818_cfg.h','nvs.h','freertos/FreeRTOS.h','freertos/semphr.h','freertos/task.h'):
        (out/name).write_text('#include "stubs.h"\n',encoding='utf-8')
    codes={}
    for engine in ('netp2pnet.c','netrelay.c'):
        src=(ROOT/'firmware/main'/engine).read_text(encoding='utf-8')
        if engine=='netp2pnet.c':
            production=''.join(function(src,n) for n in ('peer_reset_stream','jbuf_audio','handle_frame','peer_jbuf_next','select_active_peer','bridge_key_update'))
            post=order.NP_POST.replace('np_addr_t src={.fam=4', 'pkt[24]=(uint8_t)wire_pcm;pkt[25]=(uint8_t)(wire_pcm>>8);np_addr_t src={.fam=4')
            adapter='static bool engine_take(int16_t *out){int peak;rt_peer_t *p=select_active_peer();return p&&peer_jbuf_next(p,out,&peak);}\n'
            code=common+voice_code+order.NP_ENV+production+post+adapter
            mode=1
        else:
            production=''.join(function(src,n) for n in ('relay_source','relay_reset_stream','relay_store_packet','rel_jbuf_next','bridge_key_update'))
            post=order.REL_POST.replace('mqtt_event_t event=', 'pkt[14]=(uint8_t)wire_pcm;pkt[15]=(uint8_t)(wire_pcm>>8);mqtt_event_t event=')
            adapter='static bool pull_audio(void){int16_t out[320];int peak;return rel_jbuf_next(out,&peak);}\nstatic bool engine_take(int16_t *out){int peak;return rel_jbuf_next(out,&peak);}\n'
            code=common+voice_code+order.REL_ENV+production+post+order.mqtt_data_case(src)+'\n}\n}\n'+adapter
            mode=2
        codes[engine]=f'#define MODE {mode}\n#define ENGINE "{engine}"\n'+code+TEST
    mute='if (rf_audio && (speaker_settling || (rx_blocked & RX_GUARD_OUT_SPK))) '+body(voice,'if (rf_audio && (speaker_settling')
    codes['rf-prefix']=RF_ENV+function(voice,'clamp16')+function(voice,'gain_mul')+function(voice,'rf_forward_one')+function(voice,'rf_forward_block')+RF_TEST.replace('SPEAKER_MUTE',mute)
    results=[]
    for name,code in codes.items():
        c=out/(name+'.c');exe=out/(name+'.exe');c.write_text(code,encoding='utf-8')
        env=dict(os.environ,TEMP=str(out),TMP=str(out),TMPDIR=str(out))
        command=[shutil.which('gcc'),'-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable','-Wno-misleading-indentation','-I',str(out),str(c),'-o',str(exe)]
        r=subprocess.run(command,capture_output=True,text=True,env=env)
        (out/(name+'.compile.log')).write_text(r.stdout+r.stderr,encoding='utf-8')
        assert r.returncode==0,r.stderr
        r=subprocess.run([str(exe)],capture_output=True,text=True,env=env,timeout=30)
        (out/(name+'.run.log')).write_text(r.stdout+r.stderr,encoding='utf-8')
        print(r.stdout+r.stderr,end='');assert r.returncode==0
        results.append({'program':name,'command':command,'exit_code':r.returncode,'output':r.stdout})
    (out/'results.json').write_text(json.dumps(results,indent=2),encoding='utf-8')

def main():
    if os.environ.get('SPEECH_PREFIX_TEST_OUTPUT'):
        out=Path(os.environ['SPEECH_PREFIX_TEST_OUTPUT']).resolve();out.mkdir(parents=True,exist_ok=True);run(out);return
    scratch=ROOT/'.tmp';scratch.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='bridge-prefix-',dir=scratch) as temp:
        out=Path(temp);assert out.resolve().parent==scratch.resolve();run(out)
if __name__=='__main__':main()
