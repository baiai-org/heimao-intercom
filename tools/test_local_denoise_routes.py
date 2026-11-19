#!/usr/bin/env python3
"""Exercise the real speaker COS and TALK drain helpers with the real DSP."""
from pathlib import Path
import re
import subprocess
import tempfile

from test_bridge_speech_prefix import function
from test_rf_denoise import compile_executable

ROOT = Path(__file__).resolve().parents[1]

PRELUDE = r'''
#include "rf_denoise.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define AMP_SETTLE_SAMPLES 400
#define AUDIO_I2S_BUFFER_SAMPLES 768
#define BLOCK_FRAMES 256
#define RX_GUARD_OUT_SPK 4
#define ESP_OK 0
enum { PTT_OWNER_NONE, PTT_OWNER_TALK, PTT_OWNER_NETWORK };
enum { PTT_IDLE, PTT_TX };
typedef struct { bool ready; unsigned applied_revision; } sa818_cfg_status_t;
typedef struct { int unused; } eq6_t, tx_history_t;
static bool s_preview_amp, s_bridge_inj_amp, s_mod_sq_open, amp;
static int s_amp_settle, state, owner, blocked;
static unsigned profile = 1, mode, writes, revoke_after;
static size_t written;
static bool radio_ready = true;
static int64_t clock_us;
static bool failed;

void *heap_caps_malloc(size_t n, unsigned caps) { (void)caps; return malloc(n); }
void *heap_caps_calloc(size_t count, size_t n, unsigned caps) { (void)caps; return calloc(count,n); }
void *heap_caps_aligned_alloc(size_t alignment, size_t n, unsigned caps) {
    void *p = heap_caps_malloc(n, caps); assert((uintptr_t)p % alignment == 0); return p;
}
void heap_caps_free(void *p) { free(p); }
int64_t esp_timer_get_time(void) { return ++clock_us; }
static void board_amp_enable(bool value) { amp = value; }
static void sa818_cfg_status(sa818_cfg_status_t *out) { *out = (sa818_cfg_status_t){radio_ready,profile}; }
static bool sa818_cfg_tx_ready(void) { return radio_ready; }
static unsigned netcom_mode(void) { return mode; }
static int ptt_owner(void) { return owner; }
static bool ptt_owned(int kind, uint32_t generation) { return owner == kind && generation == 99; }
static int ptt_get_state(void) { return state; }
static bool ptt_rf_keying_enabled(void) { return true; }
static int rx_guard_blocked(void) { return blocked; }
static void talk_shape(int16_t *pcm,size_t n,eq6_t *eq,tx_history_t *h) {
    (void)pcm;(void)n;(void)eq;(void)h;
}
static int audio_i2s_write_until(const int16_t *buf,size_t n,int64_t deadline) {
    (void)buf;
    assert(deadline > clock_us && n > 0 && n <= BLOCK_FRAMES);
    if (failed) return -1;
    written += n;
    if (++writes == revoke_after) { state = PTT_IDLE; owner = PTT_OWNER_NONE; }
    return ESP_OK;
}
'''

CASES = r'''
static void speaker_open(speaker_denoise_t *dn, int16_t *buf) {
    *dn = (speaker_denoise_t){.enabled=true,.mode=mode,.profile_revision=profile};
    amp = s_mod_sq_open = true;
    for (size_t i=0;i<257;i++) buf[i] = (int16_t)(i * 17 + 100);
    speaker_denoise_render(dn, buf, 257, true);
    assert(dn->started && dn->active);
    for (size_t i=0;i<257;i++) assert(!buf[i]);
}
static void mic_open(int16_t *buf) {
    for (size_t i=0;i<110;i++) buf[i] = (int16_t)(i * 23 - 1000);
    state=PTT_TX; owner=PTT_OWNER_TALK; failed=false; writes=0; written=0; revoke_after=0;
    assert(rf_denoise_start(RF_DENOISE_HANDSET_TX));
    assert(rf_denoise_play_block(RF_DENOISE_HANDSET_TX,buf,110));
}

int main(void) {
    int16_t pcm[320];
    rf_denoise_prepare(RF_DENOISE_SPEAKER);
    rf_denoise_prepare(RF_DENOISE_HANDSET_TX);
    speaker_denoise_t dn;
    speaker_open(&dn,pcm);
    s_mod_sq_open=false; speaker_denoise_cos_closed(&dn);
    assert(amp && dn.close_us);
    for (int block=0;block<5;block++) {
        speaker_denoise_render(&dn,pcm,256,true);
        assert(amp && dn.finished);
    }
    assert(!rf_denoise_play_pending(RF_DENOISE_SPEAKER) && !dn.dma_left);
    speaker_denoise_end(&dn);
    assert(!amp && !dn.active && s_amp_settle == AMP_SETTLE_SAMPLES);
    for (int cancel=0;cancel<4;cancel++) {
        speaker_open(&dn,pcm);
        s_mod_sq_open=false; speaker_denoise_cos_closed(&dn);
        if (cancel==0) blocked=RX_GUARD_OUT_SPK;
        if (cancel==1) ++profile;
        if (cancel==2) owner=PTT_OWNER_NETWORK;
        if (cancel==3) clock_us=dn.close_us;
        speaker_denoise_render(&dn,pcm,256,true);
        assert(!amp && !dn.active && !rf_denoise_play_pending(RF_DENOISE_SPEAKER));
        blocked=0; owner=PTT_OWNER_NONE;
    }
    eq6_t eq; tx_history_t history;
    mic_open(pcm);
    assert(talk_denoise_drain(99,pcm,&eq,&history) && written==320 && writes==2);
    talk_denoise_drain_dac(99,pcm);
    assert(written==320+AUDIO_I2S_BUFFER_SAMPLES);
    mic_open(pcm); revoke_after=1;
    assert(!talk_denoise_drain(99,pcm,&eq,&history) && written==256);
    talk_denoise_drain_dac(99,pcm); assert(written==256);
    mic_open(pcm); state=PTT_IDLE;
    assert(!talk_denoise_drain(99,pcm,&eq,&history) && !writes);
    mic_open(pcm); failed=true;
    assert(!talk_denoise_drain(99,pcm,&eq,&history) && !written);
    puts("PASS: actual COS amp drain, speaker guard/profile/owner/deadline cancellation, TALK drain and ownership loss");
    return 0;
}
'''


def main():
    voice = (ROOT / 'firmware/main/voice.c').read_text(encoding='utf-8')
    speaker = re.search(r'typedef struct \{[^{}]*\} speaker_denoise_t;', voice).group()
    names = ('speaker_denoise_end', 'speaker_denoise_cos_closed', 'speaker_denoise_route',
             'speaker_denoise_render', 'talk_denoise_owned', 'talk_denoise_drain',
             'talk_denoise_drain_dac')
    harness = PRELUDE + speaker + ''.join(function(voice, name) for name in names) + CASES
    scratch = ROOT / '.tmp'
    scratch.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='denoise-voice-', dir=scratch) as temp:
        executable = compile_executable(Path(temp), harness)
        subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    main()
