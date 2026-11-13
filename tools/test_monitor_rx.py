#!/usr/bin/env python3
"""Host test of the production RF/network phone-monitor ring and arbitration."""
from pathlib import Path
import os, shutil, subprocess, tempfile
ROOT=Path(__file__).resolve().parents[1]
HARNESS=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "monitor_rx.h"
int main(void){
 int16_t ring[8],out[8];const int16_t rf[]={1,2,3},net[]={101,-202,303};monitor_rx_t m;
 monitor_rx_init(&m,ring,8);monitor_rx_mode(&m,1);
 monitor_rx_rf(&m,rf,3,true,0);assert(m.source==MONITOR_RX_RF);
 monitor_rx_network(&m,1,net,3,1000); // replace RF before any BGTX/MIC processing
 monitor_rx_rf(&m,NULL,0,false,2000); // closed COS or RF receive guard cannot erase NET
 monitor_rx_rf(&m,rf,3,true,3000); // network priority during playout gap
 assert(monitor_rx_take(&m,out,8)==3 && !memcmp(out,net,sizeof(net)));
 monitor_rx_network(&m,1,net,3,4000);monitor_rx_rf(&m,rf,3,true,254000);
 assert(monitor_rx_take(&m,out,8)==3 && !memcmp(out,rf,sizeof(rf))); // exact expiry, no extra latency
 monitor_rx_rf(&m,rf,3,true,300000);monitor_rx_rf(&m,NULL,0,false,300001);assert(!monitor_rx_take(&m,out,8));
 monitor_rx_network(&m,1,net,3,400000);monitor_rx_local_tx(&m,true);assert(!monitor_rx_take(&m,out,8));
 monitor_rx_network(&m,1,net,3,500000);monitor_rx_rf(&m,rf,3,true,600000);assert(!monitor_rx_take(&m,out,8));
 // Local PTT OFF request does not unmute: caller keeps local_tx through its tail/release.
 monitor_rx_network(&m,1,net,3,700000);assert(m.local_tx && !monitor_rx_take(&m,out,8));
 monitor_rx_local_tx(&m,false);monitor_rx_network(&m,1,net,3,800000);assert(monitor_rx_take(&m,out,8)==3);
 monitor_rx_network(&m,1,net,3,900000);monitor_rx_mode(&m,2);assert(!monitor_rx_take(&m,out,8));
 monitor_rx_network(&m,1,net,3,900001);assert(!monitor_rx_take(&m,out,8)); // old engine callback
 monitor_rx_network(&m,2,net,3,900002);monitor_rx_rf(&m,NULL,0,false,900003);
 assert(monitor_rx_take(&m,out,8)==3 && !memcmp(out,net,sizeof(net)));
 for(int i=0;i<4;i++)monitor_rx_network(&m,2,net,3,1000000+i);
 assert(m.head-m.tail==8 && m.drops==4);assert(monitor_rx_take(&m,out,8)==8);
 monitor_rx_mode(&m,0);monitor_rx_network(&m,2,net,3,1100000);assert(!monitor_rx_take(&m,out,8));
 monitor_rx_rf(&m,rf,3,true,1200000);assert(monitor_rx_take(&m,out,8)==3);
 assert(m.rf_samples>0 && m.network_samples>0);
 puts("PASS: network fan-out preserves raw PCM; RF close/guard cannot clear NET; source priority, local PTT/tail mute, mode/session reset, bounded backlog");
}
'''
def main():
 (ROOT/'.tmp').mkdir(exist_ok=True)
 with tempfile.TemporaryDirectory(prefix='monitor-rx-',dir=ROOT/'.tmp') as temp:
  d=Path(temp);(d/'test.c').write_text(HARNESS,encoding='utf-8')
  env=dict(os.environ,TEMP=temp,TMP=temp,TMPDIR=temp)
  exe=d/'test.exe'
  subprocess.run([shutil.which('gcc'),'-std=c11','-Wall','-Wextra','-Werror','-I',str(ROOT/'firmware/main'),str(ROOT/'firmware/main/monitor_rx.c'),str(d/'test.c'),'-o',str(exe)],check=True,env=env)
  subprocess.run([str(exe)],check=True,timeout=15)
if __name__=='__main__':main()
