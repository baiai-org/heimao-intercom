#!/usr/bin/env python3
"""Run production radio_led.c against a deterministic clock, without any board."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
STUBS = r'''
#include <stdint.h>
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))
int64_t esp_timer_get_time(void);
'''
HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "radio_led.c"
static int64_t clock_ms;
int64_t esp_timer_get_time(void) { return clock_ms * 1000; }
static radio_led_state_t sample(bool tx, bool valid, bool cos, bool ready, uint8_t mode) {
    radio_led_state_t state;
    radio_led_sample(tx, valid, cos, ready, mode, &state);
    return state;
}
static void reset(void) {
    memset(&s_state, 0, sizeof(s_state));
    memset(&s_rx_event, 0, sizeof(s_rx_event));
    memset(&s_tx_event, 0, sizeof(s_tx_event));
    s_cos_candidate = false;
    s_cos_since = s_rx_blink_since = s_tx_blink_since = 0;
    clock_ms = 0;
}
int main(void) {
    radio_led_state_t s;
    /* Complete truth table, including simultaneous RF/network directions.
     * A half-duplex RF TX suppresses only RF RX; network RX remains visible. */
    for (int bits = 0; bits < 16; ++bits) {
        reset();
        bool rf_tx = bits & 1, cos = bits & 2;
        bool net_tx = bits & 4, net_rx = bits & 8;
        sample(rf_tx, true, cos, true, 1);
        clock_ms = 40;
        if (net_tx) radio_led_note_net_tx(1);
        if (net_rx) radio_led_note_net_rx(1);
        s = sample(rf_tx, true, cos, true, 1);
        const bool rf_rx = cos && !rf_tx;
        assert(s.rf_tx == rf_tx && s.rf_rx == rf_rx);
        assert(s.net_tx == net_tx && s.net_rx == net_rx);
        assert(s.tx == (rf_tx || net_tx) && s.rx == (rf_rx || net_rx));
        assert(s.tx_mode == (rf_tx ? RADIO_LED_RF : net_tx ? RADIO_LED_NET : RADIO_LED_OFF));
        assert(s.rx_mode == (rf_rx ? RADIO_LED_RF : net_rx ? RADIO_LED_NET : RADIO_LED_OFF));
        clock_ms = 140; /* 100 ms later: network off, RF remains steady. */
        if (net_tx) radio_led_note_net_tx(1);
        if (net_rx) radio_led_note_net_rx(1);
        s = sample(rf_tx, true, cos, true, 1);
        assert(s.tx == rf_tx && s.rx == rf_rx);
        assert(s.net_tx == net_tx && s.net_rx == net_rx);
    }
    reset();
    /* 5 Hz stays anchored to the stream, not restarted by every 20 ms frame.
     * Zero-valued/silent voice frames count too; there is no amplitude gate. */
    for (clock_ms = 0; clock_ms <= 1000; clock_ms += 20) {
        radio_led_note_net_tx(2);
        radio_led_note_net_rx(2);
        s = sample(false, true, false, true, 2);
        assert(s.tx == ((clock_ms / 100) % 2 == 0) && s.rx == s.tx);
    }
    clock_ms = 1199; s = sample(false, true, false, true, 2);
    assert(s.net_tx && s.net_rx);
    clock_ms = 1200; s = sample(false, true, false, true, 2);
    assert(!s.tx && !s.rx && !s.net_tx && !s.net_rx);
    clock_ms = 1234; radio_led_note_net_tx(2);
    s = sample(false, true, false, true, 2);
    assert(s.tx); /* New burst starts lit regardless of absolute clock phase. */

    reset();
    /* COS deglitching, short noise pulse, close delay, and TX fresh re-arm. */
    sample(false, true, true, false, 0);
    clock_ms = 20; s = sample(false, true, true, false, 0); assert(!s.rx);
    clock_ms = 30; sample(false, true, false, false, 0);
    clock_ms = 40; sample(false, true, true, false, 0);
    clock_ms = 79; s = sample(false, true, true, false, 0); assert(!s.rx);
    clock_ms = 80; s = sample(false, true, true, false, 0); assert(s.rx);
    clock_ms = 100; sample(false, true, false, false, 0);
    clock_ms = 159; s = sample(false, true, false, false, 0); assert(s.rx);
    clock_ms = 160; s = sample(false, true, false, false, 0); assert(!s.rx);
    clock_ms = 200; sample(false, true, true, false, 0);
    clock_ms = 240; s = sample(false, true, true, false, 0); assert(s.rx);
    clock_ms = 241; s = sample(true, true, true, false, 0); assert(s.tx && !s.rx);
    clock_ms = 260; s = sample(false, true, true, false, 0); assert(!s.rx);
    clock_ms = 299; s = sample(false, true, true, false, 0); assert(!s.rx);
    clock_ms = 300; s = sample(false, true, true, false, 0); assert(s.rx);
    clock_ms = 301; s = sample(false, false, true, false, 0); assert(!s.rx);
    clock_ms = 5000; s = sample(false, false, true, false, 0); assert(!s.rx);

    reset();
    /* Mode changes, stop/reset, disconnect/reconnect, and controls-only idle
     * cannot reuse a previous stream. Late events from another engine vanish. */
    radio_led_note_net_rx(1); radio_led_note_net_tx(1);
    s = sample(false, false, false, true, 1); assert(s.rx && s.tx);
    s = sample(false, false, false, true, 2); assert(!s.rx && !s.tx);
    radio_led_reset_net();
    s = sample(false, false, false, true, 1); assert(!s.rx && !s.tx);
    radio_led_note_net_rx(1); radio_led_note_net_tx(1);
    s = sample(false, false, false, false, 1); assert(!s.rx && !s.tx);
    s = sample(false, false, false, true, 1); assert(!s.rx && !s.tx);
    radio_led_note_net_rx(1); radio_led_note_net_tx(1);
    s = sample(false, false, false, true, 0); assert(!s.rx && !s.tx);
    s = sample(false, false, false, true, 1); assert(!s.rx && !s.tx);
    for (clock_ms = 0; clock_ms < 10000; clock_ms += 20) {
        s = sample(false, true, false, true, 1); assert(!s.rx && !s.tx);
    }
    /* Uptime beyond 32-bit milliseconds must not stick the LEDs on. */
    clock_ms = (int64_t)UINT32_MAX + 10000;
    radio_led_note_net_tx(1);
    s = sample(false, false, false, true, 1); assert(s.tx);
    clock_ms += 200; s = sample(false, false, false, true, 1); assert(!s.tx);
    radio_led_get_state(&s); assert(!s.tx && !s.rx);
    assert(strcmp(radio_led_mode_name(RADIO_LED_RF), "rf") == 0);
    puts("PASS: 16-source truth table; RF priority; 5 Hz waveform; burst/timeout; "
         "COS debounce; TX re-arm; invalid COS; modes; disconnect/reset; long uptime");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='gcc')
    args = parser.parse_args()
    compiler = shutil.which(args.cc)
    if not compiler:
        parser.error('host compiler not found: ' + args.cc)
    scratch = ROOT / '.tmp'
    scratch.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='orl-led-', dir=scratch) as temp:
        directory = Path(temp)
        environment = dict(os.environ, TEMP=temp, TMP=temp, TMPDIR=temp)
        (directory / 'freertos').mkdir()
        for name in ('radio_led.c', 'radio_led.h'):
            shutil.copy2(ROOT / 'firmware/main' / name, directory / name)
        for name in ('esp_timer.h', 'freertos/FreeRTOS.h'):
            (directory / name).write_text(STUBS, encoding='utf-8')
        (directory / 'test.c').write_text(HARNESS, encoding='utf-8')
        executable = directory / 'test.exe'
        subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-I', str(directory), str(directory / 'test.c'),
                        '-o', str(executable)], check=True, env=environment)
        subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    main()
