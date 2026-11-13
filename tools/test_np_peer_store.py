#!/usr/bin/env python3
"""Actual versioned NVS peer codec, including old four-slot IPv4/IPv6 blobs."""
from pathlib import Path
import os,shutil,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
HARNESS=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "np_peer_store.h"
int main(void){
 unsigned char v1[4*15]={0},v2[4*27]={0},v3[NP_PEER_STORE_BYTES];np_peer_record_t out[16],records[16]={0};size_t count=99;
 for(int i=0;i<4;i++){
  sprintf((char*)v1+i*15,"old%d",i);v1[i*15+8]=i+1;v1[i*15+12]=0xd4;v1[i*15+13]=0x31;
  sprintf((char*)v2+i*27,"v6-%d",i);v2[i*27+8]=i%2?6:4;v2[i*27+9]=0x20;v2[i*27+24]=i+1;v2[i*27+25]=0xd4;v2[i*27+26]=0x31;
 }
 assert(np_peer_store_decode(v1,sizeof(v1),1,out,&count)&&count==4);
 assert(out[3].family==4 && out[3].port==54321 && out[3].address[0]==4);
 assert(np_peer_store_decode(v2,sizeof(v2),2,out,&count)&&count==4);
 assert(out[3].family==6 && out[3].address[15]==4 && out[3].port==54321);
 size_t n=np_peer_store_encode(out,count,v3,sizeof(v3));assert(n==112);
 assert(np_peer_store_decode(v3,n,3,records,&count)&&count==4);
 for(int i=0;i<4;i++)assert(!memcmp(records[i].name,out[i].name,8)&&!memcmp(records[i].address,out[i].address,16)&&records[i].family==out[i].family&&records[i].port==out[i].port);
 for(int i=4;i<16;i++){sprintf(records[i].name,"p%d",i);records[i].family=i%2?6:4;records[i].port=54321+i;records[i].address[15]=i;}
 n=np_peer_store_encode(records,16,v3,sizeof(v3));assert(n==NP_PEER_STORE_BYTES);
 assert(np_peer_store_decode(v3,n,3,out,&count)&&count==16 && out[15].port==54336);
 assert(!np_peer_store_encode(records,17,v3,sizeof(v3)));assert(!np_peer_store_decode(v3,n-1,3,out,&count));
 v3[1]=17;assert(!np_peer_store_decode(v3,n,3,out,&count));v3[1]=16;v3[2]=26;assert(!np_peer_store_decode(v3,n,3,out,&count));v3[2]=27;
 memset(v3+4,'x',8);assert(!np_peer_store_decode(v3,n,3,out,&count));
 assert(!np_peer_store_decode(v1,sizeof(v1)-1,1,out,&count));
 assert(np_peer_store_encode(NULL,0,v3,sizeof(v3))==4);assert(np_peer_store_decode(v3,4,3,out,&count)&&count==0);
 puts("PASS: legacy four-slot v1/v2 migration, IPv4 byte order/IPv6/ports, 16-slot v3 roundtrip, bounds/corruption/empty table");
}
'''
def main():
 (ROOT/'.tmp').mkdir(exist_ok=True)
 with tempfile.TemporaryDirectory(prefix='np-store-',dir=ROOT/'.tmp') as temp:
  d=Path(temp);(d/'test.c').write_text(HARNESS,encoding='utf-8');exe=d/'test.exe'
  env=dict(os.environ,TEMP=temp,TMP=temp,TMPDIR=temp)
  subprocess.run([shutil.which('gcc'),'-std=c11','-Wall','-Wextra','-Werror','-I',str(ROOT/'firmware/main'),str(ROOT/'firmware/main/np_peer_store.c'),str(d/'test.c'),'-o',str(exe)],check=True,env=env)
  subprocess.run([str(exe)],check=True,timeout=15)
if __name__=='__main__':main()
