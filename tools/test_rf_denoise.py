#!/usr/bin/env python3
"""Exercise the real fixed-point preprocessor and its PCM stream adapter."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SPEEX = ROOT / 'firmware/components/speexdsp'
MAIN = ROOT / 'firmware/main'

HEAP_STUB = """
#include <stddef.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
void *heap_caps_calloc(size_t, size_t, unsigned);
void *heap_caps_aligned_alloc(size_t, size_t, unsigned);
void *heap_caps_malloc(size_t, unsigned);
void heap_caps_free(void *);
"""

HARNESS = r'''
#include "rf_denoise.h"
#include "orl_speex.h"
#include "esp_heap_caps.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned allocation, fail_at, live_allocations;
static int64_t now;
static unsigned slow_clock_call;
static uint32_t slow_us = 10099;
static void (*on_slow_frame)(void);
static int16_t input[2048], output[2048], reference[2048];
static size_t written;
extern bool rf_denoise_test_autorun;
extern bool rf_denoise_test_service(void);
static bool cancel;

void *heap_caps_malloc(size_t n, unsigned flags) {
    assert(flags == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (++allocation == fail_at) { return NULL; }
    void *p = malloc(n); assert(p); ++live_allocations; return p;
}
void *heap_caps_calloc(size_t n, size_t size, unsigned flags) {
    void *p = heap_caps_malloc(n * size, flags);
    if (p) { memset(p, 0, n * size); }
    return p;
}
void *heap_caps_aligned_alloc(size_t alignment, size_t n, unsigned flags) {
    void *p = heap_caps_malloc(n, flags);
    assert((uintptr_t)p % alignment == 0);
    return p;
}
void heap_caps_free(void *p) { if (p) { --live_allocations; free(p); } }
int64_t esp_timer_get_time(void) {
    if (slow_clock_call && --slow_clock_call == 0) {
        now += slow_us;
        if (on_slow_frame) { on_slow_frame(); }
    }
    return ++now;
}

static bool collect(const int16_t *pcm, size_t n, void *ctx) {
    (void)ctx;
    if (cancel) { return false; }
    assert(written + n <= sizeof(output) / sizeof(*output));
    memcpy(output + written, pcm, n * sizeof(*pcm));
    written += n;
    return true;
}

static void process(size_t n, size_t chunk) {
    written = 0;
    (void)rf_denoise_start(RF_DENOISE_BRIDGE);
    for (size_t i = 0; i < n; i += chunk) {
        size_t count = n - i < chunk ? n - i : chunk;
        assert(rf_denoise_push(RF_DENOISE_BRIDGE, input + i, count, collect, NULL));
    }
    assert(rf_denoise_finish(RF_DENOISE_BRIDGE, collect, NULL));
    assert(written == n);
}

static void compare_waves(const char *src, const char *dst, int frame, int db) {
    assert(frame == 160 || frame == 256 || frame == 320);
    void *arena = malloc(65536);
    size_t used;
    void *st = orl_speex_init(arena, 65536, frame, db, &used);
    assert(st && used);
    FILE *from = fopen(src, "rb"), *to = fopen(dst, "wb");
    assert(from && to);
    int16_t block[320];
    size_t preceding = 0;
    size_t n;
    while ((n = fread(block, sizeof(*block), frame, from)) != 0) {
        memset(block + n, 0, (frame - n) * sizeof(*block));
        orl_speex_run(st, block);
        assert(fwrite(block, sizeof(*block), preceding, to) == preceding);
        preceding = n;
    }
    memset(block, 0, frame * sizeof(*block));
    orl_speex_run(st, block);
    assert(fwrite(block, sizeof(*block), preceding, to) == preceding);
    assert(!ferror(from) && fclose(from) == 0 && fclose(to) == 0);
    free(arena);
}

static void push_frame(unsigned frame) {
    assert(rf_denoise_push(RF_DENOISE_BRIDGE, input + frame * 160, 160, collect, NULL));
}

static void play_while_worker_is_late(void) {
    push_frame(7);
    assert(written == 640); // The foreground emitted on this tick.
    assert(!memcmp(output + 560, input + 560, 80 * sizeof(*input)));
}

static void replace_source_while_worker_runs(void) {
    rf_denoise_stop(RF_DENOISE_BRIDGE);
    assert(rf_denoise_start(RF_DENOISE_BRIDGE));
    written = 0;
    assert(rf_denoise_push(RF_DENOISE_BRIDGE, input + 640, 47, collect, NULL));
}

static bool count_output(const int16_t *pcm, size_t n, void *context) {
    (void)pcm;
    *(size_t *)context += n;
    return true;
}

static void service_between_blocks(void) {
    while (rf_denoise_test_service()) { }
}

static void test_input_cadences(void) {
    enum { RF_FRAMES = 848, NETWORK_FRAMES = 914 };
    const rf_denoise_route_t forward[] = {RF_DENOISE_BRIDGE, RF_DENOISE_RELAY_BRIDGE};
    const rf_denoise_route_t network[] = {RF_DENOISE_P2P_SPEAKER, RF_DENOISE_P2P_RF,
                                         RF_DENOISE_RELAY_SPEAKER, RF_DENOISE_RELAY_RF};
    rf_denoise_stats_t stats;
    int16_t block[320];
    rf_denoise_test_autorun = false;
    for (unsigned mode = 0; mode < 2; ++mode) {
        rf_denoise_prepare(forward[mode]);
        assert(rf_denoise_start(forward[mode]));
        assert(rf_denoise_start(RF_DENOISE_SPEAKER));
        size_t streamed = 0, played = 0;
        uint64_t input_power = 0, speaker_power = 0;
        for (unsigned pos = 0; pos < RF_FRAMES * 160; pos += 256) {
            memcpy(block, input, 256 * sizeof(*block));
            assert(rf_denoise_push(forward[mode], block, 256, count_output, &streamed));
            assert(rf_denoise_play_block(RF_DENOISE_SPEAKER, block, 256));
            if (pos >= 16000) {
                // The periodic, seeded PCM makes each complete block's input power equal.
                for (size_t i = 0; i < 256; ++i) {
                    input_power += (int32_t)input[i] * input[i];
                    speaker_power += (int32_t)block[i] * block[i];
                }
            }
            played += 256;
            // Both real-time consumers finish this ADC batch before the worker runs.
            service_between_blocks();
        }
        assert(speaker_power * 10 < input_power); // At least 10 dB of steady noise reduction.
        rf_denoise_stats(forward[mode], &stats);
        assert(stats.wet_frames == RF_FRAMES - 4 && stats.dry_frames == 0);
        assert(rf_denoise_finish(forward[mode], count_output, &streamed));
        assert(rf_denoise_play_finish(RF_DENOISE_SPEAKER));
        size_t count;
        while ((count = rf_denoise_play_take(RF_DENOISE_SPEAKER, block, 256))) {
            played += count;
            service_between_blocks();
        }
        assert(streamed == RF_FRAMES * 160 && played == RF_FRAMES * 160 + 640);
        rf_denoise_stats(forward[mode], &stats);
        assert(stats.input_frames == RF_FRAMES && stats.wet_frames == RF_FRAMES - 1 &&
               stats.dry_frames == 1 && stats.recoveries == 0 && stats.queue_full == 0);
        rf_denoise_stats(RF_DENOISE_SPEAKER, &stats);
        assert(stats.input_frames == RF_FRAMES && stats.wet_frames == RF_FRAMES &&
               stats.dry_frames == 0 && stats.recoveries == 0 && stats.queue_full == 0);
        rf_denoise_stop(forward[mode]);
        rf_denoise_stop(RF_DENOISE_SPEAKER);
    }
    for (unsigned i = 0; i < 4; ++i) {
        const rf_denoise_route_t route = network[i];
        rf_denoise_prepare(route);
        assert(rf_denoise_start(route));
        rf_denoise_stats(route, &stats);
        const uint32_t prior_late = stats.overruns;
        for (unsigned frame = 0; frame < NETWORK_FRAMES; frame += 2) {
            memcpy(block, input, sizeof(block));
            assert(rf_denoise_play_block(route, block, 320));
            service_between_blocks();
        }
        assert(rf_denoise_play_pending(route) == 480);
        assert(rf_denoise_play_finish(route));
        service_between_blocks();
        size_t drained = 0, count;
        while ((count = rf_denoise_play_take(route, block, 320))) { drained += count; }
        assert(drained == 480);
        rf_denoise_stats(route, &stats);
        assert(stats.input_frames == NETWORK_FRAMES && stats.wet_frames == NETWORK_FRAMES &&
               stats.dry_frames == 0 && stats.overruns == prior_late);
        rf_denoise_stop(route);
    }
    rf_denoise_test_autorun = true;
    puts("PASS: 16 ms ADC batches, both modes: wet SPK 848/848 and RF-to-net 847/848; 20 ms network routes: wet 914/914");
}

static void test_mixed_deadlines(void) {
    rf_denoise_stats_t before, after;
    rf_denoise_stats(RF_DENOISE_RELAY_RF, &before);
    rf_denoise_test_autorun = false;
    assert(rf_denoise_start(RF_DENOISE_BRIDGE));
    assert(rf_denoise_start(RF_DENOISE_RELAY_RF));
    written = 0;
    push_frame(0);
    int16_t network[160] = {0};
    assert(rf_denoise_play_block(RF_DENOISE_RELAY_RF, network, 160));
    assert(rf_denoise_test_service());
    rf_denoise_stats(RF_DENOISE_RELAY_RF, &after);
    assert(after.frames == before.frames + 1);
    service_between_blocks();
    rf_denoise_stop(RF_DENOISE_BRIDGE);
    rf_denoise_stop(RF_DENOISE_RELAY_RF);
    rf_denoise_test_autorun = true;
    puts("PASS: shorter network receive lead wins dispatch over an older capture job");
}

int main(int argc, char **argv) {
    if (argc == 5) {
        compare_waves(argv[1], argv[2], atoi(argv[3]), atoi(argv[4]));
        return 0;
    }
    uint32_t random = 1;
    for (size_t i = 0; i < sizeof(input)/sizeof(*input); ++i) {
        random = random * 1664525u + 1013904223u;
        input[i] = (int16_t)((random >> 17) - 16384);
    }
    for (fail_at = 1; fail_at <= 3; ++fail_at) {
        allocation = 0;
        rf_denoise_prepare(RF_DENOISE_BRIDGE);
        assert(live_allocations == 0);
        process(1023, 257);
        assert(!memcmp(input, output, sizeof(*output) * written));
    }
    fail_at = 0;
    rf_denoise_prepare(RF_DENOISE_BRIDGE);
    rf_denoise_prepare(RF_DENOISE_SPEAKER);
    rf_denoise_prepare(RF_DENOISE_RELAY_RF);
    assert(live_allocations == 9);

    for (size_t n = 0; n <= 641; ++n) {
        process(n, 256);
        memcpy(reference, output, n * sizeof(*output));
        process(n, 71);
        assert(!memcmp(reference, output, n * sizeof(*output)));
        int16_t dac[641 + RF_DENOISE_CAPTURE_LEAD_SAMPLES];
        memcpy(dac, input, n * sizeof(*dac));
        assert(rf_denoise_start(RF_DENOISE_SPEAKER));
        for (size_t pos = 0; pos < n; ) {
            const size_t count = n - pos < 257 ? n - pos : 257;
            assert(rf_denoise_play_block(RF_DENOISE_SPEAKER, dac + pos, count));
            pos += count;
        }
        assert(rf_denoise_play_finish(RF_DENOISE_SPEAKER));
        const size_t delay = n ? RF_DENOISE_CAPTURE_LEAD_SAMPLES : 0;
        assert(rf_denoise_play_pending(RF_DENOISE_SPEAKER) == delay);
        assert(rf_denoise_play_take(RF_DENOISE_SPEAKER, dac + n, delay) == delay);
        for (size_t pos = 0; pos < delay; ++pos) { assert(!dac[pos]); }
        assert(!memcmp(dac + delay, reference, n * sizeof(*dac)));
    }

    const uint32_t completed_in_time[] = {4632, 5830};
    for (unsigned i = 0; i < 2; ++i) {
        rf_denoise_test_autorun = false;
        assert(rf_denoise_start(RF_DENOISE_BRIDGE));
        written = 0;
        push_frame(0);
        push_frame(1);
        slow_us = completed_in_time[i];
        slow_clock_call = 2;
        while (rf_denoise_test_service()) { }
        rf_denoise_test_autorun = true;
        push_frame(2);
        push_frame(3);
        assert(rf_denoise_finish(RF_DENOISE_BRIDGE, collect, NULL));
        rf_denoise_stats_t fast;
        rf_denoise_stats(RF_DENOISE_BRIDGE, &fast);
        assert(written == 640 && fast.wet_frames == 4 && fast.dry_frames == 0);
    }
    puts("PASS: 4.632 ms and 5.830 ms results completed inside the playback lead remain wet");
    slow_us = 10099;

    assert(rf_denoise_start(RF_DENOISE_BRIDGE));
    written = 0;
    for (unsigned i = 0; i <= 3; ++i) { push_frame(i); }
    rf_denoise_test_autorun = false;
    push_frame(4);
    push_frame(5);
    push_frame(6);
    slow_clock_call = 2;
    on_slow_frame = play_while_worker_is_late;
    assert(rf_denoise_test_service());
    on_slow_frame = NULL;
    while (rf_denoise_test_service()) { }
    rf_denoise_test_autorun = true;
    for (unsigned i = 8; i <= 9; ++i) { push_frame(i); }
    assert(rf_denoise_finish(RF_DENOISE_BRIDGE, collect, NULL));
    assert(written == 1600);
    rf_denoise_stats_t stats;
    rf_denoise_stats(RF_DENOISE_BRIDGE, &stats);
    assert(stats.input_frames == 10 && stats.wet_frames == 7 && stats.dry_frames == 3);
    assert(stats.overruns == 1 && stats.recoveries == 1 && stats.max_us >= 10099);
    puts("PASS: a 10.099 ms worker spike emits aligned dry PCM on time, discards late result, and recovers");

    rf_denoise_test_autorun = false;
    assert(rf_denoise_start(RF_DENOISE_BRIDGE));
    assert(rf_denoise_start(RF_DENOISE_RELAY_RF));
    written = 0;
    int16_t second[RF_DENOISE_NETWORK_LEAD_SAMPLES];
    for (unsigned i = 0; i < 12; ++i) {
        push_frame(i);
        int16_t block[160];
        memcpy(block, input + i * 160, sizeof(block));
        assert(rf_denoise_play_block(RF_DENOISE_RELAY_RF, block, 160));
        if (i >= 3) { assert(!memcmp(block, input + (i - 3) * 160, sizeof(block))); }
    }
    assert(rf_denoise_finish(RF_DENOISE_BRIDGE, collect, NULL));
    assert(written == 1920 && !memcmp(input, output, sizeof(*input) * written));
    rf_denoise_stats(RF_DENOISE_BRIDGE, &stats);
    assert(stats.input_frames == 12 && stats.dry_frames == 12 && stats.queue_full > 0);
    rf_denoise_stats(RF_DENOISE_RELAY_RF, &stats);
    assert(stats.input_frames == 12 && stats.dry_frames == 9 && stats.queue_full > 0);
    assert(rf_denoise_play_finish(RF_DENOISE_RELAY_RF));
    assert(rf_denoise_play_take(RF_DENOISE_RELAY_RF, second, 480) == 480);
    assert(!memcmp(second, input + 1440, 480 * sizeof(*second)));
    rf_denoise_stop(RF_DENOISE_RELAY_RF);
    assert(!rf_denoise_play_pending(RF_DENOISE_RELAY_RF));
    assert(rf_denoise_start(RF_DENOISE_BRIDGE));
    written = 0;
    push_frame(0);
    rf_denoise_test_autorun = true;
    while (rf_denoise_test_service()) { }
    push_frame(1);
    assert(rf_denoise_finish(RF_DENOISE_BRIDGE, collect, NULL));
    assert(written == 320);
    rf_denoise_stats(RF_DENOISE_BRIDGE, &stats);
    assert(stats.input_frames == 2 && stats.wet_frames == 0 && stats.dry_frames == 2);
    assert(!memcmp(output, input, 320 * sizeof(*input)));
    puts("PASS: stalled worker and queue saturation preserve dry PCM on two routes; old generation cannot leak");

    assert(rf_denoise_start(RF_DENOISE_BRIDGE));
    written = 0;
    push_frame(0);
    rf_denoise_test_autorun = false;
    push_frame(1);
    slow_clock_call = 2;
    on_slow_frame = replace_source_while_worker_runs;
    assert(rf_denoise_test_service());
    on_slow_frame = NULL;
    rf_denoise_test_autorun = true;
    assert(rf_denoise_finish(RF_DENOISE_BRIDGE, collect, NULL));
    assert(written == 47);
    memcpy(reference, output, 47 * sizeof(*output));
    rf_denoise_stats(RF_DENOISE_BRIDGE, &stats);
    assert(stats.input_frames == 1 && stats.wet_frames == 1 && stats.dry_frames == 0);
    assert(rf_denoise_start(RF_DENOISE_BRIDGE));
    written = 0;
    assert(rf_denoise_push(RF_DENOISE_BRIDGE, input + 640, 47, collect, NULL));
    assert(rf_denoise_finish(RF_DENOISE_BRIDGE, collect, NULL));
    assert(written == 47 && !memcmp(output, reference, 47 * sizeof(*output)));
    puts("PASS: cancellation during a worker pass cannot publish into the next source");

    assert(rf_denoise_start(RF_DENOISE_BRIDGE));
    written = 0;
    assert(rf_denoise_push(RF_DENOISE_BRIDGE, input, 47, collect, NULL));
    cancel = true;
    assert(!rf_denoise_finish(RF_DENOISE_BRIDGE, collect, NULL) && written == 0);
    cancel = false;
    rf_denoise_stop(RF_DENOISE_BRIDGE);
    puts("PASS: allocation fallback, valid partial PCM, startup lead, chunk invariance and cancelled output");
    test_input_cadences();
    test_mixed_deadlines();
    return 0;
}

'''


def compile_executable(directory, harness_source=HARNESS):
    (directory / 'esp_heap_caps.h').write_text(HEAP_STUB, encoding='utf-8')
    (directory / 'esp_timer.h').write_text('#include <stdint.h>\nint64_t esp_timer_get_time(void);\n',
                                           encoding='utf-8')
    harness = directory / 'harness.c'
    harness.write_text(harness_source, encoding='utf-8')
    worker_stub = directory / 'denoise_worker_stub.c'
    worker_stub.write_text("""#include <stdbool.h>
