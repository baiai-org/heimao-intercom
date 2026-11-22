"""Exercise the production whole-device denoise transaction and legacy reads."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

from test_tx_ownership import function

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "firmware/main"

STUBS = r"""
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "rf_denoise.h"

typedef int esp_err_t;
typedef int nvs_handle_t;
#define ESP_OK 0
#define ESP_ERR_NVS_NOT_FOUND 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_INVALID_ARG 3
#define ESP_ERR_NO_MEM 4
#define NVS_READWRITE 1
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
#define NETCOM_MODE_LOCAL 0
#define NETCOM_MODE_NETP2P 1
#define NETCOM_MODE_RELAY 2
#define portENTER_CRITICAL(lock) ((void)0)
#define portEXIT_CRITICAL(lock) ((void)0)

static int s_state_mux=1;
static atomic_uint s_denoise_config;
static unsigned prepared;
static bool fail_commit, modern_present, legacy_present;
static uint32_t saved[5], pending[5], legacy;
static int commits;
static int xSemaphoreTake(int lock, int ticks) { (void)ticks; return lock; }
static void xSemaphoreGive(int lock) { assert(lock); }
static esp_err_t nvs_open(const char *ns, int mode, int *handle)
{ (void)ns; (void)mode; *handle=1; return ESP_OK; }
static esp_err_t nvs_get_u32(int handle, const char *key, uint32_t *value)
{
    assert(handle);
    if (!strcmp(key,"auddn_lvl1") && legacy_present) { *value=legacy; return ESP_OK; }
    return ESP_ERR_NVS_NOT_FOUND;
}
static esp_err_t nvs_get_u8(int handle, const char *key, uint8_t *value)
{ (void)handle; (void)key; (void)value; return ESP_ERR_NVS_NOT_FOUND; }
static esp_err_t nvs_get_blob(int handle, const char *key, void *value, size_t *size)
{
    assert(handle && !strcmp(key,"auddn_sel2"));
    if (!modern_present) { return ESP_ERR_NVS_NOT_FOUND; }
    assert(*size==sizeof(saved));
    memcpy(value,saved,*size);
    return ESP_OK;
}
static esp_err_t nvs_set_blob(int handle, const char *key, const void *value, size_t size)
{
    assert(handle && !strcmp(key,"auddn_sel2") && size==sizeof(pending));
    memcpy(pending,value,size);
    return ESP_OK;
}
static esp_err_t nvs_commit(int handle)
{
    assert(handle);
    if (fail_commit) { return ESP_ERR_NO_MEM; }
    modern_present=true; memcpy(saved,pending,sizeof(saved)); ++commits; return ESP_OK;
}
static void nvs_close(int handle) { assert(handle); }
void rf_denoise_prepare(rf_denoise_route_t route) { prepared |= 1u << route; }
"""

TEST = r"""
int main(void)
{
    publish_denoise(load_denoise_config(1));
    bridge_denoise_config_t defaults=bridge_denoise_config();
    assert(defaults.selection_level==RF_DENOISE_LEVEL_OFF && !defaults.levels && !defaults.revision);
    assert(defaults.paths==DENOISE_CONFIG_MASK && !prepared);
    const bridge_denoise_config_t legacy_off={.selection_level=RF_DENOISE_LEVEL_STANDARD};
    assert(bridge_denoise_save_selection(RF_DENOISE_LEVEL_STANDARD,0,legacy_off)==ESP_OK);
    assert(bridge_denoise_config().paths==DENOISE_CONFIG_MASK && !modern_present);

    legacy_present=true;
    publish_denoise(load_denoise_config(1));
    assert(bridge_denoise_config().levels==0);
    assert(bridge_denoise_config().selection_level==RF_DENOISE_LEVEL_STANDARD);
    legacy=RF_DENOISE_LEVEL_LIGHT | (RF_DENOISE_LEVEL_EXTREME << 27);
    publish_denoise(load_denoise_config(1));
    bridge_denoise_config_t mixed=bridge_denoise_config();
    const uint32_t paths=(1u<<1) | (1u<<4) | (1u<<10);
    assert(mixed.levels==legacy && mixed.selection_level==RF_DENOISE_LEVEL_STANDARD);
    assert(bridge_denoise_save_selection(RF_DENOISE_LEVEL_STRONG,paths,legacy_off)==ESP_ERR_INVALID_STATE);
    assert(!modern_present && bridge_denoise_config().levels==legacy);
    fail_commit=true;
    assert(bridge_denoise_save_selection(RF_DENOISE_LEVEL_STRONG,paths,mixed)==ESP_ERR_NO_MEM);
    assert(bridge_denoise_config().levels==legacy && !prepared);
    fail_commit=false;
    assert(bridge_denoise_save_selection(RF_DENOISE_LEVEL_STRONG,paths,mixed)==ESP_OK);
    assert(bridge_denoise_config().selection_level==RF_DENOISE_LEVEL_STRONG);
    assert(bridge_denoise_flags(bridge_denoise_levels())==paths);
    assert(prepared==(1u<<RF_DENOISE_BRIDGE | 1u<<RF_DENOISE_HANDSET_TX | 1u<<RF_DENOISE_RELAY_RF));
    assert(same_denoise(load_denoise_config(1),bridge_denoise_config()));
    assert(bridge_denoise_save_selection(RF_DENOISE_LEVEL_STRONG,paths,mixed)==ESP_OK && commits==1);
    assert(bridge_denoise_save_selection(RF_DENOISE_LEVEL_OFF,paths,mixed)==ESP_ERR_INVALID_ARG);
    assert(bridge_denoise_save_config(RF_DENOISE_LEVEL_OFF,paths,0)==ESP_ERR_INVALID_STATE);
    assert(bridge_denoise_save_config(RF_DENOISE_LEVEL_OFF,paths,1)==ESP_OK);
    assert(bridge_denoise_save_selection(RF_DENOISE_LEVEL_STANDARD,0,legacy_off)==ESP_OK);
    publish_denoise(load_denoise_config(1));
    assert(!bridge_denoise_levels() && bridge_denoise_config().selection_level==RF_DENOISE_LEVEL_OFF);
    assert(bridge_denoise_config().paths==paths && bridge_denoise_config().revision==2);
    assert(bridge_denoise_save_config(RF_DENOISE_LEVEL_OFF,0,2)==ESP_OK);
    assert(bridge_denoise_config().paths==0);
    assert(bridge_denoise_save_config(RF_DENOISE_LEVEL_EXTREME,paths,3)==ESP_OK);
    assert(bridge_denoise_flags(bridge_denoise_levels())==paths);
    assert(bridge_denoise_save_config(RF_DENOISE_LEVEL_OFF,paths,4)==ESP_OK);
    const uint32_t local_speaker=1u<<DENOISE_SETTING_SPEAKER;
    assert(denoise_update(NETCOM_MODE_LOCAL,local_speaker,0,local_speaker,true)==ESP_OK);
    assert(bridge_denoise_level(bridge_denoise_levels(),DENOISE_SETTING_SPEAKER)==RF_DENOISE_LEVEL_STANDARD);
    assert(legacy==(RF_DENOISE_LEVEL_LIGHT | (RF_DENOISE_LEVEL_EXTREME << 27)));
    puts("PASS: OFF with ten routes selected, edits while off, legacy migration and no-op, revision conflicts and failed save");
}
"""


def main():
    source = (MAIN / "bridge.c").read_text(encoding="utf-8")
    header = (MAIN / "bridge.h").read_text(encoding="utf-8")
    definitions = header[header.index("enum {\n    DENOISE_SETTING_"):
                         header.index("uint8_t bridge_denoise_network_setting")]
    keys = re.findall(r"^#define (?:NVS_NS|KEY_(?:LOCAL_)?DENOISE\w*|DENOISE_MODES|DENOISE_LOCAL|DENOISE_LEVEL_FIELD)\b.*$",
                      source, flags=re.M)
    names = [
        "bridge_denoise_network_setting", "denoise_mode_mask", "bridge_denoise_route",
        "denoise_level_shift", "bridge_denoise_levels", "bridge_denoise_config", "publish_denoise",
        "bridge_denoise_level", "bridge_denoise_flags", "denoise_levels_from_flags",
        "denoise_selection_level", "denoise_levels_valid", "prepare_denoise",
        "same_denoise", "save_denoise_locked", "valid_selection_level", "bridge_denoise_save_selection",
        "bridge_denoise_save_config",
        "denoise_update", "load_denoise_config",
    ]
    code = STUBS + definitions + "static bridge_denoise_config_t s_denoise_selection;\n" + \
        "\n".join(keys) + "\n" + "".join(function(source,n) for n in names) + TEST
    with tempfile.TemporaryDirectory(prefix="denoise-selection-", dir=ROOT / ".tmp") as temp:
        test = Path(temp) / "test.c"
        test.write_text(code, encoding="utf-8")
        output = Path(temp) / "test.exe"
        env = dict(os.environ, TEMP=temp, TMP=temp, TMPDIR=temp)
        subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-I", str(MAIN), str(test), "-o", str(output)], check=True, env=env)
        subprocess.run([str(output)], check=True, env=env)


if __name__ == "__main__":
    main()
