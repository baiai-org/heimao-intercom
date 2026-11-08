"""Verified OTA for explicitly identified boards. No RF tests.

Every board must be named with its expected ``device_id`` on the command line;
there are no built-in addresses. Snapshot and deploy are separate commands.
Existing controller leases are never taken over. Each deployment rereads
settings and saves the actual rollback BIN.
"""
import argparse
import hashlib
import json
import ssl
import time
import urllib.request
import uuid
from pathlib import Path

TARGETS={}

def parse_targets(values):
    targets={}
    for value in values:
        ip,sep,device=value.partition('=')
        if not sep or not ip.strip() or not device.strip():
            raise SystemExit('--target must be IP=DEVICE_ID')
        targets[ip.strip()]=device.strip()
    return targets

def sha(data):return hashlib.sha256(data).hexdigest()
def save(path,value):
    path.parent.mkdir(parents=True,exist_ok=True)
    path.write_text(json.dumps(value,ensure_ascii=False,indent=2),encoding='utf-8')

class Board:
    def __init__(self,ip):
        self.ip=ip;self.headers={'X-ORL-Device':TARGETS[ip]}
        self.opener=urllib.request.build_opener(urllib.request.ProxyHandler({}))
    def request(self,path,data=None,timeout=15):
        headers=dict(self.headers)
        if isinstance(data,dict):headers['Content-Type']='application/json';data=json.dumps(data).encode()
        elif data is not None:headers['Content-Type']='application/octet-stream'
        req=urllib.request.Request('http://'+self.ip+'/api/'+path,data=data,headers=headers)
        return self.opener.open(req,timeout=timeout).read()
    def get(self,path,data=None,timeout=15):return json.loads(self.request(path,data,timeout))
    def claim(self):
        lease=self.get('control',dict(op='claim',client='activity-ota-'+uuid.uuid4().hex,
            label='Verified firmware upgrade',device_id=TARGETS[self.ip]))
        assert lease.get('ok') and lease.get('mine'),'control occupied'
        # r11-33+/1.1.7+: the board has no control lease and returns no token.
        # Accept that mode; only send X-ORL-Control when the board issued one.
        if lease.get('token'):
            self.headers['X-ORL-Control']=lease['token']
    def release(self):
        try:self.get('control',{'op':'release'})
        except Exception:pass
        self.headers.pop('X-ORL-Control',None)
    def snapshot(self,folder):
        snap={name:self.get(name) for name in ['device','status','radio','live','wifi','channels','adc','speaker_diag']}
        assert snap['device']['device_id']==TARGETS[self.ip], 'device identity mismatch'
        for name,value in snap.items():save(folder/(name+'.json'),value)
        return snap

def protected(s):
    status=s['status']
    # Scan strength and retry counters change naturally across reboot; saved
    # profile IDs, credentials-presence, auto-join and priority must match.
    profiles=[{k:v for k,v in profile.items() if k not in ('rssi','failures','retry_in_ms')}
              for profile in s['wifi'].get('profiles',[])]
    return dict(freq=status['freq'],audio={k:status[k] for k in ['gain','txgain','ntxgain','cvol','spk','micgain','mic_db','adcvol','sqgate','eq_spk','eq_mic','tail']},
        intercom={k:status['intercom'][k] for k in ['mode','room','broker','user','pass']},
        bridge={k:status['bridge'][k] for k in ['en','chan','chan_name']},
        frontpanel={k:status['fp'][k] for k in ['chan','chan_n','act']},monitor=status['mode'],
        wifi={k:status['wifi'][k] for k in ['ssid','host']},saved_wifi=sorted(profiles,key=lambda item:item['id']),
        radio={k:s['radio'][k] for k in ['band','model','power_applied','power_control','hl_open_drain','hl_pull_up','hl_pull_down']},
        extras={k:s['live'][k] for k in ['bgtx','sqgpio','rf_tx']},channels=s['channels'])

def idle(s):
    status=s['status'];return status['ptt']=='IDLE' and not status['bridge']['keyed'] and not status['bridge']['rx_open'] and not status['sdr']['on'] and not status['sdr']['ptt'] and not s['live']['rf_key']

