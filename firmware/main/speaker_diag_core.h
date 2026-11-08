/* Measurement and deterministic test PCM only. Never changes live audio. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t samples, squares, rail_samples;
    uint32_t peak;
} speaker_meter_t;

static inline void speaker_meter_add(speaker_meter_t *m, const int16_t *p, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        const int32_t v = p[i];
        const uint32_t a = v < 0 ? (uint32_t)-v : (uint32_t)v;
        if (a > m->peak) { m->peak = a; }
        m->squares += (uint64_t)((int64_t)v * v);
        m->rail_samples += v == INT16_MIN || v == INT16_MAX;
    }
    m->samples += n;
}

/* 1 kHz at 16 kHz, peak 2048 (-24 dBFS), short endpoint ramps. Test only. */
static inline int16_t speaker_test_tone(size_t i, size_t total)
{
    static const int16_t wave[16] = {
        0, 784, 1448, 1892, 2048, 1892, 1448, 784,
        0, -784, -1448, -1892, -2048, -1892, -1448, -784
    };
    int32_t v = wave[i % 16];
    const size_t fade = 80; /* 5 ms, only on the explicitly generated test tone */
    if (i < fade) { v = v * (int32_t)i / (int32_t)fade; }
    if (total - i < fade) { v = v * (int32_t)(total - i) / (int32_t)fade; }
    return (int16_t)v;
}
