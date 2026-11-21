#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    SPECTRUM_AUTO = 0,
    SPECTRUM_RF_SPEAKER,
    SPECTRUM_RF_MONITOR,
    SPECTRUM_RF_P2P,
    SPECTRUM_RF_MQTT,
    SPECTRUM_P2P_SPEAKER,
    SPECTRUM_P2P_MONITOR,
    SPECTRUM_P2P_RF,
    SPECTRUM_MQTT_SPEAKER,
    SPECTRUM_MQTT_MONITOR,
    SPECTRUM_MQTT_RF,
    SPECTRUM_MIC_RF,
    SPECTRUM_MIC_P2P,
    SPECTRUM_MIC_MQTT,
    SPECTRUM_REMOTE_RF,
    SPECTRUM_REMOTE_P2P,
    SPECTRUM_REMOTE_MQTT,
    SPECTRUM_PATH_COUNT,
} audio_spectrum_path_t;

enum {
    SPECTRUM_MAX_SAMPLES = 5120,
    SPECTRUM_HEADER_BYTES = 44,
    SPECTRUM_REPLY_BYTES = SPECTRUM_HEADER_BYTES + 2 * SPECTRUM_MAX_SAMPLES,
};

void audio_spectrum_init(void);
void audio_spectrum_tap(audio_spectrum_path_t path, const int16_t *pcm, size_t samples);
size_t audio_spectrum_read(uint32_t after, uint32_t generation, bool select, audio_spectrum_path_t path,
                           uint8_t out[SPECTRUM_REPLY_BYTES]);
