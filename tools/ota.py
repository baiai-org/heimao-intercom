"""Push the current firmware build to a board over the network (no USB).

usage:
    python tools/ota.py <ip-or-host> [image]

    python tools/ota.py 192.0.2.198
    python tools/ota.py 192.0.2.178 ../firmware/build/openradiolink.bin

Why this exists: OTA is the normal way to update a board in this project.
It takes ~6 s, keeps the WiFi/peer configuration in NVS, and the board switches
itself to MODE 00 first (the OTA handler does that, see net_http.c h_ota) so a
running transmission cannot be corrupted mid-flash.

A serial flash is only needed for a board that cannot reach the network, or when
the partition table changes (tools/flash.ps1 -Action full).

The board reboots as soon as the image is written, so the POST usually ends with
a dropped connection -- that is expected, and the script then waits for the
board to answer /api/status again and prints what it came back as.
"""
import json
import os
import sys
import time
import urllib.request
import urllib.error
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_IMAGE = os.path.join(HERE, "..", "firmware", "build", "openradiolink.bin")

target = sys.argv[1] if len(sys.argv) > 1 else None
if not target:
    raise SystemExit("usage: python tools/ota.py <ip-or-host> [image]")
image = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_IMAGE

with open(image, "rb") as f:
    payload = f.read()

if len(payload) < 65536 or payload[0] != 0xe9:
    raise SystemExit('not an ESP application image')

# A v2 board requires the same exclusive lease as the App and web panel.
# A busy controller is never silently taken over by this tool.
headers = {"Content-Type": "application/octet-stream"}
device_id = None
try:
    identity = json.loads(urllib.request.urlopen("http://%s/api/device" % target, timeout=5).read())
except urllib.error.HTTPError as error:
    if error.code != 404:
        raise
else:
    if identity.get('api_version') != 2 or not identity.get('device_id'):
        raise SystemExit('unsupported device API')
    device_id = identity['device_id']
    claim = urllib.request.Request("http://%s/api/control" % target,
        data=json.dumps({'op':'claim','client':'ota-'+uuid.uuid4().hex,
                         'label':'Firmware OTA tool','device_id':device_id}).encode(),
        headers={'Content-Type':'application/json'}, method='POST')
    lease = json.loads(urllib.request.urlopen(claim, timeout=5).read())
    if not lease.get('ok'):
        raise SystemExit('could not acquire device control')
    # r11-51 removed the control lease: the board answers OK without a token and
    # no longer needs one, so upload without the header in that case.
    if lease.get('token'):
        headers.update({'X-ORL-Control': lease['token'], 'X-ORL-Device': device_id})

print("image  : %s (%d bytes)" % (os.path.normpath(image), len(payload)))
print("target : %s" % target)

req = urllib.request.Request(
    "http://%s/api/ota" % target,
    data=payload,
    method="POST",
    headers=headers,
)

t0 = time.time()
try:
    r = urllib.request.urlopen(req, timeout=180)
    response = r.read().decode()
    if not json.loads(response).get('ok'):
        raise SystemExit('OTA rejected: '+response[:120])
    print("response: %s %s  (%.1f s)" % (r.status, response[:120],
                                         time.time() - t0))
except urllib.error.HTTPError as e:
    raise SystemExit('OTA HTTP rejection: '+str(e.code))
except Exception as e:
    print("connection ended after %.1f s (%s) -- normal if the board rebooted"
          % (time.time() - t0, type(e).__name__))
finally:
    if device_id:
        try:
            release = urllib.request.Request('http://%s/api/control' % target,
                data=b'{"op":"release"}', headers={**headers,'Content-Type':'application/json'}, method='POST')
            urllib.request.urlopen(release, timeout=3).close()
        except Exception:
            pass

for _ in range(30):
    time.sleep(2)
    try:
        s = json.loads(urllib.request.urlopen(
            "http://%s/api/status" % target, timeout=5).read())
        running = urllib.request.urlopen("http://%s/api/firmware.bin" % target, timeout=20).read()
        if running != payload:
            continue
        if device_id:
            identity = json.loads(urllib.request.urlopen("http://%s/api/device" % target, timeout=5).read())
            if identity.get('device_id') != device_id:
                raise SystemExit('device identity changed after OTA')
        w = s.get("wifi", {})
        print("back: uptime=%s host=%s ip=%s ip6=%s" %
              (s.get("uptime"), w.get("host"), w.get("ip"), w.get("ip6")))
        print("PASS: complete running application equals uploaded image")
        break
    except Exception:
        continue
else:
    print("FAIL: running application was not verified; inspect board before retrying")
    sys.exit(1)
