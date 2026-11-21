#include "net_audio_denoise.h"

#include <string.h>
#include "bridge.h"
#include "netcom.h"
#include "ptt.h"
#include "voice.h"

enum { NET_AUDIO_DRAIN_MS = 2000 };

bool net_rf_close_hold(net_rf_close_t *close, bool enabled, bool draining)
{
    if (!enabled || !ptt_owned(PTT_OWNER_NETWORK, 1) || !ptt_bridge_keyed()) {
        voice_net_tail_stop();
        *close = (net_rf_close_t){0};
        return false;
    }
    if (!close->armed) {
        ptt_begin_release(PTT_OWNER_NETWORK, 1);
        close->armed = true;
    }
    if (draining || voice_net_tail_active()) { return true; }
    if (close->local_tail && !close->tail_attempted) {
        close->tail_attempted = true;
        if (voice_net_tail_start()) { return true; }
    }
    *close = (net_rf_close_t){0};
    return false;
}

net_audio_source_t net_audio_source(const uint8_t mac[6], uint16_t epoch,
                                    uint16_t first_seq, bool feeding)
{
    net_audio_source_t source = {.first_seq = first_seq, .present = true, .feeding = feeding};
    memcpy(&source.sender_epoch, mac, 6);
    memcpy((uint8_t *)&source.sender_epoch + 6, &epoch, sizeof(epoch));
    return source;
}

void net_audio_denoise_reset(net_audio_denoise_t *dn)
{
    if (dn->requested) { rf_denoise_stop(dn->route); }
    *dn = (net_audio_denoise_t){0};
}

bool net_audio_denoise_pending(net_audio_denoise_t *dn, int64_t now)
{
    if (dn->pending && ((dn->close_ms && now - dn->close_ms >= NET_AUDIO_DRAIN_MS) ||
                       now - dn->last_pcm_ms >= 2 * NET_AUDIO_DRAIN_MS)) {
        net_audio_denoise_reset(dn);
    }
    return dn->pending;
}

void net_audio_denoise_add_lead(net_audio_denoise_t *dn, const int16_t *block, int64_t now)
{
    rf_denoise_play_add_lead(dn->route, block);
    dn->pending = rf_denoise_play_pending(dn->route) != 0;
    dn->last_pcm_ms = now;
}

bool net_audio_denoise_render(net_audio_denoise_t *dn, uint8_t mode,
                              net_audio_source_t source, bool have, bool output_ready,
                              int16_t *block, int64_t now)
{
    const uint8_t current_mode = netcom_mode();
    const ptt_owner_t owner = ptt_owner();
    const bool rf = owner == PTT_OWNER_NETWORK && ptt_bridge_keyed();
    if (source.rf_suppressed) {
        have = false;
        if (dn->latched) { source = dn->source; }
        source.feeding = false;
    }
    const bool owns_output = voice_mode() == VOICE_MONITOR ?
        !ptt_local_audio_active() && (owner == PTT_OWNER_NONE || owner == PTT_OWNER_NETWORK) :
        !voice_dac_held_by_talk();
    const bool allowed = current_mode == mode && owns_output;
    const uint16_t room = netcom_room();
    const bool changed = net_audio_source_changed(dn, source);
    const bool defer = net_audio_denoise_defer(dn, source);
    const bool route_changed = dn->latched && dn->rf != rf;
    const uint8_t first = bridge_denoise_network_setting(mode, DENOISE_NET_SPEAKER);
    const uint32_t levels = bridge_denoise_levels();
    const rf_denoise_level_t speaker_level = bridge_denoise_level(levels, first);
    const rf_denoise_level_t rf_level = first ? bridge_denoise_level(levels, first + 1) : RF_DENOISE_LEVEL_OFF;
    const bool opt_in = speaker_level || rf_level;
    if (!allowed || route_changed || (dn->latched && dn->room != room) ||
        (changed && !source.previous_completed && !defer)) {
        if (dn->requested || (dn->latched && opt_in)) { voice_bridge_network_discard(); }
        net_audio_denoise_reset(dn);
        if (!allowed) { return have && !opt_in && current_mode == mode; }
    } else if (defer) {
        have = false;
        source.feeding = false;
    } else if (changed) {
        net_audio_denoise_reset(dn);
    }
    if (have && !dn->latched) {
        const rf_denoise_level_t level = rf ? rf_level : speaker_level;
        dn->source = source;
        dn->room = room;
        dn->latched = true;
        dn->rf = rf;
        dn->requested = level != RF_DENOISE_LEVEL_OFF;
        if (dn->requested) {
            dn->route = bridge_denoise_route(first + rf);
            dn->active = rf_denoise_start(dn->route, level);
        }
    }
    if (have) {
        dn->last_pcm_ms = now;
        dn->close_ms = 0;
        if (!dn->active) { return true; }
        if (!rf_denoise_play_block(dn->route, block, RF_DENOISE_MAX_BLOCK)) {
            net_audio_denoise_reset(dn);
            return false;
        }
        dn->pending = rf_denoise_play_pending(dn->route) != 0;
        return true;
    }
    if (source.feeding) { dn->close_ms = 0; }
    if (!dn->pending) {
        if (!source.feeding) { net_audio_denoise_reset(dn); }
        return false;
    }
    if (!source.feeding && !dn->close_ms) { dn->close_ms = now; }
    if (!output_ready) { return false; }
    if (!source.feeding && !dn->finished) {
        if (!rf_denoise_play_finish(dn->route)) {
            net_audio_denoise_reset(dn);
            return false;
        }
        dn->finished = true;
    }
    /* A live jitter-buffer miss can spend the audio already queued for DSP.
     * Missing DSP results fall back to the corresponding dry PCM. */
    const size_t n = rf_denoise_play_take(dn->route, block, RF_DENOISE_MAX_BLOCK);
    memset(block + n, 0, (RF_DENOISE_MAX_BLOCK - n) * sizeof(*block));
    dn->pending = rf_denoise_play_pending(dn->route) != 0;
    if (!source.feeding && !dn->pending) { net_audio_denoise_reset(dn); }
    return n != 0;
}
