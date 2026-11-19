#include "net_audio_denoise.h"

#include <string.h>
#include "bridge.h"
#include "netcom.h"
#include "ptt.h"
#include "voice.h"

enum { NET_AUDIO_DRAIN_MS = 2000 };

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

bool net_audio_denoise_render(net_audio_denoise_t *dn, uint8_t mode,
                              net_audio_source_t source, bool have, bool output_ready,
                              int16_t *block, int64_t now)
{
    const uint8_t current_mode = netcom_mode();
    const ptt_owner_t owner = ptt_owner();
    const bool rf = owner == PTT_OWNER_NETWORK && ptt_bridge_keyed();
    const bool owns_output = voice_mode() == VOICE_MONITOR ?
        !ptt_local_audio_active() && (owner == PTT_OWNER_NONE || owner == PTT_OWNER_NETWORK) :
        !voice_dac_held_by_talk();
    const bool allowed = current_mode == mode && owns_output;
    const uint16_t room = netcom_room();
    const bool changed = dn->latched && source.present &&
        (dn->source.sender_epoch != source.sender_epoch || dn->source.first_seq != source.first_seq);
    const bool route_changed = dn->latched && dn->rf != rf;
    const uint8_t first = bridge_denoise_network_setting(mode, DENOISE_NET_SPEAKER);
    const uint32_t flags = bridge_denoise_flags();
    const bool opt_in = (flags & (3u << first)) != 0;
    if (!allowed || changed || route_changed || (dn->latched && dn->room != room)) {
        if (dn->requested || (dn->latched && opt_in)) { voice_bridge_network_discard(); }
        net_audio_denoise_reset(dn);
        if (!allowed) { return have && !opt_in && current_mode == mode; }
    }
    if (have && !dn->latched) {
        const uint8_t setting = first + rf;
        dn->source = source;
        dn->room = room;
        dn->latched = true;
        dn->rf = rf;
        dn->requested = (flags & (1u << setting)) != 0;
        if (dn->requested) {
            dn->route = bridge_denoise_route(setting);
            dn->active = rf_denoise_start(dn->route);
        }
    }
    if (have) {
        dn->last_pcm_ms = now;
        dn->close_ms = 0;
        if (!dn->active) { return true; }
        dn->pending = rf_denoise_play_block(dn->route, block, RF_DENOISE_MAX_BLOCK);
        if (!dn->pending) { net_audio_denoise_reset(dn); }
        return dn->pending;
    }
    if (source.feeding) { dn->close_ms = 0; return false; }
    if (!dn->pending) {
        net_audio_denoise_reset(dn);
        return false;
    }
    if (!dn->close_ms) { dn->close_ms = now; }
    if (!output_ready) { return false; }
    if (!dn->finished) {
        if (!rf_denoise_play_finish(dn->route)) {
            net_audio_denoise_reset(dn);
            return false;
        }
        dn->finished = true;
    }
    const size_t n = rf_denoise_play_take(dn->route, block, RF_DENOISE_MAX_BLOCK);
    memset(block + n, 0, (RF_DENOISE_MAX_BLOCK - n) * sizeof(*block));
    if (!rf_denoise_play_pending(dn->route)) { net_audio_denoise_reset(dn); }
    return n != 0;
}