bool rf_denoise_test_autorun = true;
extern bool rf_denoise_test_service(void);
void rf_denoise_test_notify(void) {
    if (rf_denoise_test_autorun) { while (rf_denoise_test_service()) {} }
}
""", encoding='utf-8')
    executable = directory / 'rf_denoise_test.exe'
    upstream = SPEEX / 'upstream/libspeexdsp'
    sources = ['preprocess.c', 'fftwrap.c', 'filterbank.c', 'kiss_fft.c', 'kiss_fftr.c']
    includes = [directory, MAIN, SPEEX, SPEEX / 'upstream/include', upstream]
    command = ['gcc', '-std=gnu11', '-O2', '-DHAVE_CONFIG_H', '-DRF_DENOISE_HOST_TEST', '-Werror=implicit-function-declaration',
               *(f'-I{include}' for include in includes), str(harness), str(worker_stub), str(MAIN / 'rf_denoise.c'),
               str(SPEEX / 'orl_speex.c'), *(str(upstream / f) for f in sources), '-lm',
               '-o', str(executable)]
    env = dict(os.environ, TEMP=str(directory), TMP=str(directory), TMPDIR=str(directory))
    subprocess.run(command, check=True, env=env)
    return executable


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ab', nargs=4, metavar=('IN_PCM', 'OUT_PCM', 'FRAME', 'DB'))
    args = parser.parse_args()
    scratch = ROOT / '.tmp'
    scratch.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='denoise-', dir=scratch) as temp:
        executable = compile_executable(Path(temp))
        subprocess.run([str(executable), *(args.ab or [])], check=True)


if __name__ == '__main__':
    main()
