"""Exercise the production tail settings and file switch in a temporary filesystem."""
import os
from pathlib import Path
import subprocess
import tempfile

from test_np_peer_runtime import function


ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "firmware/main"
PREFIX = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include "g711.h"

#define RATE 16000
#define CUSTOM_MAX_STORED (RATE * 5)
#define CUSTOM_PATH "legacy-tail.bin"
#define NORM_PEAK 12000
#define TAIL_OFF 0
#define TAIL_CUSTOM 8
#define TAIL_COUNT 9
#define TAIL_MODE_ALL 7
#define ESP_OK 0
#define ESP_FAIL 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_INVALID_ARG 3
#define ESP_ERR_INVALID_SIZE 4
#define ESP_ERR_NOT_FOUND 5
#define ESP_ERR_NO_MEM 6
#define ESP_ERR_NVS_NOT_FOUND 7
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define MALLOC_CAP_SPIRAM 1
#define portMAX_DELAY 0
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))
typedef int nvs_handle_t;
typedef int esp_err_t;
typedef int StaticSemaphore_t;
typedef int *SemaphoreHandle_t;
typedef int portMUX_TYPE;
typedef struct { bool open; } bridge_rx_stats_t;
typedef struct {
    int id;
    uint8_t modes;
    size_t custom_samples;
    size_t custom_play_samples;
    bool enabled;
} tail_status_t;
static int16_t pool[1], *s_pool = pool;
static size_t s_len[TAIL_COUNT] = {0, 3200};
"""

STUBS = r"""
static tail_config_t saved, staged;
static bool has_saved, fail_commit, tx_active, rf_active, preview, reserved;
static const char *fail_open;

static esp_err_t nvs_open(const char *ns, int access, nvs_handle_t *h) {
    assert(strcmp(ns, "tail") == 0);
    *h = access;
    return ESP_OK;
}
static void nvs_close(nvs_handle_t h) { (void)h; }
static esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *n) {
    assert(h == NVS_READONLY && strcmp(key, "state") == 0 && *n == sizeof(saved));
    if (!has_saved) return ESP_ERR_NVS_NOT_FOUND;
    memcpy(out, &saved, *n);
    return ESP_OK;
}
static esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *value, size_t n) {
    assert(h == NVS_READWRITE && strcmp(key, "state") == 0 && n == sizeof(staged));
    memcpy(&staged, value, n);
    return ESP_OK;
}
static esp_err_t nvs_commit(nvs_handle_t h) {
    assert(h == NVS_READWRITE);
    if (fail_commit) return ESP_FAIL;
    saved = staged;
    has_saved = true;
    return ESP_OK;
}
static esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *out) {
    assert(h == NVS_READONLY);
    *out = strcmp(key, "sel") == 0 ? TAIL_CUSTOM : 2;
    return ESP_OK;
}
static esp_err_t nvs_get_u32(nvs_handle_t h, const char *key, uint32_t *out) {
    assert(h == NVS_READONLY && strcmp(key, "rate") == 0);
    *out = 8000;
    return ESP_OK;
}
static void *heap_caps_malloc(size_t n, unsigned caps) { assert(caps == MALLOC_CAP_SPIRAM); return malloc(n); }
static void xSemaphoreTake(SemaphoreHandle_t lock, int wait) { assert(lock && !*lock && !wait); ++*lock; }
static void xSemaphoreGive(SemaphoreHandle_t lock) { assert(*lock == 1); --*lock; }
static bool ptt_local_audio_begin(void) {
    assert(!reserved);
    if (tx_active) return false;
    reserved = true;
    return true;
}
static void ptt_local_audio_end(void) { assert(reserved); reserved = false; }
static bool tail_preview_active(void) { return preview; }
static void bridge_rx_stats(bridge_rx_stats_t *stats) { stats->open = rf_active; }
static FILE *open_simulated(const char *path, const char *mode) {
    if (fail_open && strcmp(fail_open, path) == 0) return NULL;
    return fopen(path, mode);
}
#define fopen open_simulated
"""

TEST = r"""
static int16_t *clip(size_t samples, int amplitude)
{
    int16_t *pcm = malloc(samples * sizeof(*pcm));
    assert(pcm);
    for (size_t i = 0; i < samples; ++i) pcm[i] = (i & 1) ? amplitude : -amplitude;
    return pcm;
}

static void reboot(tail_config_t defaults)
{
    free(s_clip.pcm);
    s_clip = (tail_clip_t){0};
    s_config = defaults;
    load_config();
    load_custom();
}

