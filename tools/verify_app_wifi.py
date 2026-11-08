"""Validate one development board's v2 APIs without transmitting RF.

Requires a pre-upgrade snapshot (status/radio/channels) and explicit target.
Only temporary disabled Wi-Fi profiles are written; current credentials and
radio settings are never modified. No PTT command or microphone PCM is sent.
"""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import secrets
import socket
import ssl
import struct
import time
import urllib.error
import urllib.request
import uuid


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('target')
    parser.add_argument('--before',type=Path,required=True)
    parser.add_argument('--out',type=Path,required=True)
    args=parser.parse_args()
    args.out.mkdir(parents=True,exist_ok=True)
    root=Path(__file__).resolve().parent.parent
    context=ssl.create_default_context(cafile=str(root/'firmware/main/servercert.pem'))
    # Pin the existing project self-signed certificate for this local test.
    context.check_hostname=False
    tests=[]

    def request(path,body=None,headers=None,secure=False,expected=200,raw=False):
        own=dict(headers or {})
        if body is not None: own['Content-Type']='application/json'
        req=urllib.request.Request(('https' if secure else 'http')+'://'+args.target+'/'+path,
            data=json.dumps(body).encode() if body is not None else None,headers=own)
        try:
            with urllib.request.urlopen(req,context=context,timeout=10) as response:
                code=response.status;data=response.read()
        except urllib.error.HTTPError as error:
            code=error.code;data=error.read()
        assert code==expected,(path,code,data[:100])
        return data if raw else json.loads(data)

    identity=request('api/device')
    assert identity['api_version']==2
    assert request('api/device',secure=True)['device_id']==identity['device_id']
    tests.append('HTTP/HTTPS stable identity')
    assert b'app-control.js' in request('',raw=True)
    assert b'ORLControl' in request('app-control.js',raw=True)
    assert b'form' in request('setup',raw=True)
    assert 'token' not in request('api/control')

    def claim(client,op='claim',expected=200,headers=None,secure=False):
        result=request('api/control',{'op':op,'client':client,'label':'Development verification '+client,
            'device_id':identity['device_id']},headers,secure,expected)
        if expected==200:
            assert result['ok'] and result.get('token')
            return {'X-ORL-Control':result['token'],'X-ORL-Device':identity['device_id']}
        return result

    active=claim('A-'+uuid.uuid4().hex)
    try:
        claim('B',expected=409,secure=True)
        request('api/set?k=__read_only_probe&v=0',expected=423)
        assert not request('api/set?k=__read_only_probe&v=0',headers=active)['ok']
        assert request('api/control',{'op':'renew'},active)['ok']
        tests.append('exclusive settings across HTTP/HTTPS and legacy GET guard')
        initial=request('api/wifi')
        assert initial['connected'] and not initial['ap'] and not initial['storage_error']
        assert initial['profiles'] and '"password":' not in json.dumps(initial)
        name='ORL-DEV-TEST-'+uuid.uuid4().hex[:8]
        temp_id=None
        try:
            saved=request('api/wifi',{'op':'save','ssid':name,'password':'','auto_join':False,'priority':0},active)
            assert saved['ok'];temp_id=saved['id']
            listed=request('api/wifi')
            assert len(listed['profiles'])==len(initial['profiles'])+1
            edited=request('api/wifi',{'op':'save','id':temp_id,'auto_join':False,'priority':3},active)
            assert edited['ok'] and edited['id']==temp_id
            listed=request('api/wifi');temp=next(n for n in listed['profiles'] if n['id']==temp_id)
            assert temp['priority']==3 and not temp['auto_join'] and not temp['has_password']
        finally:
            if temp_id: assert request('api/wifi',{'op':'forget','id':temp_id},active)['ok']
        after_profiles=request('api/wifi')
        assert after_profiles['connected'] and after_profiles['active_id']==initial['active_id']
        for field in ['id','ssid','auto_join','priority','has_password']:
            assert [n[field] for n in initial['profiles']]==[n[field] for n in after_profiles['profiles']]
        tests.append('legacy credential migration; add/edit/forget disabled profile without changing active Wi-Fi')
        old=active;active=claim('B','takeover')
        request('api/set?k=__read_only_probe&v=0',headers=old,expected=423)
        assert request('api/control',{'op':'release'},active)['ok'];active=None
        tests.append('explicit takeover, stale token rejection and release')
        active=claim('expiry')
        opened=request('api/sdr?on=1&client='+str(secrets.randbelow(2**32-1)+1),headers=active)
        assert opened['ok'] and request('api/status')['sdr']['on']
        time.sleep(15.5)
        # /api/status does not expire leases itself: disabled audio here proves
        # the independent lease worker revoked it without an API trigger.
        assert not request('api/status')['sdr']['on']
        assert not request('api/control')['held']
        request('api/set?k=__read_only_probe&v=0',headers=active,expected=423)
        active=None;tests.append('autonomous lease expiry')
        active=claim('receive-only')
        for secure in [False,True]:
            assert request('api/control',{'op':'renew'},active)['ok']
            opened=request('api/sdr?on=1&client='+str(secrets.randbelow(2**32-1)+1),headers=active)
            assert opened['ok'] and opened['sid'];sid=opened['sid']
            try:
                pcm=request('api/sdr_rx?sid='+str(sid),headers=active,raw=True)
                assert len(pcm)%2==0
                # An authenticated PING/PONG exercises the actual wrapped WS
                # frame handler. It cannot key PTT and carries no audio input.
                sock=socket.create_connection((args.target,443 if secure else 80),timeout=5)
                if secure: sock=context.wrap_socket(sock,server_hostname=args.target)
                try:
                    key=base64.b64encode(secrets.token_bytes(16)).decode()
                    lines=['GET /ws/sdr?sid='+str(sid)+' HTTP/1.1','Host: '+args.target,
                        'Upgrade: websocket','Connection: Upgrade','Sec-WebSocket-Version: 13','Sec-WebSocket-Key: '+key]
                    lines.extend(k+': '+v for k,v in active.items())
                    sock.sendall(('\r\n'.join(lines)+'\r\n\r\n').encode())
                    response=b''
                    while b'\r\n\r\n' not in response:
                        response+=sock.recv(1)
                        assert len(response)<4096
                    assert b' 101 ' in response
                    accepted=base64.b64encode(hashlib.sha1((key+'258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest())
                    assert accepted.lower() in response.lower()
                    mask=secrets.token_bytes(4);sock.sendall(b'\x89\x81'+mask+bytes([ord('q')^mask[0]]))
                    frame=sock.recv(2)
                    while len(frame)<2: frame+=sock.recv(2-len(frame))
                    assert frame[0]&15==10 and frame[1]==1 and sock.recv(1)==b'q'
                    mask=secrets.token_bytes(4);payload=struct.pack('!H',1000)
                    sock.sendall(b'\x88\x82'+mask+bytes(v^mask[i%4] for i,v in enumerate(payload)))
                finally: sock.close()
            finally:
                assert request('api/sdr?on=0&sid='+str(sid),headers=active)['ok']
        tests.append('receive-only HTTP PCM and authenticated WS/WSS PING/PONG; no PTT or microphone data')
    finally:
        if active:
            request('api/control',{'op':'release'},active)

    snapshots={name:request('api/'+name) for name in ['status','radio','channels','adc','live','wifi','device','control']}
    for name,data in snapshots.items():
        (args.out/(name+'.json')).write_text(json.dumps(data,ensure_ascii=False,indent=2),encoding='utf-8')
    before_status=json.loads((args.before/'status.json').read_bytes())
    assert snapshots['radio']==json.loads((args.before/'radio.json').read_bytes())
    assert snapshots['channels']==json.loads((args.before/'channels.json').read_bytes())
    for key in ['freq','eq_spk','eq_mic','gain','txgain','cvol','spk','micgain','adcvol','tail']:
        assert before_status[key]==snapshots['status'][key],('configuration changed',key)
    assert snapshots['status']['ptt']=='IDLE' and not snapshots['status']['sdr']['ptt']
    assert snapshots['adc']['running'] and snapshots['adc']['clock_exact']
    tests.append('radio/channels/audio/tail configuration preserved; ADC clock healthy; PTT idle')
    result={'target':args.target,'version':snapshots['status']['build']['version'],'device_id':identity['device_id'],
        'tests':tests,'heapInt':snapshots['status']['heapInt'],'heapIntMax':snapshots['status']['heapIntMax'],
        'no_rf_transmission':True,'no_microphone_upload':True,'status':'PASS'}
    (args.out/'result.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps(result,ensure_ascii=False),flush=True)


if __name__=='__main__':main()
