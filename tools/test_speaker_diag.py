"""Compile the production diagnostic state machine against host-only drivers.

Exercises real record/replay PCM, stable capture leases, time limits, stop/drain,
and PTT refusal. It does not connect to hardware or emit audio.
"""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
STUBS = {
    'esp_err.h': '''#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_NOT_FOUND 3
#define ESP_ERR_NO_MEM 4
''',
    'freertos/FreeRTOS.h': '''#pragma once
#include <assert.h>
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
#define pdMS_TO_TICKS(x) (x)
''',
    'freertos/task.h': '''#pragma once
void vTaskDelay(unsigned ms);
''',
    'esp_timer.h': '#pragma once\n#include <stdint.h>\nint64_t esp_timer_get_time(void);\n',
    'esp_heap_caps.h': '''#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 1
#define heap_caps_malloc(n,c) malloc(n)
#define heap_caps_free(p) free(p)
''',
    'audio_i2s.h': '#pragma once\n#define AUDIO_SAMPLE_RATE_HZ 16000\n',
    'audio_codec.h': '''#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
esp_err_t audio_codec_diag_mute_begin(uint8_t*);
esp_err_t audio_codec_diag_mute_set(bool,uint8_t*);
esp_err_t audio_codec_diag_mute_end(uint8_t*);
''',
    'board_hw.h': '#pragma once\n#include <stdbool.h>\nvoid board_amp_enable(bool on);\n',
    'ptt.h': '''#pragma once
#include <stdbool.h>
bool ptt_local_audio_begin(void);
void ptt_local_audio_end(void);
bool ptt_local_audio_active(void);
''',
    'voice.h': '#pragma once\nint voice_get_speaker_level(void);\n',
}

HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "speaker_diag.h"
static int64_t clock_us;
static bool reserved, tx_busy, amp;
static bool codec_owned;
static uint8_t codec_byte = 0x85, codec_original;
static int begin_error, apply_error, restore_errors;
esp_err_t audio_codec_diag_mute_begin(uint8_t *original) {
    if (begin_error) return ESP_ERR_INVALID_STATE;
    assert(!codec_owned); codec_owned=true; *original=codec_original=codec_byte; return ESP_OK;
}
esp_err_t audio_codec_diag_mute_set(bool mute,uint8_t *readback) {
    assert(codec_owned);
    codec_byte=mute ? codec_original|0x60 : codec_original;
    if (apply_error) return ESP_ERR_INVALID_STATE;
    *readback=codec_byte; return ESP_OK;
}
esp_err_t audio_codec_diag_mute_end(uint8_t *readback) {
    assert(codec_owned);
    if (restore_errors) { --restore_errors; return ESP_ERR_INVALID_STATE; }
    *readback=codec_byte=codec_original; codec_owned=false; return ESP_OK;
}
int64_t esp_timer_get_time(void) { return clock_us; }
void vTaskDelay(unsigned ms) { clock_us += ms * 1000; }
bool ptt_local_audio_begin(void) { if (reserved || tx_busy) return false; reserved = true; return true; }
bool ptt_local_audio_active(void) { return reserved; }
void ptt_local_audio_end(void) { assert(reserved); reserved = false; }
void board_amp_enable(bool on) { amp = on; }
int voice_get_speaker_level(void) { return 7; }
size_t strlcpy(char *d, const char *s, size_t n) {
    const size_t len = strlen(s); if (n) { const size_t take = len < n-1 ? len : n-1; memcpy(d,s,take); d[take]=0; } return len;
}
static void advance_block(void) { clock_us += 16000; speaker_diag_tick(); }
static void finish_blocks(int16_t *out) {
    for(int i=0;i<400 && speaker_diag_output_active();i++) {
        bool drain=speaker_diag_render(out,256);
        if (speaker_diag_output_amp_on()) amp=true;
        speaker_diag_render_written(out,256,drain); advance_block();
    }
    assert(!reserved && !speaker_diag_output_active() && !amp && !codec_owned);
}
int main(void) {
    speaker_diag_stats_t s;
    int16_t in[256], local[256], out[256];
    for (int i=0;i<256;i++) { in[i]=(int16_t)(i-128); local[i]=(int16_t)(in[i]/2); }
    speaker_meter_t meter = {0};
    const int16_t rail[] = {-32768,32767,0,10};
    speaker_meter_add(&meter,rail,4);
    assert(meter.peak==32768 && meter.rail_samples==2 && meter.samples==4);
    assert(speaker_diag_command("bad",1000)==ESP_ERR_INVALID_ARG);
    assert(speaker_diag_command("tone",5001)==ESP_ERR_INVALID_ARG);
    assert(speaker_diag_command("replay",1000)==ESP_ERR_NOT_FOUND);
    assert(speaker_diag_command("arm",1000)==ESP_OK);
    speaker_diag_observe(in,local,256,false,100);
    speaker_diag_get(&s); assert(s.captured==0 && s.mode==SPEAKER_DIAG_ARMED);
    for(int i=0;i<63;i++) { advance_block(); speaker_diag_observe(in,local,256,true,110); }
    speaker_diag_get(&s);
    assert(s.mode==SPEAKER_DIAG_IDLE && s.captured==16000 && s.capture_speaker_step==7);
    assert(s.common.samples==63*256 && s.local.peak==64);
    const int16_t *a,*b; size_t n;
    assert(speaker_diag_capture_acquire(&a,&b,&n) && n==16000);
    assert(a[0]==-128 && b[0]==-64);
    assert(speaker_diag_command("arm",1000)==ESP_ERR_INVALID_STATE);
    speaker_diag_capture_release();
    tx_busy=true; assert(speaker_diag_command("replay",1000)==ESP_ERR_INVALID_STATE); tx_busy=false;
    assert(speaker_diag_command("replay",1000)==ESP_OK && reserved);
    bool drain=speaker_diag_render(out,256); assert(!drain && !memcmp(out,in,sizeof(in)));
    speaker_diag_render_written(out,256,drain);
    assert(speaker_diag_command("stop",1000)==ESP_OK && reserved);
    for(int i=0;i<5;i++) {
        drain=speaker_diag_render(out,256); assert(drain);
        for(int j=0;j<256;j++) assert(out[j]==0);
        speaker_diag_render_written(out,256,drain); if(i<4) assert(reserved);
    }
    assert(!reserved && !amp);
    assert(speaker_diag_command("tone",250)==ESP_OK);
    unsigned peak=0;
    for(int i=0;i<22 && speaker_diag_output_active();i++) {
        drain=speaker_diag_render(out,256);
        for(int j=0;j<256;j++) { unsigned v=out[j]<0?(unsigned)-out[j]:(unsigned)out[j]; if(v>peak)peak=v; }
        speaker_diag_render_written(out,256,drain); advance_block();
    }
    assert(peak==2048 && !reserved && !speaker_diag_output_active());
    assert(speaker_diag_command("silence",1000)==ESP_OK);
    drain=speaker_diag_render(out,256); assert(!drain);
    for(int j=0;j<256;j++) assert(out[j]==0);
    clock_us+=2000000; speaker_diag_tick(); assert(reserved);
    clock_us+=300000; speaker_diag_tick(); assert(!reserved && !speaker_diag_output_active());
    assert(speaker_diag_command("arm",1000)==ESP_OK);
    clock_us+=61000000; speaker_diag_tick();
    speaker_diag_get(&s); assert(s.mode==SPEAKER_DIAG_IDLE && s.captured==0);
    assert(speaker_diag_command("arm",1000)==ESP_OK);
    speaker_diag_observe(in,local,256,true,100); speaker_diag_observe(in,local,256,false,100);
    speaker_diag_get(&s); assert(s.mode==SPEAKER_DIAG_IDLE && s.captured==256);
    assert(speaker_diag_command("replay",1000)==ESP_OK);
    drain=speaker_diag_render(out,256); assert(!drain && reserved);
    speaker_diag_render_written(out,256,drain); assert(reserved);
    for(int i=0;i<5;i++) { drain=speaker_diag_render(out,256); speaker_diag_render_written(out,256,drain); }
    assert(!reserved);
    begin_error=1;
    assert(speaker_diag_command("dacmute",500)==ESP_ERR_INVALID_STATE && !reserved);
    begin_error=0;
    assert(speaker_diag_command("dacmute",250)==ESP_OK && reserved && codec_owned);
    assert(codec_byte==0x85); /* Changed only on the monitor writer. */
    drain=speaker_diag_render(out,256); assert(!drain && codec_byte==0xe5);
    for(int j=0;j<256;j++) assert(out[j]==0);
    speaker_diag_render_written(out,256,drain);
    speaker_diag_get(&s); assert(s.codec_restore_pending && s.codec_readback==0xe5);
    finish_blocks(out);
    speaker_diag_get(&s);
    assert(codec_byte==0x85 && !s.codec_restore_pending && s.codec_readback==0x85 && s.codec_restore_attempts==1);

    assert(speaker_diag_command("dacmute",500)==ESP_OK);
    drain=speaker_diag_render(out,256); speaker_diag_render_written(out,256,drain);
    restore_errors=2; assert(speaker_diag_command("stop",1000)==ESP_OK);
    for(int i=0;i<5;i++) { drain=speaker_diag_render(out,256); speaker_diag_render_written(out,256,drain); }
    speaker_diag_get(&s);
    assert(s.mode==SPEAKER_DIAG_RESTORE && s.codec_restore_pending && reserved && !amp);
    assert(!speaker_diag_output_amp_on());
    assert(speaker_diag_command("tone",500)==ESP_ERR_INVALID_STATE);
    assert(speaker_diag_command("stop",1000)==ESP_OK);
    clock_us+=110000; speaker_diag_tick(); assert(reserved && codec_owned);
    clock_us+=110000; speaker_diag_tick(); assert(!reserved && !codec_owned && codec_byte==0x85);

    apply_error=1; assert(speaker_diag_command("dacmute",500)==ESP_OK);
    finish_blocks(out); apply_error=0; assert(codec_byte==0x85);
    speaker_diag_get(&s); assert(s.codec_apply_error && !s.codec_mute_applied && !s.codec_error);

    assert(speaker_diag_command("dacmute",500)==ESP_OK);
    drain=speaker_diag_render(out,256); speaker_diag_render_written(out,256,drain);
    restore_errors=1; speaker_diag_monitor_end();
    assert(!reserved && !codec_owned && codec_byte==0x85);

    /* Stop while an audio write is blocked: the drain timeout must also
     * outlast the final partial write, not just the stop command. */
    assert(speaker_diag_command("tone",500)==ESP_OK);
    drain=speaker_diag_render(out,256); assert(!drain);
    assert(speaker_diag_command("stop",1000)==ESP_OK);
    clock_us+=260000; speaker_diag_render_failed(drain); speaker_diag_tick(); assert(reserved);
    clock_us+=79000; speaker_diag_tick(); assert(reserved);
    clock_us+=1000; speaker_diag_tick(); assert(!reserved);
    puts("PASS: production speaker diagnostics: PCM/leases, PTT, stop/drain, DAC restore and I2C/write errors");
    return 0;
}
'''


def main():
    scratch = ROOT / '.tmp'
    scratch.mkdir(exist_ok=True)
    env = os.environ.copy()
    env.update(TMP=str(scratch), TEMP=str(scratch))
    with tempfile.TemporaryDirectory(prefix='speaker-diag-', dir=scratch) as tmp:
        p = Path(tmp)
        for name, body in STUBS.items():
            dest = p / name
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_text(body, encoding='utf-8')
        # Copy only the production state-machine translation unit so its
        # quoted hardware includes resolve to the host drivers above.
        for name in ('speaker_diag.c','speaker_diag.h','speaker_diag_core.h'):
            (p/name).write_bytes((ROOT/'firmware/main'/name).read_bytes())
        (p/'compat.h').write_text('#include <stddef.h>\nsize_t strlcpy(char*,const char*,size_t);\n',encoding='utf-8')
        (p/'test.c').write_text(HARNESS,encoding='utf-8')
        exe = p/'test.exe'
        subprocess.run(['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-include',str(p/'compat.h'),
                        '-I',str(p),str(p/'speaker_diag.c'),str(p/'test.c'),'-o',str(exe)],check=True,env=env)
        subprocess.run([str(exe)],check=True)


if __name__ == '__main__':
    main()
