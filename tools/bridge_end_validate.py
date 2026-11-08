#!/usr/bin/env python3
"""Verify on-board end-marker transport and playback release without RF.

Uses the existing network TALK path; bridge.c's RF edge itself is covered by
test_bridge_edges.py. Physical COS, RF release, and audio still need field tests.
Run after audit_validate.py snapshot/OTA, using the same external --results.
"""
import argparse
from pathlib import Path
import time

import audit_validate as audit


def wait_for(predicate, description, seconds=8):
    deadline = time.monotonic() + seconds
    while True:
        value = predicate()
        if value:
            return value
        if time.monotonic() > deadline:
            raise RuntimeError(description)
        time.sleep(.1)


def state(ip):
    s = audit.request(ip, '/api/status')
    live = audit.request(ip, '/api/live')
    if s['bridge']['en'] or s['bridge']['keyed'] or live['rf_key'] or live['rf_tx']:
        raise RuntimeError(ip + ' network-only guard failed')
    return s


def test_talk(source, mode, number):
    before = {ip: state(ip) for ip in audit.HOSTS}
    if mode == 2:
        recipients = [ip for ip in audit.HOSTS if ip != source]
    else:
        recipients = [p['ip'] for p in before[source]['np2p']['peers']
                      if p['ip'] in audit.HOSTS and p['state'] == 'linked']
    if not recipients:
        raise RuntimeError(source + ' has no recipients in saved topology')
    audit.request(source, '/api/talk?on=1&confirm=1&secs=8')
    # Exercise both a short and a longer stream, preserving each panel tail.
    time.sleep(.3 if number % 2 == 0 else 3)
    stopped = time.monotonic()
    audit.request(source, '/api/talk?on=0')

    def completed():
        s = state(source)
        return s if (s['ptt'] == 'IDLE' and
                     s['ptt_net']['tx_end_sent'] > before[source]['ptt_net']['tx_end_sent']) else None

    after_source = wait_for(completed, source + ' did not send its end marker')
    source_release_s = time.monotonic() - stopped
    pending = set(recipients)
    received = {}
    deadline = time.monotonic() + 5
    while pending:
        for ip in list(pending):
            s = state(ip)
            d = s['ptt_net']
            if (d['rx_end'] > before[ip]['ptt_net']['rx_end'] and
                    not d['rx_ptt'] and not d['rx_carrier'] and
                    d['rx_release_ms'] >= d['rx_end_ms']):
                received[ip] = s
                pending.remove(ip)
        if time.monotonic() > deadline:
            raise RuntimeError(source + ' recipients did not release: ' + ', '.join(pending))
        if pending:
            time.sleep(.1)
    tx = after_source['ptt_net']
    if tx['tx_end_req'] - before[source]['ptt_net']['tx_end_req'] != 1:
        raise RuntimeError(source + ' did not request exactly one end marker')
    if tx['tx_end_fail'] != before[source]['ptt_net']['tx_end_fail']:
        raise RuntimeError(source + ' failed to send an end marker')
    if tx['tx_delay_ms'] > 1000:
        raise RuntimeError(source + ' end marker queued/published for over 1 s')
    row = {'source': source, 'mode': mode, 'number': number,
           'source_release_s': round(source_release_s, 3),
           'tx_delay_ms': tx['tx_delay_ms'], 'receivers': {}}
    for ip, s in received.items():
        d = s['ptt_net']
        release_ms = d['rx_release_ms'] - d['rx_end_ms']
        if release_ms > 1000:
            raise RuntimeError(ip + ' playback release exceeded 1 s after PTT=0')
        if d['rx_timeout'] != before[ip]['ptt_net']['rx_timeout']:
            raise RuntimeError(ip + ' received no fresh PTT audio for 2 s during this call')
        row['receivers'][ip] = {'release_ms': release_ms, 'ptt_net': d}
    audit.event('end_marker_verified', **row)
    audit.say('%s mode=%d queue/publish=%d ms, receive-end to release: %s' % (
        source, mode, row['tx_delay_ms'], ', '.join(
            '%s %d ms' % (ip, value['release_ms']) for ip, value in row['receivers'].items())))
    return row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--results', type=Path, required=True)
    args = parser.parse_args()
    audit.RESULTS = args.results.resolve()
    audit.BASE = audit.json.loads((audit.RESULTS / 'initial.json').read_text(encoding='utf-8'))
    rows = []
    try:
        audit.prepare()
        for mode in (1, 2):
            audit.PHASE = 'end-marker-' + ('p2p' if mode == 1 else 'mqtt')
            audit.mode_all(mode, sdr=True)
            for number in range(6):
                rows.append(test_talk(audit.HOSTS[number % 3], mode, number))
                audit.save('end-marker-results.json', rows)
            audit.summary()
    finally:
        audit.restore()


if __name__ == '__main__':
    main()