def main():
    global TARGETS
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('action',choices=['snapshot','deploy','verify'])
    p.add_argument('--target',action='append',default=[],metavar='IP=DEVICE_ID',
                   help='repeatable; verified board identity, e.g. 192.0.2.50=orl-0123456789ac')
    p.add_argument('--ip',required=True);p.add_argument('--output',required=True);p.add_argument('--image')
    args=p.parse_args();TARGETS=parse_targets(args.target)
    if args.ip not in TARGETS:raise SystemExit('--ip must match one of --target')
    out=Path(args.output)/args.ip;b=Board(args.ip)
    if args.action=='snapshot':
        before=b.snapshot(out/'before');assert idle(before),'board busy'
        rollback=b.request('firmware.bin',timeout=40)
        (out/'rollback.bin').write_bytes(rollback)
        save(out/'rollback-info.json',dict(sha256=sha(rollback),build=before['status']['build'],bytes=len(rollback)))
        print(args.ip,'snapshot/rollback saved',before['status']['build']['version'],before['radio']['band']);return
    image=Path(args.image).read_bytes();version=image[0x30:0x50].split(b'\0')[0].decode();elf=image[0xb0:0xd0].hex()
    before=json.loads((out/'before/status.json').read_text(encoding='utf-8'))
    if args.action=='deploy':
        fresh=b.snapshot(out/'preflight');assert idle(fresh),'board busy; no upload'
        expected={n:json.loads((out/'before'/(n+'.json')).read_text(encoding='utf-8')) for n in fresh}
        assert protected(fresh)==protected(expected),'settings changed since snapshot; no upload'
        assert b.request('firmware.bin',timeout=40)==(out/'rollback.bin').read_bytes(),'rollback image changed'
        b.claim()
        try:
            result=b.get('ota',image,180);assert result.get('ok'),result
            print(args.ip,'OTA accepted',flush=True)
        finally:b.release()
        deadline=time.monotonic()+100
        while time.monotonic()<deadline:
            time.sleep(2)
            try:
                status=b.get('status',timeout=3)
                if status['build']['sha256']==elf and b.get('radio')['ready']:break
            except Exception:pass
        else:raise RuntimeError('candidate did not return')
        # OTA safely stops runtime modes. Restore only those three switches;
        # never submit frequencies, power, tones, gains, channels or Wi-Fi.
        current=b.get('status');changes=[]
        if bool(current['bridge']['en'])!=bool(before['bridge']['en']):changes.append(('bridge',str(int(bool(before['bridge']['en'])))))
        if current['mode']!=before['mode']:
            assert before['mode'].startswith(('MONITOR','STOP','IDLE')),'unsupported runtime mode'
            changes.append(('monitor','1' if before['mode'].startswith('MONITOR') else '0'))
        if current['intercom']['mode']!=before['intercom']['mode']:changes.append(('icmode',str(before['intercom']['mode'])))
        if changes:
            b.claim()
            try:
                for key,value in changes:assert b.get('set',dict(k=key,v=value)).get('ok')
            finally:b.release()
        save(out/'restored-switches.json',changes)
    after=b.snapshot(out/'after')
    baseline={n:json.loads((out/'before'/(n+'.json')).read_text(encoding='utf-8')) for n in after}
    changed=[k for k,v in protected(baseline).items() if protected(after)[k]!=v]
    assert not changed, 'settings changed: '+str(changed)
    assert after['status']['build']['version']==version and after['status']['build']['sha256']==elf,'running build mismatch'
    running=b.request('firmware.bin',timeout=40);assert running==image,'full running application differs'
    assert after['device']['capabilities'].get('radio_activity')==1
    assert idle(after) and after['radio']['ready'] and after['radio']['compatible']
    samples=[]
    for _ in range(20):
        started=time.monotonic();sample=b.get('activity');elapsed=(time.monotonic()-started)*1000
        assert sample['device_id']==TARGETS[args.ip] and not sample['tx_active'],'unexpected TX'
        if samples:assert sample['boot_id']==samples[-1]['snapshot']['boot_id'] and sample['sample_us']>samples[-1]['snapshot']['sample_us']
        samples.append(dict(milliseconds=round(elapsed,2),snapshot=sample));time.sleep(.05)
    context=ssl._create_unverified_context() # existing LAN self-signed certificate; identity checked in JSON
    https=json.loads(urllib.request.urlopen('https://'+args.ip+'/api/activity',context=context,timeout=10).read())
    assert https['device_id']==TARGETS[args.ip] and https['boot_id']==samples[-1]['snapshot']['boot_id']
    final=b.get('status');assert final['uptime']>=after['status']['uptime'],'unexpected restart'
    assert final['ptt']=='IDLE' and not final['sdr']['on'] and final['sdr']['tx']==0 and final['intercom']['tx']==0 and final['tx_count']==0,'unexpected post-boot TX'
    result=dict(ip=args.ip,device_id=TARGETS[args.ip],version=version,elf_sha256=elf,bin_sha256=sha(running),
        settings_equal=True,full_running_image_equal=True,http_https_activity=True,radio_band=after['radio']['band'],
        radio_ready=True,no_transmit_test=True,samples=samples)
    save(out/'verification.json',result);print(args.ip,'PASS',version,after['radio']['band'],'settings and full running image match',flush=True)

if __name__=='__main__':main()