int main(void)
{
    s_write_lock = &s_write_storage;
    const tail_config_t defaults = s_config;
    FILE *file = fopen(CUSTOM_PATH, "wb");
    assert(file);
    for (int i = 0; i < 3200; ++i) assert(fputc(0x91, file) != EOF);
    assert(fclose(file) == 0);
    reboot(defaults);
    assert(tail_get() == TAIL_CUSTOM && tail_samples(TAIL_CUSTOM) == 6400);
    assert(tail_id_for_mode(0) == TAIL_OFF && tail_id_for_mode(1) == TAIL_CUSTOM);
    assert(tail_set(1) == ESP_OK && tail_id_for_mode(1) == 1);
    assert(tail_modes_set(5) == ESP_OK && tail_id_for_mode(1) == TAIL_OFF);
    assert(tail_should_play(0) && tail_should_play(2));
    assert(tail_modes_set(0) == ESP_ERR_INVALID_ARG);

    assert(custom_store_finish(clip(CUSTOM_MAX_STORED, 6000), CUSTOM_MAX_STORED, true) == ESP_OK);
    assert(s_config.file == 1 && tail_get() == TAIL_CUSTOM && s_config.modes == 5);
    assert(tail_samples(TAIL_CUSTOM) == CUSTOM_MAX_STORED);
    int16_t last[16];
    assert(tail_fetch(TAIL_CUSTOM, CUSTOM_MAX_STORED - 3, last, 16) == 3);
    assert(last[0] != 0 && last[1] == -last[0]);
    reboot(defaults);
    assert(tail_samples(TAIL_CUSTOM) == CUSTOM_MAX_STORED && tail_id_for_mode(2) == TAIL_CUSTOM);

    /* A partial inactive file, failed file write or failed NVS commit leaves
     * the prior selection and clip available to the next boot. */
    file = fopen(s_tail_files[2], "wb");
    assert(file && fputc(0, file) != EOF && fclose(file) == 0);
    reboot(defaults);
    assert(s_config.file == 1 && tail_samples(TAIL_CUSTOM) == CUSTOM_MAX_STORED);
    fail_open = s_tail_files[2];
    assert(custom_store_finish(clip(3200, 5000), 3200, true) == ESP_FAIL);
    fail_open = NULL;
    fail_commit = true;
    assert(tail_set(1) == ESP_FAIL && tail_get() == TAIL_CUSTOM);
    assert(custom_store_finish(clip(3200, 5000), 3200, true) == ESP_FAIL);
    reboot(defaults);
    assert(s_config.file == 1 && tail_samples(TAIL_CUSTOM) == CUSTOM_MAX_STORED && tail_get() == TAIL_CUSTOM);
    fail_commit = false;
    tx_active = true;
    assert(custom_store_finish(clip(3200, 5000), 3200, true) == ESP_ERR_INVALID_STATE);
    tx_active = false;
    rf_active = true;
    assert(tail_custom_delete() == ESP_ERR_INVALID_STATE);
    rf_active = false;
    assert(s_config.file == 1 && !reserved);

    assert(tail_set(1) == ESP_OK);
    assert(custom_store_finish(clip(3200, 5000), 3200, false) == ESP_OK);
    assert(s_config.file == 2 && tail_samples(TAIL_CUSTOM) == 3200 && tail_get() == 1);
    assert(tail_set(TAIL_CUSTOM) == ESP_OK && tail_id_for_mode(0) == TAIL_CUSTOM);
    assert(tail_custom_delete() == ESP_OK);
    reboot(defaults);
    assert(!s_clip.samples && tail_get() == TAIL_OFF && s_config.modes == 5);
    assert(tail_set(TAIL_CUSTOM) == ESP_ERR_NOT_FOUND && !reserved && !atomic_load(&s_storing));
    puts("PASS: legacy clip, mode mask, file/selection commit, recovery, busy replacement, and delete");
    return 0;
}
"""


def main():
    source = (MAIN / "voice_tail.c").read_text("utf-8")
    first = source.index("/* A complete settings record")
    end = source.index("/* ------------------------------------------------------- built-in synth */")
    state = source[first:end].replace('"/storage/', '"')
    names = ("load_config", "save_config", "audio_window", "load_custom", "tail_samples_locked",
             "tail_samples", "tail_status", "tail_id_for_mode", "tail_get", "tail_should_play",
             "save_selection", "tail_modes_set", "tail_set", "reserve_custom_edit",
             "install_custom", "tail_custom_delete", "write_custom_file", "custom_store_finish")
    fetching = function(source, "tail_fetch")
    # The built-in pool is immutable. The test only fetches the custom slot.
    code = (PREFIX + state + STUBS + "\n".join(function(source, name) for name in names)
            + "\nstatic size_t s_off[TAIL_COUNT];\n" + fetching + TEST)
    scratch = ROOT / ".tmp"
    scratch.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="tail-files-", dir=scratch) as temp:
        work = Path(temp)
        program = work / "regression.c"
        exe = work / "regression.exe"
        program.write_text(code, encoding="utf-8")
        env = dict(os.environ, TEMP=temp, TMP=temp, TMPDIR=temp)
        subprocess.run(
            ["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(MAIN), str(program),
             str(MAIN / "g711.c"), "-o", str(exe)], check=True, env=env)
        subprocess.run([str(exe)], cwd=work, check=True, env=env)


if __name__ == "__main__":
    main()
