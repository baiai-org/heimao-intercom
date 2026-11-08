"""Check the production 270 Hz/3.7 kHz RX filter cascade and 2:1 decimation."""
import array
import cmath
import json
import math
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FS = 32000

HARNESS = r'''
#include "rx_band.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
int main(int argc, char **argv) {
    if (argc != 2) return 2;
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    rx_band_t band;
    rx_band_reset(&band);
    double hz = strtod(argv[1], NULL);
    for (int i = 0; i < 2 * RX_LOWCUT_RATE_HZ; ++i) {
        float x = 2048.0f;
        if (hz > 0) x += (float)(512.0 * sin(6.283185307179586 * hz * i / RX_LOWCUT_RATE_HZ));
        float y = rx_band_run(&band, x);
        if (!isfinite(y)) return 3;
        /* Same phase and scale as monitor_task: keep the second sample. */
        y /= 2;
        if (i >= RX_LOWCUT_RATE_HZ && (i & 1) && fwrite(&y, sizeof(y), 1, stdout) != 1) return 4;
    }
    rx_band_reset(&band);
    for (int i = 0; i < 100; ++i)
        if (rx_band_run(&band, 0) != 0) return 5;
    return 0;
}
'''


def coefficients(name):
    header = (ROOT / 'firmware/main' / name).read_text(encoding='utf-8')
    sos = []
    for row in re.findall(r'\{([^{}]+)\},', header):
        values = re.findall(r'(-?\d+(?:\.\d+)?(?:e[+-]?\d+)?)f', row)
        if len(values) == 5:
            sos.append([struct.unpack('<f', struct.pack('<f', float(x)))[0] for x in values])
    assert len(sos) == {'rx_lowcut.h': 7, 'rx_highcut.h': 6}[name]
    for _, _, _, a1, a2 in sos:
        root = cmath.sqrt(a1 * a1 - 4 * a2)
        assert max(abs((-a1 + root) / 2), abs((-a1 - root) / 2)) < 1
    return sos


def main():
    sos = coefficients('rx_lowcut.h') + coefficients('rx_highcut.h')

    def db(hz):
        z = cmath.exp(-2j * math.pi * hz / FS)
        h = 1.0 + 0j
        for b0, b1, b2, a1, a2 in sos:
            h *= (b0 + b1 * z + b2 * z * z) / (1 + a1 * z + a2 * z * z)
        return 20 * math.log10(max(abs(h), 1e-30))

    low_max = max(db(i / 100) for i in range(27001))
    high_max = max(db(3700 + i / 10) for i in range(123001))
    passed = [db(300 + i / 10) for i in range(31001)]
    assert low_max <= -80 and high_max <= -80, (low_max, high_max)
    assert min(passed) >= -0.23 and max(passed) <= 0.02
    print(json.dumps({'cascade_low_stop_max_db': low_max, 'cascade_high_stop_max_db': high_max,
                      'cascade_pass_min_db': min(passed), 'cascade_pass_max_db': max(passed)}, indent=2))
    env = os.environ.copy()
    scratch = ROOT / '.tmp'
    scratch.mkdir(exist_ok=True)
    env.update(TMP=str(scratch), TEMP=str(scratch))
    with tempfile.TemporaryDirectory(prefix='rx-band-', dir=scratch) as tmp:
        d = Path(tmp)
        source = d / 'response.c'
        source.write_text(HARNESS, encoding='utf-8')
        exe = d / 'response.exe'
        subprocess.run(['gcc', '-std=c11', '-O2', '-ffp-contract=off', '-Wall', '-Wextra',
                        '-Werror', '-I', str(ROOT / 'firmware/main'), str(source),
                        '-lm', '-o', str(exe)], check=True, env=env)
        # Roundoff depends on the input frequency: cover both stop bands as
        # well as their edges, not just the ideal coefficient response.
        tones = {0, 67, 88.5, 250.3, 260, 270, 280, 300, 350, 1000, 2000,
                 3000, 3400, 3500, 3600, 3700, 4000, 7000, 8100, 13000, 15911.5}
        tones.update(range(10, 270, 20))
        tones.update(range(3750, 16000, 250))
        for hz in sorted(tones):
            data = array.array('f')
            data.frombytes(subprocess.check_output([str(exe), str(hz)]))
            assert len(data) == FS // 2 and all(math.isfinite(v) for v in data)
            rms = math.sqrt(sum(v * v for v in data) / len(data))
            if hz == 0:
                assert max(abs(v) for v in data) < 0.001
                print('PASS: cascade DC settling and both filter state resets')
                continue
            measured = 20 * math.log10(max(rms / (256 / math.sqrt(2)), 1e-30))
            if hz <= 270 or hz >= 3700:
                assert measured <= -80, (hz, measured)
            elif 300 <= hz <= 3400:
                assert -0.23 <= measured <= 0.02, (hz, measured)
            print('PASS: production C cascade and decimation, %7.1f Hz: %8.3f dB' % (hz, measured))
    print('PASS: both stop bands, speech pass band, float32 stability and high-frequency alias rejection')


if __name__ == '__main__':
    main()
