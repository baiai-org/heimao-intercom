"""Verify the persisted TX limit and clock windows using the production C code."""

import os
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "firmware/main"

NVS = r"""
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "esp_err.h"
typedef int nvs_handle_t;
#define NVS_READONLY 0
#define NVS_READWRITE 1
static uint16_t stored, pending;
static int write_error, commit_error;
static int nvs_open(const char *ns, int mode, nvs_handle_t *out) {
    assert(!strcmp(ns, "orl")); *out = mode; pending = stored; return ESP_OK;
}
static int nvs_get_u16(nvs_handle_t h, const char *key, uint16_t *out) {
    assert(h == NVS_READONLY && !strcmp(key, "tx_timeout_s"));
    if (!stored) { return 1; }
    *out = stored;
    return ESP_OK;
}
static int nvs_set_u16(nvs_handle_t h, const char *key, uint16_t value) {
    assert(h == NVS_READWRITE && !strcmp(key, "tx_timeout_s"));
    pending = value;
    return write_error;
}
static int nvs_commit(nvs_handle_t h) {
    assert(h == NVS_READWRITE);
    if (!commit_error) { stored = pending; }
    return commit_error;
}
static void nvs_close(nvs_handle_t h) { (void)h; }
"""

TEST = r"""
#include "tx_timeout.c"
#include <stdio.h>
int main(void) {
    tx_timeout_load();
    assert(tx_timeout_seconds() == 180);
    assert(tx_timeout_save(4, 180) == ESP_ERR_INVALID_ARG);
    assert(tx_timeout_save(601, 180) == ESP_ERR_INVALID_ARG);
    assert(tx_timeout_save(5, 180) == ESP_OK);
    assert(tx_timeout_save(600, 180) == ESP_ERR_INVALID_STATE && stored == 5);
    write_error = 9;
    assert(tx_timeout_save(600, 5) == 9 && tx_timeout_seconds() == 5 && stored == 5);
    write_error = 0; commit_error = 9;
    assert(tx_timeout_save(600, 5) == 9 && tx_timeout_seconds() == 5 && stored == 5);
    commit_error = 0;
    assert(tx_timeout_save(600, 5) == ESP_OK);
    tx_timeout_load();
    assert(tx_timeout_seconds() == 600 && stored == 600);
    stored = 601;
    tx_timeout_load();
    assert(tx_timeout_seconds() == 180);

    const unsigned values[] = {5, 180, 600};
    const int64_t started = INT64_C(9000000000);
    for (unsigned i = 0; i < sizeof(values) / sizeof(*values); ++i) {
        const tx_timeout_window_t window = tx_timeout_window(started, values[i]);
        assert(window.speech_us - started == (int64_t)values[i] * 1000000);
        assert(window.release_us - window.speech_us == 6000000);
        assert(!tx_timeout_due(window, window.speech_us - 1));
        assert(tx_timeout_due(window, window.speech_us));
    }
    puts("PASS: TX timeout NVS bounds/defaults/conflicts/failures/reload and 64-bit 5/180/600 s windows");
}
"""


def main():
    scratch = ROOT / ".tmp"
    scratch.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="tx-timeout-", dir=scratch) as temp:
        work = Path(temp)
        (work / "esp_err.h").write_text(
            "#pragma once\ntypedef int esp_err_t;\n"
            "#define ESP_OK 0\n#define ESP_ERR_INVALID_ARG 2\n#define ESP_ERR_INVALID_STATE 3\n",
            encoding="utf-8",
        )
        (work / "nvs.h").write_text(NVS, encoding="utf-8")
        harness = work / "test.c"
        harness.write_text(TEST, encoding="utf-8")
        exe = work / "test.exe"
        env = dict(os.environ, TEMP=temp, TMP=temp, TMPDIR=temp)
        subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", temp,
                        "-I", str(MAIN), str(harness), "-o", str(exe)], check=True, env=env)
        subprocess.run([str(exe)], check=True, env=env)


if __name__ == "__main__":
    main()
