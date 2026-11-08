/*
 * frontpanel.h -- v0.4 PCB front panel: three LEDs and five keys.
 *
 * The second revision of the board adds controls the firmware had no concept
 * of before: a power LED, a receive LED, a transmit LED, a PTT key and four
 * function keys.  frontpanel.c owns all of them -- the LEDs are driven from
 * live state, the keys are polled, debounced and dispatched here.
 *
 * The four function keys are remappable.  Each one holds an action byte in
 * NVS, so any key can carry channel step, volume, squelch, RF power, monitor,
 * mode or the analog bridge.  The defaults match the built v0.4 panel:
 *
 *   index 0  VOL+  GPIO38   -> 音量 +
 *   index 1  VOL-  GPIO39   -> 音量 -
 *   index 2  CH+   GPIO15   -> 频道 +（切换并应用已存频道）
 *   index 3  CH-   GPIO16   -> 频道 -
 *
 * The PTT key is deliberately NOT remappable: it is a level control, not an
 * action, and mis-mapping a transmitter key is not a mistake worth allowing.
 */
#pragma once

#include "radio_led.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Number of remappable function keys (the PTT key is separate). */
#define FP_KEY_COUNT 4

/* Actions a function key can carry.  Keep the string table in frontpanel.c in
 * the same order -- frontpanel_action_name() indexes it by value. */
typedef enum {
    FP_ACT_OFF = 0,     /* 无功能 */
    FP_ACT_CH_UP,       /* 频道 + （应用已存频道） */
    FP_ACT_CH_DN,       /* 频道 - */
    FP_ACT_VOL_UP,      /* 全局音量 + (0..100) */
    FP_ACT_VOL_DN,      /* 全局音量 - */
    FP_ACT_SQL_UP,      /* 静噪 + (0..8) */
    FP_ACT_SQL_DN,      /* 静噪 - */
    FP_ACT_POWER,       /* RF 高/低功率切换 (H/L) */
    FP_ACT_MONITOR,     /* 守听开/关 */
    FP_ACT_MODE,        /* 模式 0 -> 1 -> 2 循环 */
    FP_ACT_BRIDGE,      /* 模拟桥开/关 */
    FP_ACT_COUNT
} fp_action_t;

/* Configure the LEDs and keys and start the poll task.  Safe to call once. */
esp_err_t frontpanel_init(void);

/* Action name ("vol+", "mode", ...) or "?" out of range. */
const char *frontpanel_action_name(uint8_t act);

/* Parse an action name; -1 when unknown.  Used by the HTTP API / panel. */
int frontpanel_action_from_name(const char *name);

/* Key 0..FP_KEY_COUNT-1 as listed above. */
const char *frontpanel_key_label(int key);
uint8_t     frontpanel_key_action(int key);
esp_err_t   frontpanel_key_set_action(int key, uint8_t act);

/* Put the four keys back on their default actions and forget the current
 * channel index.  Called from the factory reset, so that a "restore defaults"
 * really restores everything the operator can change. */
void frontpanel_reset(void);

/* Live slice for /api/status: what the panel needs to draw the block. */
typedef struct {
    uint8_t act[FP_KEY_COUNT];      /* configured action per key        */
    int     level[FP_KEY_COUNT];    /* 0 = pressed, 1 = released        */
    bool    ptt_pressed;
    bool    led_rx;                 /* IO11 as currently driven         */
    bool    led_tx;                 /* IO42 as currently driven         */
    radio_led_state_t leds;         /* source and RF/network policy     */
    int     channel;                /* current preset, 1-based; 0 = none */
    int     channel_count;
} frontpanel_state_t;

void frontpanel_get_state(frontpanel_state_t *out);

#ifdef __cplusplus
}
#endif
