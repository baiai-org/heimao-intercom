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
static unsigned timer_calls, slow_at;
static int64_t now;
static int16_t input[1024], output[1024], reference[1024];
static size_t written;
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
int64_t esp_timer_get_time(void) { now += ++timer_calls == slow_at ? 3000 : 1; return now; }

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

int main(int argc, char **argv) {
    if (argc == 5) {
        compare_waves(argv[1], argv[2], atoi(argv[3]), atoi(argv[4]));
        return 0;
    }
    uint32_t rand_state = 1;
    for (size_t i = 0; i < sizeof(input)/sizeof(*input); ++i) {
        rand_state = rand_state * 1664525u + 1013904223u;
        input[i] = (int16_t)((rand_state >> 17) - 16384);
    }
    void *arena = malloc(65536);
    assert(arena);
    for (int frame = 160; frame <= 320; frame += 160) {
        size_t used;
        assert(orl_speex_init(arena, 65536, frame, -6, &used));
        const size_t required = used;
        /* Walk every capacity, so each internal allocation is made to fail. */
        for (size_t capacity = 0; capacity < required; ++capacity) {
            assert(!orl_speex_init(arena, capacity, frame, -6, &used) && !used);
        }
        assert(orl_speex_init(arena, required, frame, -6, &used));
        printf("speex_frame=%d arena_used=%zu\n", frame, used);
    }
    free(arena);
    for (fail_at = 1; fail_at <= 3; ++fail_at) {
        allocation = 0;
        rf_denoise_prepare(RF_DENOISE_BRIDGE);
        assert(live_allocations == 0);
        process(1023, 257);
        assert(!memcmp(input, output, sizeof(*output) * written));
    }
    fail_at = 0;
    rf_denoise_prepare(RF_DENOISE_BRIDGE);
    assert(live_allocations == 3);
    for (size_t n = 0; n <= 641; ++n) {
        process(n, 256);
        memcpy(reference, output, n * sizeof(*output));
        process(n, 71);
        assert(!memcmp(reference, output, n * sizeof(*output)));
    }

    /* Bad deadline on either a complete frame or the partial-frame drain.
       The pending raw interval and every following sample come out once. */
    timer_calls = 0; slow_at = 4;
    process(640, 257);
    assert(!memcmp(input, output, 640 * sizeof(*output)));
    timer_calls = 0; slow_at = 4;
    process(47, 47);
    assert(!memcmp(input, output, 47 * sizeof(*output)));
    timer_calls = 0; slow_at = 2;
    process(47, 47);
    assert(!memcmp(input, output, 47 * sizeof(*output)));
    slow_at = 0;

    written = 0;
    assert(rf_denoise_start(RF_DENOISE_BRIDGE));
    assert(rf_denoise_push(RF_DENOISE_BRIDGE, input, 47, collect, NULL));
    cancel = true;
    assert(!rf_denoise_finish(RF_DENOISE_BRIDGE, collect, NULL) && written == 0);
    cancel = false;
    process(1, 1);
    rf_denoise_stats_t stats;
    rf_denoise_stats(RF_DENOISE_BRIDGE, &stats);
    assert(stats.overruns == 3 && stats.max_us == 3000 && stats.frames > 1000);
    printf("PASS: bounded OOM cleanup, chunk-invariant PCM, short drain, deadline bypass, canceled output, reset (%lu bytes)\n",
           (unsigned long)stats.workspace_bytes);
    rf_denoise_prepare(RF_DENOISE_SPEAKER);
    rf_denoise_prepare(RF_DENOISE_HANDSET_TX);
    assert(live_allocations == 9);
    for (size_t count = 1; count <= 640; ++count) {
        process(count, 72);
        memcpy(reference, output, count * sizeof(*output));
        int16_t speaker[1024], mic[1024], reverse_reference[640];
        for (size_t i = 0; i < count; ++i) {
            speaker[i] = input[i];
            mic[i] = input[count - i - 1];
        }
        assert(rf_denoise_start(RF_DENOISE_SPEAKER));
        assert(rf_denoise_start(RF_DENOISE_HANDSET_TX));
        assert(rf_denoise_start(RF_DENOISE_BRIDGE)); written = 0;
        for (size_t i = 0; i < count; ) {
            size_t n = count - i < 257 ? count - i : 257;
            assert(rf_denoise_push(RF_DENOISE_BRIDGE, mic + i, n, collect, NULL));
            assert(rf_denoise_play_block(RF_DENOISE_SPEAKER, speaker + i, n));
            assert(rf_denoise_play_block(RF_DENOISE_HANDSET_TX, mic + i, n));
            i += n;
        }
        assert(rf_denoise_finish(RF_DENOISE_BRIDGE, collect, NULL) && written == count);
        memcpy(reverse_reference, output, count * sizeof(*output));
        assert(rf_denoise_play_finish(RF_DENOISE_SPEAKER));
        assert(rf_denoise_play_finish(RF_DENOISE_HANDSET_TX));
        assert(rf_denoise_play_pending(RF_DENOISE_SPEAKER) == 320);
        assert(rf_denoise_play_take(RF_DENOISE_SPEAKER, speaker + count, 320) == 320);
        assert(rf_denoise_play_take(RF_DENOISE_HANDSET_TX, mic + count, 320) == 320);
        assert(!memcmp(speaker, mic, 320 * sizeof(*mic)));
        for (size_t i = 0; i < 320; ++i) { assert(!speaker[i]); }
        assert(!memcmp(speaker + 320, reference, count * sizeof(*speaker)));
        assert(!memcmp(mic + 320, reverse_reference, count * sizeof(*mic)));
        rf_denoise_stop(RF_DENOISE_SPEAKER);
        assert(!rf_denoise_play_pending(RF_DENOISE_SPEAKER));
        assert(!rf_denoise_play_take(RF_DENOISE_SPEAKER, speaker, 320));
    }
    puts("PASS: three independent live streams, fixed 20ms DAC lead, exact short/long drains and cancel");
    return 0;
}
'''


def compile_executable(directory, harness_source=HARNESS):
    (directory / 'esp_heap_caps.h').write_text(HEAP_STUB, encoding='utf-8')
    (directory / 'esp_timer.h').write_text('#include <stdint.h>\nint64_t esp_timer_get_time(void);\n',
                                           encoding='utf-8')
    harness = directory / 'harness.c'
    harness.write_text(harness_source, encoding='utf-8')
    executable = directory / 'rf_denoise_test.exe'
    upstream = SPEEX / 'upstream/libspeexdsp'
    sources = ['preprocess.c', 'fftwrap.c', 'filterbank.c', 'kiss_fft.c', 'kiss_fftr.c']
    includes = [directory, MAIN, SPEEX, SPEEX / 'upstream/include', upstream]
    command = ['gcc', '-std=gnu11', '-O2', '-DHAVE_CONFIG_H', '-Werror=implicit-function-declaration',
               *(f'-I{include}' for include in includes), str(harness), str(MAIN / 'rf_denoise.c'),
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
