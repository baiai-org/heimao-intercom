#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

enum {
    TX_TIMEOUT_MIN_S = 5,
    TX_TIMEOUT_MAX_S = 600,
    TX_TIMEOUT_DEFAULT_S = 180,
    TX_TIMEOUT_TAIL_MS = 6000,
};

typedef struct {
    int64_t speech_us;
    int64_t release_us;
} tx_timeout_window_t;

unsigned tx_timeout_seconds(void);
void tx_timeout_load(void);
esp_err_t tx_timeout_save(unsigned seconds, unsigned expected);

static inline bool tx_timeout_valid(unsigned seconds)
{
    return seconds >= TX_TIMEOUT_MIN_S && seconds <= TX_TIMEOUT_MAX_S;
}

static inline tx_timeout_window_t tx_timeout_window(int64_t started_us, unsigned seconds)
{
    if (!tx_timeout_valid(seconds)) { seconds = TX_TIMEOUT_DEFAULT_S; }
    const int64_t speech_us = started_us + (int64_t)seconds * 1000000;
    return (tx_timeout_window_t){
        .speech_us = speech_us,
        .release_us = speech_us + (int64_t)TX_TIMEOUT_TAIL_MS * 1000,
    };
}

static inline bool tx_timeout_due(tx_timeout_window_t window, int64_t now_us)
{
    return window.speech_us != 0 && now_us >= window.speech_us;
}
