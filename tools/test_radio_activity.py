"""Run the production read-only activity serializer against simulated hardware."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parent.parent
HEADERS={
 'board_hw.h':'#include <stdbool.h>\nbool board_ptt_is_asserted(void);',
 'device_identity.h':'const char *device_identity_id(void);',
 'esp_random.h':'#include <stdint.h>\nuint32_t esp_random(void);',
 'esp_timer.h':'#include <stdint.h>\nint64_t esp_timer_get_time(void);',
 'netcom.h':'#include <stdbool.h>\n#define NETCOM_MODE_LOCAL 0\nint netcom_mode(void);\nbool netcom_tx_ready(void);',
 'ptt.h':'#define PTT_TX 2\nint ptt_get_state(void);',
}
HARNESS=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "radio_activity.c"
static radio_led_state_t leds;
static bool keyed, ready;
static int mode, ptt;
static int64_t stamp=1000000;
static int random_calls;
uint32_t esp_random(void){++random_calls;return 0x1234abcd;}
int64_t esp_timer_get_time(void){return stamp++;}
bool board_ptt_is_asserted(void){return keyed;}
const char *device_identity_id(void){return "board-A";}
int netcom_mode(void){return mode;}
bool netcom_tx_ready(void){return ready;}
int ptt_get_state(void){return ptt;}
void radio_led_get_state(radio_led_state_t *out){*out=leds;}
static void emit(void){
    struct {char json[RADIO_ACTIVITY_JSON_SIZE];unsigned char guard[16];} b;
    memset(&b,0xa5,sizeof(b));
    radio_activity_json(b.json,sizeof(b.json));
    assert(strlen(b.json)<sizeof(b.json));
    for(int i=0;i<16;i++)assert(b.guard[i]==0xa5);
    puts(b.json);
}
int main(void){
    radio_activity_init();radio_activity_init();assert(random_calls==1);
    emit(); /* idle */
    leds.cos_valid=true;leds.rf_rx=true;emit(); /* debounced RF RX */
    keyed=true;emit(); /* bridge hardware TX suppresses RF RX */
    keyed=false;leds.rf_rx=false;ptt=PTT_TX;mode=1;ready=true;emit(); /* network-only PTT */
    ptt=0;leds.net_rx=true;leds.net_tx=true;leds.rx=false;leds.tx=false;emit(); /* not blink phase */
    ready=false;emit(); /* stale network flags ignored after disconnect */
    ready=true;mode=0;emit(); /* mode change ignores previous engine */
    leds.net_rx=leds.net_tx=false;leds.rf_rx=true;leds.cos_valid=false;emit(); /* invalid COS */
    mode=2;ready=true;
    char a[RADIO_ACTIVITY_JSON_SIZE],b[RADIO_ACTIVITY_JSON_SIZE];
    uint32_t sig=radio_activity_json(a,sizeof(a));
    assert(sig==radio_activity_json(b,sizeof(b)) && strcmp(a,b));
}
'''

def main():
    with tempfile.TemporaryDirectory(prefix='activity-',dir=ROOT/'.tmp') as temp:
        directory=Path(temp)
        for name in ('radio_activity.c','radio_activity.h','radio_led.h'):
            shutil.copy2(ROOT/'firmware/main'/name,directory/name)
        for name,text in HEADERS.items():(directory/name).write_text(text,encoding='utf-8')
        (directory/'test.c').write_text(HARNESS,encoding='utf-8')
        exe=directory/'test.exe';env=dict(os.environ,TEMP=temp,TMP=temp,TMPDIR=temp)
        subprocess.run([shutil.which('gcc'),'-std=c11','-Wall','-Wextra','-Werror','-I',temp,str(directory/'test.c'),'-o',str(exe)],check=True,env=env)
        frames=[json.loads(s) for s in subprocess.check_output([str(exe)],text=True).splitlines()]
        assert [(s['tx_active'],s['rx_active']) for s in frames]==[(False,False),(False,True),(True,False),(True,False),(True,True),(False,False),(False,False),(False,False)]
        assert all(s['boot_id']=='1234abcd' and s['device_id']=='board-A' and s['type']=='activity' for s in frames)
        assert all(a['sample_us']<b['sample_us'] for a,b in zip(frames,frames[1:]))
        print('PASS: actual activity serializer; RF bridge/network-only TX; steady network flags; mode/disconnect/COS validity; boot/timestamp; bounded buffer')

if __name__=='__main__':main()
