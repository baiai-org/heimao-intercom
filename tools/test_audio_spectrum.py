"""Check the board's spectrum routing, binary frames and real KISS FFT."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "firmware/main"

STUBS = r"""
#include "audio_spectrum.h"
#include "spectrum_kiss.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define AUDIO_SAMPLE_RATE_HZ 16000
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY 1000
#define pdMS_TO_TICKS(ms) (ms)

typedef bool StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
static uint32_t clock_ms = 1000;
static int64_t esp_timer_get_time(void) { return 1000 * clock_ms; }
static uint32_t esp_random(void) { return 100; }
static void vTaskDelay(int ticks) { (void)ticks; }
static void *heap_caps_calloc(size_t n, size_t size, int caps) { (void)caps; return calloc(n, size); }
static void *heap_caps_malloc(size_t n, int caps) { (void)caps; return malloc(n); }
#define heap_caps_free free
static SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *lock) { return lock; }
static int xSemaphoreTake(SemaphoreHandle_t lock, int ticks)
{
    if (*lock) { assert(ticks == 0); return 0; }
    *lock = true;
    return pdTRUE;
}
static void xSemaphoreGive(SemaphoreHandle_t lock) { assert(*lock); *lock = false; }
static int xTaskCreatePinnedToCoreWithCaps(void (*fn)(void *), const char *name, unsigned stack,
                                          void *arg, int priority, void *out, int core, int caps)
{
    (void)fn; (void)name; (void)stack; (void)arg; (void)priority; (void)out; (void)core; (void)caps;
    return pdPASS;
}
"""

TEST = r"""
static uint32_t read_le32(const uint8_t *data)
{
    return data[0] | (uint32_t)data[1] << 8 | (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
}

int main(void)
{
    audio_spectrum_init();
    uint8_t reply[SPECTRUM_REPLY_BYTES];
    int16_t block[256] = {0};
    audio_spectrum_tap(SPECTRUM_RF_SPEAKER, block, 256);
    assert(!s_state);
    assert(audio_spectrum_read(0, false, SPECTRUM_AUTO, reply) == SPECTRUM_HEADER_BYTES);
    assert(!memcmp(reply, "ASP\x01", 4) && reply[16] == 0);
    for (audio_spectrum_path_t path = 1; path < SPECTRUM_PATH_COUNT; ++path) {
        audio_spectrum_tap(path, block, 256);
    }
    assert(audio_spectrum_read(0, false, SPECTRUM_AUTO, reply) == SPECTRUM_HEADER_BYTES);
    assert(read_le32(reply + 8) == (1u << SPECTRUM_PATH_COUNT) - 2);
    assert(reply[16] == SPECTRUM_MIC_RF);
    const uint32_t old_generation = read_le32(reply + 4);
    assert(audio_spectrum_read(0, true, SPECTRUM_MQTT_RF, reply) == SPECTRUM_HEADER_BYTES);
    assert(reply[16] == SPECTRUM_MQTT_RF && read_le32(reply + 4) != old_generation);
    spectrum_state_t *s = s_state;
    for (unsigned frame = 0; frame < 8; ++frame) {
        for (unsigned i = 0; i < 256; ++i) {
            const float t = (frame * 256 + i) / 16000.0f;
            block[i] = (int16_t)(3200 * sinf(6.2831853f * 1000 * t) +
                                 2200 * sinf(6.2831853f * 1046.875f * t));
        }
        clock_ms += 16;
        audio_spectrum_tap(SPECTRUM_MQTT_RF, block, 256);
        audio_spectrum_tap(SPECTRUM_REMOTE_RF, block, 256);
    }
    assert(s->path == SPECTRUM_MQTT_RF && s->input_count == FFT_SIZE);
    s->fft = kiss_fftr_alloc(FFT_SIZE, 0, s->fft_memory, &s->fft_bytes);
    assert(s->fft);
    for (unsigned i = 0; i < FFT_SIZE; ++i) {
        s->hann[i] = 0.5f - 0.5f * cosf(6.28318530718f * i / (FFT_SIZE - 1));
        s->snapshot[i] = s->input[i];
    }
    spectrum_row_t *row = &s->rows[0];
    analyse(s, row->bins);
    const uint8_t *bins = row->bins + (128 - SPECTRUM_FIRST_BIN);
    assert(bins[0] > 225 && bins[6] > 212);
    assert(bins[0] > bins[-1] && bins[0] > bins[1]);
    assert(bins[6] > bins[5] && bins[6] > bins[7] && bins[3] < 12);

    row->sequence = 16;
    row->stamp = clock_ms;
    s->row_count = s->row_head = 1;
    assert(audio_spectrum_read(15, false, SPECTRUM_AUTO, reply) == SPECTRUM_HEADER_BYTES + SPECTRUM_ROW_BYTES);
    assert(reply[17] == 1 && read_le32(reply + 20) == 16);
    assert(!memcmp(reply + 28, row->bins, SPECTRUM_BINS));
    assert(audio_spectrum_read(16, false, SPECTRUM_AUTO, reply) == SPECTRUM_HEADER_BYTES);
    assert(!reply[17]);
    clock_ms += 170;
    audio_spectrum_tap(SPECTRUM_MQTT_RF, block, 256);
    assert(s->input_count == 256);
    clock_ms += 3000;
    audio_spectrum_tap(SPECTRUM_MQTT_RF, block, 256);
    assert(s->input_count == 256);
    puts("PASS: every route, shared route switch, expiry and overlap gap, binary cursor, 1009 bins and two independent 1000/1046.875 Hz peaks");
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
        libc = ROOT / "firmware/components/speexdsp/upstream/include"
        env = dict(os.environ, TMP=directory, TEMP=directory, TMPDIR=directory)
        subprocess.run([
            shutil.which("gcc"), "-std=c11", "-O2", "-Wall", "-Wextra",
            "-Wno-unused-parameter", "-Werror=implicit-function-declaration",
            "-I", str(MAIN), "-I", str(libc), str(test),
            str(MAIN / "spectrum_kiss_fft.c"), str(MAIN / "spectrum_kiss_fftr.c"),
            "-lm", "-o", str(executable),
        ], env=env, check=True)
        subprocess.run([str(executable)], env=env, check=True)


if __name__ == "__main__":
    main()
