"""Verify the production float32 RX filter's stop band, pass band and settling.

Uses only Python's standard library and a C compiler (gcc on PATH).
"""
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
#include "rx_lowcut.h"
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
    rx_lowcut_t f;
    rx_lowcut_reset(&f);
    double hz = strtod(argv[1], NULL);
    for (int i = 0; i < 2 * RX_LOWCUT_RATE_HZ; ++i) {
        float x = 2048.0f;
        if (hz > 0) x += (float)(512.0 * sin(6.283185307179586 * hz * i / RX_LOWCUT_RATE_HZ));
        float y = rx_lowcut_run(&f, x);
        if (!isfinite(y)) return 3;
        if (i >= RX_LOWCUT_RATE_HZ && fwrite(&y, sizeof(y), 1, stdout) != 1) return 4;
    }
    rx_lowcut_reset(&f);
    for (int i = 0; i < 100; ++i) if (rx_lowcut_run(&f, 0) != 0) return 5;
    return 0;
}
'''


def main():
    header = (ROOT / 'firmware/main/rx_lowcut.h').read_text(encoding='utf-8')
    rows = re.findall(r'\{([^{}]+)\},', header)
    sos = []
    for row in rows:
        coefficients = re.findall(r'(-?\d+(?:\.\d+)?(?:e[+-]?\d+)?)f', row)
        if len(coefficients) == 5:
            sos.append([struct.unpack('<f', struct.pack('<f', float(x)))[0] for x in coefficients])
    assert len(sos) == 7
    for _, _, _, a1, a2 in sos:
        root = cmath.sqrt(a1 * a1 - 4 * a2)
        assert max(abs((-a1 + root) / 2), abs((-a1 - root) / 2)) < 1

    def db(hz):
        z = cmath.exp(-2j * math.pi * hz / FS)
        h = 1.0 + 0j
        for b0, b1, b2, a1, a2 in sos:
            h *= (b0 + b1 * z + b2 * z * z) / (1 + a1 * z + a2 * z * z)
        return 20 * math.log10(max(abs(h), 1e-30))

    stop_max = max(db(i / 100) for i in range(27001))
    passed = [db(300 + i / 10) for i in range(int((FS / 2 - 300) * 10) + 1)]
    assert stop_max <= -80, stop_max
    assert min(passed) >= -0.12 and max(passed) <= 0.02
    print(json.dumps({'quantized_stop_max_db': stop_max,
                      'quantized_pass_min_db': min(passed),
                      'quantized_pass_max_db': max(passed)}, indent=2))

    scratch = ROOT / '.tmp'
    scratch.mkdir(exist_ok=True)
    env = os.environ.copy()
    env.update(TMP=str(scratch), TEMP=str(scratch))
    with tempfile.TemporaryDirectory(prefix='rx-lowcut-', dir=scratch) as tmp:
        d = Path(tmp)
        source = d / 'response.c'
        source.write_text(HARNESS, encoding='utf-8')
        exe = d / 'response.exe'
        subprocess.run(['gcc', '-std=c11', '-O2', '-ffp-contract=off', '-Wall', '-Wextra',
                        '-Werror', '-I', str(ROOT / 'firmware/main'), str(source),
                        '-lm', '-o', str(exe)], check=True, env=env)
        for hz in (0, 67, 88.5, 100, 150, 200, 250.3, 260, 270, 280, 300, 350, 400, 1000, 3000, 7000):
            data = array.array('f')
            data.frombytes(subprocess.check_output([str(exe), str(hz)]))
            assert len(data) == FS and all(math.isfinite(v) for v in data)
            rms = math.sqrt(sum(v * v for v in data) / len(data))
            if hz == 0:
                assert max(abs(v) for v in data) < 0.001
                print('PASS: DC settles below 0.001 ADC counts; reset clears all state')
                continue
            measured = 20 * math.log10(max(rms / (512 / math.sqrt(2)), 1e-30))
            if hz <= 270:
                assert measured <= -80, (hz, measured)
            elif hz >= 300:
                assert -0.12 <= measured <= 0.02, (hz, measured)
            print('PASS: float32 C, %7.1f Hz: %8.3f dB' % (hz, measured))
    print('PASS: dense response, float32 stability, ADC-bias settling and actual C tone attenuation')


if __name__ == '__main__':
    main()
