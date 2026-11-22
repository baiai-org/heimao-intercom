"""Check spectrum routing and the PCM cursor with the production board code."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "firmware/main"

STUBS = r"""
#include "audio_spectrum.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define AUDIO_SAMPLE_RATE_HZ 16000
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)

typedef bool StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
static uint32_t clock_ms = 1000;
static int64_t esp_timer_get_time(void) { return 1000 * clock_ms; }
static uint32_t esp_random(void) { return 100; }
static void *heap_caps_calloc(size_t n, size_t size, int caps) { (void)caps; return calloc(n, size); }
static SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *lock) { return lock; }
static int xSemaphoreTake(SemaphoreHandle_t lock, int ticks)
{
    if (*lock) { assert(ticks == 0); return 0; }
    *lock = true;
    return pdTRUE;
}
static void xSemaphoreGive(SemaphoreHandle_t lock) { assert(*lock); *lock = false; }
"""

TEST = r"""
static uint32_t read_le32(const uint8_t *data)
{
    return data[0] | (uint32_t)data[1] << 8 | (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
}

int main(void)
{
    audio_spectrum_init();
    static uint8_t reply[SPECTRUM_REPLY_BYTES];
    int16_t block[320] = {0};
    audio_spectrum_tap(SPECTRUM_RF_SPEAKER, block, 320);
    assert(!s_state);
    assert(audio_spectrum_read(0, 0, false, SPECTRUM_AUTO, reply) == SPECTRUM_HEADER_BYTES);
    assert(!memcmp(reply, "APC\x02", 4) && !read_le32(reply + 36));
    for (audio_spectrum_path_t path = 1; path < SPECTRUM_PATH_COUNT; ++path) {
        audio_spectrum_tap(path, block, 320);
    }
    audio_spectrum_read(0, 0, false, SPECTRUM_AUTO, reply);
    assert(read_le32(reply + 12) == (1u << SPECTRUM_PATH_COUNT) - 2);
    assert(read_le32(reply + 36) == SPECTRUM_MIC_RF);
    const uint32_t old_gen = read_le32(reply + 8);
    assert(audio_spectrum_read(0, 0, true, SPECTRUM_MQTT_RF, reply) == SPECTRUM_HEADER_BYTES);
    assert(read_le32(reply + 36) == SPECTRUM_MQTT_RF && read_le32(reply + 8) != old_gen);
    const uint32_t gen = read_le32(reply + 8);
    for (unsigned frame = 0; frame < 90; ++frame) {
        for (unsigned i = 0; i < 320; ++i) block[i] = (int16_t)(frame * 320 + i);
        clock_ms += 20;
        audio_spectrum_tap(SPECTRUM_MQTT_RF, block, 320);
        audio_spectrum_tap(SPECTRUM_MIC_RF, block, 320);
        if (frame % 10 == 0) { audio_spectrum_read(0, 0, false, SPECTRUM_AUTO, reply); }
    }
    assert(s_state->next == 28800 && s_state->segment_count == 1);
    /* Check a complete contiguous reply crossing physical ring wrap. */
    size_t len = audio_spectrum_read(23995, gen, false, SPECTRUM_AUTO, reply);
    assert(len == SPECTRUM_HEADER_BYTES + 2 * 4805);
    assert(read_le32(reply + 24) == 23995 && read_le32(reply + 28) == 28800);
    for (unsigned i = 0; i < 4805; ++i) {
        const unsigned j = SPECTRUM_HEADER_BYTES + 2 * i;
        assert((unsigned)(reply[j] | reply[j + 1] << 8) == 23995 + i);
    }
    audio_spectrum_read(10, gen, false, SPECTRUM_AUTO, reply);
    assert(read_le32(reply + 24) == 28800 - JOIN_SAMPLES);
    assert(read_le32(reply + 48) == REPLY_OVERRUN);
    assert(audio_spectrum_read(28800, gen, false, SPECTRUM_AUTO, reply) == SPECTRUM_HEADER_BYTES);
    /* A miss needs a new FFT segment, while unread older PCM stays available. */
    assert(xSemaphoreTake(s_mutex, 0));
    clock_ms += 20;
    audio_spectrum_tap(SPECTRUM_MQTT_RF, block, 320);
    xSemaphoreGive(s_mutex);
    clock_ms += 20;
    audio_spectrum_tap(SPECTRUM_MQTT_RF, block, 320);
    audio_spectrum_read(27840, gen, false, SPECTRUM_AUTO, reply);
    assert(read_le32(reply + 8) == gen && read_le32(reply + 32) == 960 &&
           read_le32(reply + 44) == 28800 && read_le32(reply + 48) == 0);
    audio_spectrum_read(28800, gen, false, SPECTRUM_AUTO, reply);
    assert(read_le32(reply + 24) == 29120 && read_le32(reply + 32) == 320 &&
           read_le32(reply + 40) == 1 && read_le32(reply + 48) == REPLY_GAP && read_le32(reply + 52) == 320);
    for (unsigned i = 0; i < 60; ++i) {
        clock_ms += 20;
        audio_spectrum_tap(SPECTRUM_MQTT_RF, block, 320);
    }
    audio_spectrum_read(29440, gen, false, SPECTRUM_AUTO, reply);
    assert(read_le32(reply + 8) == gen && read_le32(reply + 24) == 29440 &&
           read_le32(reply + 28) == 48640 && read_le32(reply + 32) == SPECTRUM_MAX_SAMPLES);
    clock_ms += 2500;
    audio_spectrum_tap(SPECTRUM_MQTT_RF, block, 320);
    assert(s_state->next == 48640);
    audio_spectrum_read(43520, gen, false, SPECTRUM_AUTO, reply);
    assert(read_le32(reply + 36) == SPECTRUM_MQTT_RF && read_le32(reply + 32) == 5120 &&
           read_le32(reply + 60) == 1);
    clock_ms += 20;
    audio_spectrum_tap(SPECTRUM_MQTT_RF, block, 320);
    audio_spectrum_read(48640, gen, false, SPECTRUM_AUTO, reply);
    assert(read_le32(reply + 8) == gen && read_le32(reply + 48) & REPLY_OVERRUN);
    assert(read_le32(reply + 56) == 1);
    puts("PASS: routing, ring wrap, 960 samples preserved across a 20 ms miss, 1200 ms catch-up and lease recovery");
    return 0;
}
"""


def main():
    production = (MAIN / "audio_spectrum.c").read_text(encoding="utf-8")
    source = "\n".join(line for line in production.splitlines() if not line.startswith("#include"))
    scratch = ROOT / ".tmp"
    scratch.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="spectrum-", dir=scratch) as directory:
        folder = Path(directory)
        test = folder / "test.c"
        test.write_text(STUBS + source + TEST, encoding="utf-8")
        executable = folder / "test.exe"
        env = dict(os.environ, TMP=directory, TEMP=directory, TMPDIR=directory)
        subprocess.run([
            shutil.which("gcc"), "-std=c11", "-O2", "-Wall", "-Wextra",
            "-Wno-unused-parameter", "-Werror=implicit-function-declaration",
            "-I", str(MAIN), str(test), "-o", str(executable),
        ], env=env, check=True)
        subprocess.run([str(executable)], env=env, check=True)


if __name__ == "__main__":
    main()
