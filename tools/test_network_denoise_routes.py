#!/usr/bin/env python3
"""Check network route selection, sample drains and TX ownership with real SpeexDSP."""
from pathlib import Path
import subprocess
import tempfile

from test_np_peer_runtime import function
from test_rf_denoise import MAIN, ROOT, HARNESS, compile_executable

FIXTURE = r'''
#include "net_audio_denoise.h"
enum { PTT_OWNER_NONE, PTT_OWNER_NETWORK, PTT_OWNER_TALK, PTT_TX, VOICE_MONITOR, BLOCK_FRAMES = 256 };
typedef int ptt_owner_t;
typedef enum { DENOISE_NET_TX, DENOISE_NET_SPEAKER, DENOISE_NET_RF } denoise_network_path_t;
static uint8_t current_mode;
static uint16_t room = 41;
static uint32_t levels;
static bool keyed, talking, local_audio;
static bool monitor = true, dac_held;
static unsigned discarded;
static ptt_owner_t owner;
static bool ptt_owned(ptt_owner_t kind, uint32_t token) { return owner == kind && token == 1; }
static bool ptt_bridge_keyed(void) { return keyed; }
static bool ptt_local_audio_active(void) { return local_audio; }
static ptt_owner_t ptt_owner(void) { return owner; }
static int ptt_get_state(void) { return talking ? PTT_TX : 0; }
static uint8_t netcom_mode(void) { return current_mode; }
static uint16_t netcom_room(void) { return room; }
static int voice_mode(void) { return monitor ? VOICE_MONITOR : 0; }
static bool voice_dac_held_by_talk(void) { return dac_held; }
static uint32_t bridge_denoise_levels(void) { return levels; }
static rf_denoise_level_t bridge_denoise_level(uint32_t config, uint8_t setting) {
    return setting ? (config >> ((setting - 1) * 3)) & 7u : RF_DENOISE_LEVEL_OFF;
}
static uint8_t bridge_denoise_network_setting(uint8_t mode, denoise_network_path_t path) {
    return (mode == 1 ? 5 : 8) + path;
}
static rf_denoise_route_t bridge_denoise_route(uint8_t setting) {
    switch (setting) {
        case 6: return RF_DENOISE_P2P_SPEAKER;
        case 7: return RF_DENOISE_P2P_RF;
        case 9: return RF_DENOISE_RELAY_SPEAKER;
        case 10: return RF_DENOISE_RELAY_RF;
        default: assert(!"unexpected setting"); return RF_DENOISE_NET_TX;
    }
}
static void voice_bridge_network_discard(void) { ++discarded; }
static int voice_apply_tx_processing(int16_t *pcm, size_t n) {
    for (size_t i = 0; i < n; ++i) pcm[i] += 13;
    return 10;
}
static void netcom_tx_sample(const int16_t *pcm, size_t n, bool active) {
    assert(active && collect(pcm, n, NULL));
}
'''

