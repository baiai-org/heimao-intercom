#!/usr/bin/env python3
"""Reproducible LAN-only acceptance run; never writes radio profiles or channels.

Use the project Python: tools/audit_validate.py --results <external-dir>
    snapshot | prepare | ota <IP> | lifecycle | smoke | stress | restore
Keep the initial snapshot until the operator accepts the firmware. Network-only
talks require bridge_en=0 and ix_rf=0 on ALL boards. No SDR PTT/test is sent.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import threading
import time
import urllib.parse
import urllib.request

# Boards under test, comma separated.  There are no built-in addresses:
#   ORL_HOSTS=192.0.2.50,192.0.2.198 python tools/audit_validate.py snapshot
HOSTS = tuple(h.strip() for h in os.environ.get('ORL_HOSTS', '').split(',') if h.strip())
BANDS = ('bass', 'warm', 'body', 'clear', 'pres', 'air')
WRITE_KEYS = {'ix_mode', 'ix_rf', 'bridge_en', 'monitor', 'bridge_test'} | {
    'eq_' + d + '_' + b for d in ('spk', 'mic') for b in BANDS}
ROOT = Path(__file__).resolve().parent.parent
OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))
LOCK = threading.Lock()
STOP = threading.Event()
STATS = {}
LATENCIES = {}
RESULTS = None
BASE = None
PHASE = 'setup'


def save(name, value):
    with (RESULTS / name).open('w', encoding='utf-8', newline='\n') as f:
        json.dump(value, f, ensure_ascii=False, indent=2)


def event(kind, **fields):
    with LOCK:
        with (RESULTS / 'events.jsonl').open('a', encoding='utf-8') as f:
            f.write(json.dumps(dict(time=time.time(), phase=PHASE, kind=kind,
                                    **fields), ensure_ascii=False) + '\n')


def say(message):
    print(time.strftime('%H:%M:%S'), message, flush=True)


def request(ip, path, data=None, decode=True, timeout=8):
    assert ip in HOSTS
    # Paths are constructed inside this module; no arbitrary /api/set allowed.
    req = urllib.request.Request('http://' + ip + path, data=data,
                                 headers={'Content-Type': 'application/octet-stream'})
    t0 = time.monotonic()
    label = path.split('?')[0]
    try:
        with OPENER.open(req, timeout=timeout) as response:
            body = response.read()
        result = json.loads(body) if decode else body
        if isinstance(result, dict) and result.get('ok') is False:
            raise RuntimeError('API refused: ' + str(result.get('error', result.get('msg', 'unknown'))))
    except Exception as exc:
        with LOCK:
            key = PHASE + ':' + ip + ':' + label + ':fail'
            STATS[key] = STATS.get(key, 0) + 1
        event('http_error', ip=ip, path=label, error=type(exc).__name__)
        raise
    with LOCK:
        key = PHASE + ':' + ip + ':' + label
        STATS[key + ':ok'] = STATS.get(key + ':ok', 0) + 1
        LATENCIES.setdefault(key, []).append(time.monotonic() - t0)
    return result


def set_key(ip, key, value):
    assert key in WRITE_KEYS, 'radio/channel writes are forbidden'
    return request(ip, '/api/set?' + urllib.parse.urlencode({'k': key, 'v': value}))


def protected(s, channels):
    return {'freq': s['freq'], 'channels': channels,
            'fp_chan': s['fp']['chan'], 'bridge_chan': s['bridge']['chan'],
            'levels': {k: s[k] for k in ('gain', 'txgain', 'cvol', 'spk',
                                        'micgain', 'adcvol', 'sqgate')}}


def check_protected(ip, s=None):
    s = s or request(ip, '/api/status')
    channels = request(ip, '/api/channels')
    actual = protected(s, channels)
    expected = protected(BASE[ip]['status'], BASE[ip]['channels'])
    if actual != expected:
        save('protected-mismatch-' + ip + '.json', {'before': expected, 'after': actual})
        STOP.set()
        raise RuntimeError(ip + ' radio/channel/level snapshot changed; no automatic RF write attempted')


def snapshot():
    if (RESULTS / 'initial.json').exists():
        raise RuntimeError('initial snapshot already exists; refusing to overwrite')
    data = {}
    for ip in HOSTS:
        s = request(ip, '/api/status')
        live = request(ip, '/api/live')
        if s['ptt'] != 'IDLE' or s['bridge']['keyed'] or live['rf_key']:
            raise RuntimeError(ip + ' is busy; snapshot requires idle boards')
        data[ip] = {'status': s, 'live': live,
                    'channels': request(ip, '/api/channels')}
    save('initial.json', data)
    say('Initial configuration saved outside the worktree')


def verify_safe():
    for ip in HOSTS:
        s = request(ip, '/api/status')
        live = request(ip, '/api/live')
        if s['bridge']['en'] or s['bridge']['keyed'] or live['rf_tx'] or live['rf_key']:
            raise RuntimeError(ip + ' network-only guard failed')
        check_protected(ip, s)


def prepare():
    for ip in HOSTS:
        s = request(ip, '/api/status')
        if s['ptt'] != 'IDLE':
            raise RuntimeError(ip + ' is transmitting')
        set_key(ip, 'bridge_en', 0)
        set_key(ip, 'ix_rf', 0)
    verify_safe()


def restore():
    errors = []
    for ip in HOSTS:
        try:
            request(ip, '/api/talk?on=0')
            deadline = time.monotonic() + 8
            while request(ip, '/api/status')['ptt'] != 'IDLE':
                if time.monotonic() > deadline:
                    raise RuntimeError('talk did not release')
                time.sleep(.2)
            request(ip, '/api/sdr?on=0')
            # Validate before restoring switches: do not enable RF on an
            # unexpectedly changed profile.
            check_protected(ip)
            original = BASE[ip]['status']
            for d in ('spk', 'mic'):
                for b, value in zip(BANDS, original['eq_' + d]):
                    set_key(ip, 'eq_' + d + '_' + b, value)
            set_key(ip, 'ix_mode', original['intercom']['mode'])
            set_key(ip, 'monitor', int('MONITOR' in original['mode']))
            request(ip, '/api/sdr?on=' + str(int(original['sdr']['on'])))
            set_key(ip, 'ix_rf', int(BASE[ip]['live']['rf_tx']))
            set_key(ip, 'bridge_en', original['bridge']['en'])
            s = request(ip, '/api/status')
            live = request(ip, '/api/live')
            check_protected(ip, s)
            assert s['eq_spk'] == original['eq_spk'] and s['eq_mic'] == original['eq_mic']
            assert s['intercom']['mode'] == original['intercom']['mode']
            assert s['bridge']['en'] == original['bridge']['en']
            assert s['sdr']['on'] == original['sdr']['on']
            assert live['rf_tx'] == BASE[ip]['live']['rf_tx']
            assert ('MONITOR' in s['mode']) == ('MONITOR' in original['mode'])
            save('restored-' + ip + '.json', {'status': s, 'live': live,
                                             'channels': request(ip, '/api/channels')})
            say(ip + ' configuration restored and compared')
        except Exception as exc:
            errors.append(ip + ': ' + str(exc))
    save('restore-result.json', {'errors': errors, 'time': time.time()})
    if errors:
        raise RuntimeError('; '.join(errors))


def ota(ip):
    verify_safe()
    image = ROOT / 'firmware/build/openradiolink.bin'
    payload = image.read_bytes()
    assert payload[0] == 0xE9 and struct.unpack_from('<I', payload, 32)[0] == 0xABCD5432
    expected = payload[176:208].hex()
    build_dir = RESULTS / 'candidate'
    build_dir.mkdir(exist_ok=True)
    for name in ('openradiolink.bin', 'openradiolink.elf'):
        dest = build_dir / name
        src = ROOT / 'firmware/build' / name
        if dest.exists() and dest.read_bytes() != src.read_bytes():
            raise RuntimeError('candidate artifact already exists with a different hash')
        if not dest.exists():
            shutil.copy2(src, dest)
    manifest = {'image_sha256': hashlib.sha256(payload).hexdigest(),
                'elf_sha256': expected, 'bytes': len(payload)}
    save('candidate-hashes.json', manifest)
    before = request(ip, '/api/status')
    say(ip + ' OTA start, expected ELF SHA256 ' + expected)
    result = subprocess.run([sys.executable, str(ROOT / 'tools/ota.py'), ip, str(image)],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=270)
    (RESULTS / ('ota-' + ip + '.txt')).write_bytes(result.stdout)
    deadline = time.monotonic() + 90
    while True:
        try:
            s = request(ip, '/api/status')
            if s.get('build', {}).get('sha256') == expected:
                break
        except Exception:
            pass
        if time.monotonic() > deadline:
            raise RuntimeError(ip + ' OTA did not boot the expected firmware')
        time.sleep(2)
    assert s['uptime'] < before['uptime'], 'no reboot observed'
    check_protected(ip, s)
    time.sleep(5)
    later = request(ip, '/api/status')
    assert later['uptime'] >= s['uptime'], 'reboot loop observed'
    assert later['build']['sha256'] == expected
    event('ota_verified', ip=ip, build=manifest, uptime=later['uptime'])
    say(ip + ' OTA verified by firmware hash; protected settings unchanged')


def mode_all(mode, sdr=False):
    for ip in HOSTS:
        request(ip, '/api/sdr?on=0')
        set_key(ip, 'ix_mode', mode)
        set_key(ip, 'monitor', 1)
        request(ip, '/api/sdr?on=' + str(int(sdr)))
    deadline = time.monotonic() + 45
    pending = set(HOSTS)
    while pending:
        for ip in list(pending):
            s = request(ip, '/api/status')
            live = request(ip, '/api/live')
            if s['intercom']['mode'] == mode and (mode == 0 or live['net_ready']):
                pending.remove(ip)
        if pending:
            if time.monotonic() >= deadline:
                raise RuntimeError('network not ready: ' + ', '.join(sorted(pending)))
            time.sleep(.5)
    verify_safe()


def lifecycle():
    global PHASE
    PHASE = 'lifecycle'
    prepare()
    rows = []
    for cycle in range(10):
        for mode in (2, 1):
            mode_all(mode)
            for ip in HOSTS:
                request(ip, '/api/sdr?on=1')
                request(ip, '/api/sdr?on=0')
                set_key(ip, 'bridge_test', 3000)
                # Stop a still-active feeder and exercise task ownership.
                set_key(ip, 'ix_mode', 0)
        time.sleep(2)
        row = {ip: request(ip, '/api/status') for ip in HOSTS}
        for ip, s in row.items():
            assert s['ptt'] == 'IDLE' and not s['bridge']['keyed']
            check_protected(ip, s)
        rows.append(row)
        save('lifecycle.json', rows)
        say('Lifecycle %d/10: %s' % (cycle + 1, ', '.join(
            '%s int=%d psram=%d' % (ip, s['heapInt'], s['psram']) for ip, s in row.items())))
    # Exclude the first two cycles which allocate reusable codec/jitter state.
    for ip in HOSTS:
        start, end = rows[2][ip]['psram'], rows[-1][ip]['psram']
        if start - end > 8192:
            raise RuntimeError(ip + ' repeated mode switches show PSRAM loss >8 KiB')
    event('lifecycle_pass', cycles=len(rows))


def status_record(ip, s, live):
    # Keep samples compact. Diagnostics can contain topology addresses, so
    # all detailed results stay outside the source checkout.
    result = {k: s[k] for k in ('uptime', 'rst', 'mode', 'ptt', 'heapInt',
                                'heapIntMax', 'psram', 'clip', 'ovf', 'underruns')}
    result.update(ip=ip, time=time.time(), phase=PHASE,
                  intercom={k: s['intercom'][k] for k in ('mode', 'state', 'tx', 'rx', 'peers', 'qdrop')},
                  diag=s['intercom']['diag'], sdr=s['sdr'],
                  ptt_net=s.get('ptt_net'), bridge_rx={
                      k: v for k, v in s['bridge'].items() if k.startswith('rx_')},
                  wifi_disc=s['wifi']['disc'], bridge_keyed=s['bridge']['keyed'],
                  rf_key=live['rf_key'], np2p_drops=s.get('np2p', {}).get('drops'))
    result['talk_stack_min'] = [int(n) for n in re.findall(
        r'TALK stack_min=(\d+) bytes', s.get('logx', ''))]
    with LOCK:
        with (RESULTS / 'samples.jsonl').open('a', encoding='utf-8') as f:
            f.write(json.dumps(result, ensure_ascii=False) + '\n')


def poll_worker(ip, end, prior, require_capture):
    try:
        prev = prior['uptime']
        idle_since = None
        last_rx = prior['sdr']['rsmp']
        rx_progress = time.monotonic()
        due = time.monotonic()
        while not STOP.is_set() and time.monotonic() < end:
            s = request(ip, '/api/status')
            live = request(ip, '/api/live')
            status_record(ip, s, live)
            if s['uptime'] < prev:
                raise RuntimeError(ip + ' unexpectedly rebooted')
            prev = s['uptime']
            if s['bridge']['keyed'] or live['rf_key'] or live['rf_tx']:
                raise RuntimeError(ip + ' RF guard failed')
            if not s['sdr']['on']:
                raise RuntimeError(ip + ' SDR unexpectedly disabled')
            # HTTP success alone does not prove that ADC capture is alive.
            # Own TALK pauses capture for 20 s. Continuous injection in the
            # full-load phases intentionally suppresses the RX echo tap.
            if s['sdr']['rsmp'] != last_rx:
                last_rx = s['sdr']['rsmp']
                rx_progress = time.monotonic()
            elif require_capture and time.monotonic() - rx_progress > 30:
                raise RuntimeError(ip + ' SDR delivered no new samples for 30 s')
            if s['ptt'] == 'IDLE':
                if idle_since is None:
                    idle_since = time.monotonic()
                if time.monotonic() - idle_since > 3 and 'MONITOR' not in s['mode']:
                    raise RuntimeError(ip + ' SDR capture did not resume after talk')
            else:
                idle_since = None
            # Read-only profile checks on every sample; channels at boundaries.
            expected = BASE[ip]['status']
            if s['freq'] != expected['freq'] or s['fp']['chan'] != expected['fp']['chan']:
                raise RuntimeError(ip + ' radio profile changed')
            due += 2
            STOP.wait(max(0, min(due, end) - time.monotonic()))
    except Exception as exc:
        event('fatal', ip=ip, error=str(exc))
        STOP.set()
        raise


def sdr_worker(ip, end, inject):
    due = time.monotonic()
    # 125 ms at 16 kHz, PCM16LE. Silence cannot trigger a key; PTT is never sent.
    payload = bytes(4000)
    while not STOP.is_set() and time.monotonic() < end:
        try:
            request(ip, '/api/sdr_rx', decode=False)
            if inject:
                request(ip, '/api/sdr_tx', data=payload, decode=False)
        except Exception:
            STOP.set()
            raise
        due += .125 if inject else 1
        STOP.wait(max(0, min(due, end) - time.monotonic()))


def eq_worker(end):
    tick = 0
    while not STOP.is_set() and time.monotonic() < end:
        ip = HOSTS[tick % len(HOSTS)]
        d = ('mic', 'spk')[(tick // 3) % 2]
        b = BANDS[(tick // 6) % 6]
        original = BASE[ip]['status']['eq_' + d][BANDS.index(b)]
        value = original - 1 if original > -12 else original + 1
        try:
            set_key(ip, 'eq_' + d + '_' + b, value if (tick // 36) % 2 == 0 else original)
        except Exception:
            STOP.set()
            raise
        tick += 1
        STOP.wait(max(0, min(20, end - time.monotonic())))


def talk_worker(end, seconds):
    index = 0
    while not STOP.is_set() and time.monotonic() + seconds + 4 < end:
        ip = HOSTS[index % 3]
        response = request(ip, '/api/talk?on=1&confirm=1&secs=' + str(seconds + 5))
        event('talk_start', ip=ip, seconds=seconds, result=response)
        STOP.wait(seconds)
        t0 = time.monotonic()
        request(ip, '/api/talk?on=0')
        while request(ip, '/api/live')['ptt'] != 'IDLE':
            if time.monotonic() - t0 > 8:
                STOP.set()
                raise RuntimeError(ip + ' stuck transmitting')
            time.sleep(.1)
        event('talk_end', ip=ip, release_s=round(time.monotonic() - t0, 3))
        index += 1
        # The four-second gap includes tail/release time.
        STOP.wait(max(0, 4 - (time.monotonic() - t0)))


def run_phases(phases):
    global PHASE
    prepare()
    for label, mode, duration, full, talk_s in phases:
        PHASE = label
        STOP.clear()
        mode_all(mode, sdr=True)
        start = {ip: request(ip, '/api/status') for ip in HOSTS}
        event('phase_start', duration=duration, mode=mode, sdr_tx=full)
        say(label + ' started (' + str(duration) + ' seconds)')
        end = time.monotonic() + duration
        futures = []
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            for ip in HOSTS:
                futures.append(pool.submit(poll_worker, ip, end, start[ip], not full))
                futures.append(pool.submit(sdr_worker, ip, end, full))
            futures.append(pool.submit(eq_worker, end))
            futures.append(pool.submit(talk_worker, end, talk_s))
            while not STOP.is_set() and time.monotonic() < end:
                # Observe worker failures immediately, not just at phase end.
                for f in futures:
                    if f.done() and f.exception():
                        STOP.set()
                        raise f.exception()
                STOP.wait(min(30, max(0, end - time.monotonic())))
                say(label + ' remaining %.0f s' % max(0, end - time.monotonic()))
            for f in futures:
                f.result()
        if STOP.is_set():
            raise RuntimeError(label + ' aborted')
        finish = {ip: request(ip, '/api/status') for ip in HOSTS}
        for ip in HOSTS:
            check_protected(ip, finish[ip])
        save(label + '.json', {'start': start, 'finish': finish, 'duration': duration})
        event('phase_complete', duration=duration)
        summary()


def smoke():
    run_phases((('smoke-p2p', 1, 120, True, 3),))


def stress():
    run_phases((('full-p2p', 1, 600, True, 10), ('full-mqtt', 2, 360, True, 10),
                ('talk-p2p', 1, 900, False, 20), ('talk-mqtt', 2, 480, False, 20)))


def summary():
    with LOCK:
        lat = {k: {'count': len(v), 'max_s': max(v),
                   'p95_s': sorted(v)[min(len(v) - 1, int(len(v) * .95))]}
               for k, v in LATENCIES.items()
               if k.startswith(PHASE + ':') and v}
        counts = {k: v for k, v in STATS.items()
                  if k.startswith(PHASE + ':')}
        data = {'counts': counts, 'latency': lat, 'time': time.time()}
    save('http-summary-' + PHASE + '.json', data)


def main():
    global RESULTS, BASE
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--results', type=Path, required=True)
    parser.add_argument('command', choices=('snapshot', 'prepare', 'ota', 'lifecycle', 'smoke', 'stress', 'restore'))
    parser.add_argument('ip', nargs='?', choices=HOSTS)
    args = parser.parse_args()
    RESULTS = args.results.resolve()
    assert ROOT not in RESULTS.parents and RESULTS != ROOT, 'use an external result directory'
    RESULTS.mkdir(parents=True, exist_ok=True)
    if args.command == 'snapshot':
        snapshot()
        return
    BASE = json.loads((RESULTS / 'initial.json').read_text(encoding='utf-8'))
    try:
        if args.command == 'ota':
            if not args.ip:
                parser.error('ota requires an IP')
            ota(args.ip)
        elif args.command == 'prepare':
            prepare()
        elif args.command == 'restore':
            restore()
        else:
            try:
                globals()[args.command]()
            finally:
                STOP.set()
                restore()
    except Exception as exc:
        event('command_failed', command=args.command, error=str(exc))
        raise
    finally:
        summary()


if __name__ == '__main__':
    main()
