"""Run production codec wrapper and vendor driver on a fault-injecting I2C bus.

No board is contacted. Checks exact REG31 restoration, reserved-bit retention,
existing mute protection, bus/readback faults and read-only register snapshots.
"""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
STUBS = {
    'esp_types.h': '#pragma once\n#include <stdint.h>\n#include <stdbool.h>\n#include <stdlib.h>\n#define BIT(n) (1U<<(n))\n',
    'esp_err.h': '''#pragma once
#include <assert.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_NO_MEM 3
#define ESP_ERR_TIMEOUT 4
#define ESP_ERROR_CHECK(e) assert((e)==ESP_OK)
static inline const char *esp_err_to_name(int e) {(void)e;return "error";}
''',
    'esp_log.h': '''#pragma once
#include <stdio.h>
#define ESP_LOGI(tag,...) ((void)(tag))
#define ESP_LOGW(tag,...) ((void)(tag))
#define ESP_LOGE(tag,...) ((void)(tag))
''',
    'esp_check.h': '''#pragma once
#define ESP_RETURN_ON_ERROR(expr,tag,...) do {int result=(expr);(void)(tag);if(result)return result;}while(0)
#define ESP_RETURN_ON_FALSE(cond,err,tag,...) do {(void)(tag);if(!(cond))return (err);}while(0)
''',
    'driver/gpio.h': '#pragma once\n',
    'driver/i2c_master.h': '''#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef void *i2c_master_dev_handle_t;
esp_err_t i2c_master_transmit(void*,const uint8_t*,size_t,int);
esp_err_t i2c_master_transmit_receive(void*,const uint8_t*,size_t,uint8_t*,size_t,int);
''',
    'freertos/FreeRTOS.h': '''#pragma once
#define pdMS_TO_TICKS(n) (n)
#define portMAX_DELAY 0xffffffffU
typedef struct {int held;} StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
''',
    'freertos/semphr.h': '''#pragma once
#include <assert.h>
#include "freertos/FreeRTOS.h"
static inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *p) {p->held=0;return p;}
static inline void xSemaphoreTake(SemaphoreHandle_t p,unsigned wait) {(void)wait;assert(p && !p->held);p->held=1;}
static inline void xSemaphoreGive(SemaphoreHandle_t p) {assert(p->held);p->held=0;}
''',
    'freertos/task.h': '#pragma once\nstatic inline void vTaskDelay(unsigned ms) {(void)ms;}\n',
    'audio_i2s.h': '#pragma once\n#define AUDIO_SAMPLE_RATE_HZ 16000\n#define AUDIO_MCLK_MULTIPLE 256\n',
    'board_hw.h': '''#pragma once
#include "driver/i2c_master.h"
#define BOARD_ES8311_I2C_ADDR 0x18
void *board_i2c_bus(void);
esp_err_t board_i2c_add_device(int,int,i2c_master_dev_handle_t*);
void board_i2c_remove_device(i2c_master_dev_handle_t);
''',
}

HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "audio_codec.h"
#include "board_hw.h"
static uint8_t registers[256];
static int fail_read=-1, fail_write=-1, corrupt_read=-1;
static unsigned writes, reads;
void *board_i2c_bus(void) {return registers;}
esp_err_t board_i2c_add_device(int addr,int unused,i2c_master_dev_handle_t *dev) {
    assert(addr==0x18);(void)unused;*dev=registers;return ESP_OK;
}
void board_i2c_remove_device(i2c_master_dev_handle_t dev) {assert(dev==registers);}
esp_err_t i2c_master_transmit(void *dev,const uint8_t *data,size_t n,int timeout) {
    assert(dev==registers && n==2 && timeout>0);++writes;
    if(fail_write==data[0])return ESP_ERR_TIMEOUT;
    registers[data[0]]=data[1];return ESP_OK;
}
esp_err_t i2c_master_transmit_receive(void *dev,const uint8_t *data,size_t n,uint8_t *out,size_t m,int timeout) {
    assert(dev==registers && n==1 && m==1 && timeout>0);++reads;
    if(fail_read==data[0])return ESP_ERR_TIMEOUT;
    *out=registers[data[0]]^(corrupt_read==data[0] ? 1 : 0);return ESP_OK;
}
int main(void) {
    uint8_t original=0,readback=0;
    audio_codec_diag_t snapshot;
    assert(audio_codec_diag_mute_begin(&original)==ESP_ERR_INVALID_STATE);
    assert(audio_codec_init()==ESP_OK && audio_codec_ready());
    const unsigned prior_writes=writes;
    assert(audio_codec_diag_read(&snapshot)==ESP_OK && snapshot.count==AUDIO_CODEC_DIAG_REG_COUNT);
    assert(snapshot.clock_format_match && prior_writes==writes);
    registers[9]^=1;
    assert(audio_codec_diag_read(&snapshot)==ESP_OK && !snapshot.clock_format_match);
    registers[9]^=1;
    fail_read=4;unsigned start_reads=reads;
    assert(audio_codec_diag_read(&snapshot)==ESP_ERR_TIMEOUT && snapshot.count==4 && reads-start_reads==5);
    fail_read=-1;
    registers[0x31]=0x85; /* Non-mute bits must survive every path. */
    fail_read=0x31;
    assert(audio_codec_diag_mute_begin(&original)==ESP_ERR_TIMEOUT);
    fail_read=-1;
    assert(audio_codec_diag_mute_begin(&original)==ESP_OK && original==0x85 && registers[0x31]==0x85);
    assert(audio_codec_diag_mute_begin(&original)==ESP_ERR_INVALID_STATE);
    start_reads=reads;
    assert(audio_codec_diag_read(&snapshot)==ESP_ERR_INVALID_STATE && reads==start_reads);
    assert(audio_codec_mute(false)==ESP_ERR_INVALID_STATE);
    assert(audio_codec_diag_mute_set(true,&readback)==ESP_OK && readback==0xe5 && registers[0x31]==0xe5);
    fail_write=0x31;
    assert(audio_codec_diag_mute_end(&readback)==ESP_ERR_TIMEOUT && registers[0x31]==0xe5);
    assert(audio_codec_mute(false)==ESP_ERR_INVALID_STATE);
    fail_write=-1;corrupt_read=0x31;
    assert(audio_codec_diag_mute_end(&readback)==ESP_ERR_INVALID_STATE && readback==0x84);
    assert(audio_codec_mute(false)==ESP_ERR_INVALID_STATE);
    corrupt_read=-1;
    assert(audio_codec_diag_mute_end(&readback)==ESP_OK && readback==0x85);
    assert(audio_codec_diag_mute_end(&readback)==ESP_ERR_INVALID_STATE);
    assert(audio_codec_mute(true)==ESP_OK && registers[0x31]==0xe5);
    assert(audio_codec_diag_mute_begin(&original)==ESP_ERR_INVALID_STATE && registers[0x31]==0xe5);
    assert(audio_codec_mute(false)==ESP_OK && registers[0x31]==0x85);
    /* A write can land even though its verification read fails. */
    assert(audio_codec_diag_mute_begin(&original)==ESP_OK);
    fail_read=0x31;
    assert(audio_codec_diag_mute_set(true,&readback)==ESP_ERR_TIMEOUT && registers[0x31]==0xe5);
    assert(audio_codec_diag_mute_end(&readback)==ESP_ERR_TIMEOUT);
    assert(audio_codec_mute(false)==ESP_ERR_INVALID_STATE);
    fail_read=-1;
    assert(audio_codec_diag_mute_end(&readback)==ESP_OK && readback==0x85);
    assert(prior_writes<writes);
    puts("PASS: production codec + vendor init/mute, exact REG31 restore, I2C faults/readback and snapshots");
    return 0;
}
'''


def main():
    scratch = ROOT / '.tmp'
    scratch.mkdir(exist_ok=True)
    env = os.environ.copy()
    env.update(TMP=str(scratch), TEMP=str(scratch))
    with tempfile.TemporaryDirectory(prefix='codec-diag-', dir=scratch) as tmp:
        p = Path(tmp)
        for name, body in STUBS.items():
            dest = p / name
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_text(body, encoding='utf-8')
        for name in ('audio_codec.c', 'audio_codec.h'):
            (p / name).write_bytes((ROOT / 'firmware/main' / name).read_bytes())
        vendor = ROOT / 'firmware/components/es8311'
        for source in (vendor / 'es8311.c', vendor / 'include/es8311.h', vendor / 'priv_include/es8311_reg.h'):
            (p / source.name).write_bytes(source.read_bytes())
        (p / 'test.c').write_text(HARNESS, encoding='utf-8')
        exe = p / 'test.exe'
        subprocess.run(['gcc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-Wno-sign-compare',
                        '-I', str(p), str(p / 'audio_codec.c'), str(p / 'es8311.c'),
                        str(p / 'test.c'), '-o', str(exe)], check=True, env=env)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