CASES = r'''
static void fill_block(int16_t *block, size_t offset) {
    memcpy(block, input + offset, 320 * sizeof(*block));
}

static void test_rx(uint8_t mode) {
    current_mode = mode;
    const uint8_t mac[6] = {2, 0, 0, 0, 0, 11};
    const net_audio_source_t over = net_audio_source(mac, 1234, 501, true);
    const net_audio_source_t ended = net_audio_source(mac, 1234, 501, false);
    const uint8_t first = bridge_denoise_network_setting(mode, DENOISE_NET_SPEAKER);
    const unsigned speaker_shift = (first - 1) * 3;
    const unsigned rf_shift = first * 3;
    const uint32_t speaker = (uint32_t)RF_DENOISE_LEVEL_STANDARD << speaker_shift;
    const uint32_t rf = (uint32_t)RF_DENOISE_LEVEL_STANDARD << rf_shift;
    int16_t block[320], zero[320] = {0};
    process(640, 170);
    memcpy(reference, output, 640 * sizeof(*output));
    for (unsigned route = 0; route < 2; ++route) {
        keyed = route != 0;
        owner = keyed ? PTT_OWNER_NETWORK : PTT_OWNER_NONE;
        for (unsigned all = 0; all < 4; ++all) {
            levels = ((all & 1) ? speaker : 0) | ((all & 2) ? rf : 0);
            const bool enabled = (levels & (keyed ? rf : speaker)) != 0;
            net_audio_denoise_t dn = {0};
            fill_block(block, 0);
            assert(net_audio_denoise_render(&dn, mode, over, true, true, block, 10));
            assert(net_audio_denoise_pending(&dn, 10) == enabled);
            assert(!memcmp(block, enabled ? zero : input, sizeof(block)));
            assert(!net_audio_denoise_render(&dn, mode, over, false, true, block, 100));
            assert(net_audio_denoise_pending(&dn, 100) == enabled);
            fill_block(block, 320);
            assert(net_audio_denoise_render(&dn, mode, ended, true, true, block, 110));
            if (enabled) {
                assert(!memcmp(block, zero, 160 * sizeof(*block)));
                assert(!memcmp(block + 160, reference, 160 * sizeof(*block)));
            } else {
                assert(!memcmp(block, input + 320, sizeof(block)));
            }
            assert(!net_audio_denoise_render(&dn, mode, ended, false, false, block, 130));
            assert(net_audio_denoise_pending(&dn, 130) == enabled);
            assert(net_audio_denoise_render(&dn, mode, ended, false, true, block, 145) == enabled);
            if (enabled) {
                assert(!memcmp(block, reference + 160, sizeof(block)));
                assert(net_audio_denoise_pending(&dn, 145));
                assert(net_audio_denoise_render(&dn, mode, ended, false, true, block, 165));
                assert(!memcmp(block, reference + 480, 160 * sizeof(*block)));
                assert(!memcmp(block + 160, zero, 160 * sizeof(*block)));
            }
            assert(!net_audio_denoise_pending(&dn, 165));
            assert(!net_audio_denoise_render(&dn, mode, ended, false, true, block, 185));
        }
    }
    levels = speaker | ((uint32_t)RF_DENOISE_LEVEL_EXTREME << rf_shift);
    owner = PTT_OWNER_NONE;
    keyed = false;
    net_audio_denoise_t dn = {0};
    const unsigned initial = discarded;
    fill_block(block, 0);
    assert(net_audio_denoise_render(&dn, mode, over, true, true, block, 200));
    assert(dn.pending && !dn.rf);
    rf_denoise_stats_t stats;
    rf_denoise_stats(dn.route, &stats);
    assert(stats.level == RF_DENOISE_LEVEL_STANDARD);
    keyed = true;
    owner = PTT_OWNER_NETWORK;
    fill_block(block, 320);
    assert(net_audio_denoise_render(&dn, mode, over, true, true, block, 220));
    assert(discarded == initial + 1 && dn.rf && !memcmp(block, zero, sizeof(block)));
    rf_denoise_stats(dn.route, &stats);
    assert(stats.level == RF_DENOISE_LEVEL_EXTREME);
    levels = speaker | ((uint32_t)RF_DENOISE_LEVEL_LIGHT << rf_shift);
    fill_block(block, 320);
    assert(net_audio_denoise_render(&dn, mode, over, true, true, block, 230));
    rf_denoise_stats(dn.route, &stats);
    assert(stats.level == RF_DENOISE_LEVEL_EXTREME);
    const net_audio_source_t newer = net_audio_source(mac, 1235, 501, true);
    fill_block(block, 320);
    assert(net_audio_denoise_render(&dn, mode, newer, true, true, block, 240));
    assert(discarded == initial + 2 && !memcmp(block, zero, sizeof(block)));
    rf_denoise_stats(dn.route, &stats);
    assert(stats.level == RF_DENOISE_LEVEL_LIGHT);
    ++room;
    fill_block(block, 320);
    assert(net_audio_denoise_render(&dn, mode, newer, true, true, block, 260));
    assert(discarded == initial + 3 && !memcmp(block, zero, sizeof(block)));
    owner = PTT_OWNER_TALK;
    local_audio = true;
    assert(!net_audio_denoise_render(&dn, mode, ended, false, true, block, 280));
    assert(discarded == initial + 4 && !dn.pending);
    owner = PTT_OWNER_NONE;
    local_audio = false;
    keyed = false;
    fill_block(block, 0);
    assert(net_audio_denoise_render(&dn, mode, over, true, true, block, 300));
    assert(net_audio_denoise_pending(&dn, 2300)); // A live network gap is not EOF.
    assert(!net_audio_denoise_render(&dn, mode, ended, false, false, block, 2300));
    assert(!net_audio_denoise_pending(&dn, 4300)); // Stalled output cannot hold key forever.
    assert(!net_audio_denoise_render(&dn, mode, ended, false, true, block, 4320));
    monitor = false;
    owner = PTT_OWNER_TALK;
    fill_block(block, 0);
    assert(net_audio_denoise_render(&dn, mode, over, true, true, block, 4400));
    assert(dn.pending && !dn.rf); // Network-only talk leaves the DAC with the receiver.
    dac_held = true;
    assert(!net_audio_denoise_render(&dn, mode, over, false, false, block, 4420));
    assert(!dn.pending);
    monitor = true;
    dac_held = false;
}

static void test_completed_over(uint8_t mode, bool rf, bool split_drain) {
    current_mode = mode;
    keyed = rf;
    owner = rf ? PTT_OWNER_NETWORK : PTT_OWNER_NONE;
    const uint8_t first = bridge_denoise_network_setting(mode, DENOISE_NET_SPEAKER);
    levels = RF_DENOISE_LEVEL_STANDARD << ((first + rf - 1) * 3);
    const uint8_t mac[6] = {2, 0, 0, 0, 0, 23};
    const net_audio_source_t old = net_audio_source(mac, 545, 501, true);
    net_audio_source_t end = old;
    end.feeding = false;
    net_audio_source_t next = net_audio_source(mac, 546, 504, true);
    next.previous_completed = true;
    int16_t block[320];
    process(640, 256);
    memcpy(reference, output, 640 * sizeof(*output));
    const unsigned before = discarded;
    const rf_denoise_route_t route = bridge_denoise_route(first + rf);
    rf_denoise_stats_t stats;
    rf_denoise_stats(route, &stats);
    const uint32_t prior_discarded = stats.discarded_samples;
    net_audio_denoise_t dn = {0};
    for (unsigned i = 0; i < 2; ++i) {
        fill_block(block, 320 * i);
        assert(net_audio_denoise_render(&dn, mode, old, true, true, block, 100 + 20 * i));
    }
    assert(!memcmp(block + 160, reference, 160 * sizeof(*block)));
    assert(net_audio_denoise_defer(&dn, next));
    assert(!net_audio_denoise_render(&dn, mode, next, false, false, block, 135));
    assert(rf_denoise_play_pending(route) == 480);
    const net_audio_source_t closing = split_drain ? end : next;
    assert(net_audio_denoise_render(&dn, mode, closing, false, true, block, 140));
    assert(!memcmp(block, reference + 160, sizeof(block)));
    assert(net_audio_denoise_defer(&dn, next));
    assert(net_audio_denoise_render(&dn, mode, next, false, true, block, 160));
    assert(!memcmp(block, reference + 480, 160 * sizeof(*block)));
    assert(!net_audio_denoise_defer(&dn, next) && discarded == before);
    rf_denoise_stats(route, &stats);
    assert(stats.discarded_samples == prior_discarded);
    fill_block(block, 640);
    assert(net_audio_denoise_render(&dn, mode, next, true, true, block, 180));
    assert(dn.source.sender_epoch == next.sender_epoch);
    net_audio_denoise_reset(&dn);
}

int main(void) {
    uint32_t rand_state = 42;
    for (size_t i = 0; i < sizeof(input) / sizeof(*input); ++i) {
        rand_state = rand_state * 1664525u + 1013904223u;
        input[i] = (int16_t)((rand_state >> 17) - 16384);
    }
    for (unsigned route = 0; route < RF_DENOISE_ROUTE_COUNT; ++route) rf_denoise_prepare(route);
    for (uint8_t mode = 1; mode <= 2; ++mode) {
        test_rx(mode);
        for (unsigned repeat = 0; repeat < 1000; ++repeat) {
            test_completed_over(mode, repeat & 1, repeat & 2);
        }
        const size_t short_length = 109;
        process(short_length, 64);
        memcpy(reference, output, short_length * sizeof(*output));
        written = 0;
        current_mode = mode;
        owner = PTT_OWNER_TALK;
        talking = true;
        struct talk_network_denoise tx = {.generation = 1, .room = room, .mode = mode};
        assert(rf_denoise_start(RF_DENOISE_NET_TX, RF_DENOISE_LEVEL_STANDARD));
        assert(rf_denoise_push(RF_DENOISE_NET_TX, input, short_length, talk_network_emit, &tx));
        assert(written == 0);
        assert(rf_denoise_finish(RF_DENOISE_NET_TX, talk_network_emit, &tx));
        assert(written == short_length && tx.peak == 10);
        for (size_t i = 0; i < written; ++i) assert(output[i] == reference[i] + 13);
        assert(rf_denoise_start(RF_DENOISE_NET_TX, RF_DENOISE_LEVEL_STANDARD));
        assert(rf_denoise_push(RF_DENOISE_NET_TX, input, short_length, talk_network_emit, &tx));
        owner = PTT_OWNER_NONE;
        assert(!rf_denoise_finish(RF_DENOISE_NET_TX, talk_network_emit, &tx));
        assert(written == short_length);
        owner = PTT_OWNER_TALK;
        current_mode = 3 - mode;
        assert(!talk_network_emit(input, short_length, &tx) && written == short_length);
        current_mode = mode;
        ++room;
        assert(!talk_network_emit(input, short_length, &tx) && written == short_length);
        rf_denoise_stop(RF_DENOISE_NET_TX);
    }
    puts("PASS: P2P/MQTT four-way RX routing, exact tail PCM, RF priority, timeout and sender/owner changes");
    puts("PASS: 1000 completed-over transitions per mode retain all 480 samples through prefill and backpressure");
    puts("PASS: real network TX denoise output enters mic shaping once, short drain and PTT/mode/room fences");
    return 0;
}
'''


def main():
    adapter = (MAIN / 'net_audio_denoise.c').read_text(encoding='utf-8')
    adapter = '\n'.join(line for line in adapter.splitlines() if not line.startswith('#include'))
    voice = (MAIN / 'voice.c').read_text(encoding='utf-8')
    tx_type = voice[voice.index('struct talk_network_denoise {'):voice.index('static bool talk_network_emit(')]
    harness = HARNESS[:HARNESS.index('static void compare_waves')] + FIXTURE + adapter + '\n'
    harness += tx_type + function(voice, 'talk_network_emit') + CASES
    scratch = ROOT / '.tmp'
    scratch.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='network-denoise-', dir=scratch) as folder:
        executable = compile_executable(Path(folder), harness_source=harness)
        subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    main()
