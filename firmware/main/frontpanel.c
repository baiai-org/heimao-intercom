/*
 * frontpanel.c -- v0.4 PCB front panel: three LEDs, five keys.
 *
 * WHAT IT OWNS
 *
 *   IO12  power LED    -- one short blink per second, plus the SW1 reset
 *                         counter's fast blink (that part lives in main.c's
 *                         heartbeat_task, which owned this LED before the
 *                         second board existed and still does)
 *   IO11  RX LED -- RF COS steady, actual network voice reception 5 Hz
 *   IO42  TX LED -- applied RF PTT steady, actual network voice sending 5 Hz
 *   IO41  PTT key      -- level control: hold to talk, release to end
 *   IO39/38/15/16      -- four remappable function keys
 *
 * WHY POLLING
 *
 * The keys are polled at 20 ms and accepted after three identical samples
 * (60 ms).  An interrupt-per-key design would need the GPIO ISR service, a
 * queue and a place to run the resulting work, and every one of those is a
 * new way for a press to be lost during a flash write or a long HTTP request.
 * At 20 ms a hand press is not going anywhere and debounce is three lines.
 * A separate small LED task keeps its cadence during blocking key actions
 * (channel changes, audio start/stop and NVS writes).
 *
 * RF has priority on each LED. COS is sampled here, independently of monitor
 * audio, so mute/stopping playback cannot freeze an old receive indication.
 * Network engines report accepted/sent voice frames, never cumulative stats.
 */
#include "frontpanel.h"

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "driver/gpio.h"
#include "nvs.h"

#include "board_pins.h"
#include "board_hw.h"
#include "bridge.h"
#include "channels.h"
#include "netcom.h"
#include "net_wifi.h"
#include "ptt.h"
#include "sa818_cfg.h"
#include "voice.h"

static const char *TAG = "frontpanel";

#define NVS_NS       "orl"
#define KEY_ACT      "fp_act"     /* blob, FP_KEY_COUNT bytes */
#define KEY_CHAN     "fp_chan"    /* u8, 1-based current preset */

#define FP_TICK_MS        20
#define FP_DEBOUNCE       3       /* samples; 3 x 20 ms = 60 ms */
#define FP_TASK_STACK     3584
#define FP_LED_STACK      2304

/*
 * Physical order matches the header's key index documentation:
 *
 *   index 0  VOL+  IO38      index 1  VOL-  IO39
 *   index 2  CH+   IO15      index 3  CH-   IO16
 *
 * The volume pair is the way round the built v0.4 panel behaves (pressing the
 * IO39 button lowers the volume); the aliases in board_pins.h carry that, so
 * the labels, the defaults and the pins all agree.
 */
static const gpio_num_t  s_key_pin[FP_KEY_COUNT] = {
    PIN_KEY_VOL_UP, PIN_KEY_VOL_DN, PIN_KEY_CH_UP, PIN_KEY_CH_DN,
};
static const char *const s_key_label[FP_KEY_COUNT] = { "vol+", "vol-", "ch+", "ch-" };
static const uint8_t     s_key_default[FP_KEY_COUNT] = {
    FP_ACT_VOL_UP, FP_ACT_VOL_DN, FP_ACT_CH_UP, FP_ACT_CH_DN,
};

/* Indexed by fp_action_t -- keep in step with the enum in frontpanel.h. */
static const char *const s_act_name[FP_ACT_COUNT] = {
    "off", "ch+", "ch-", "vol+", "vol-", "sql+", "sql-",
    "power", "monitor", "mode", "bridge",
};

static uint8_t s_act[FP_KEY_COUNT];
static bool    s_stable[FP_KEY_COUNT];      /* debounced state: true = pressed */
static uint8_t s_count[FP_KEY_COUNT];
static bool    s_ptt_stable;
static uint8_t s_ptt_count;
static bool    s_ptt_owned;                 /* true while THIS key holds TALK */
static uint32_t s_ptt_token;
static int     s_chan = 1;                  /* 1-based preset, 0 = unknown */
static bool    s_led_rx;
static bool    s_led_tx;

/* --------------------------------------------------------------- settings */

static void settings_load(void)
{
    memcpy(s_act, s_key_default, sizeof(s_act));

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_act);
        if (nvs_get_blob(h, KEY_ACT, s_act, &len) != ESP_OK || len != sizeof(s_act)) {
            memcpy(s_act, s_key_default, sizeof(s_act));
        }
        uint8_t ch = 0;
        if (nvs_get_u8(h, KEY_CHAN, &ch) == ESP_OK && ch >= 1) {
            s_chan = ch;
        }
        nvs_close(h);
    }
    /* An action byte from a newer firmware (or a corrupt blob) must never be
     * used as an index into the name table. */
    for (int i = 0; i < FP_KEY_COUNT; ++i) {
        if (s_act[i] >= FP_ACT_COUNT) {
            s_act[i] = s_key_default[i];
        }
    }
}

static void settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    (void)nvs_set_blob(h, KEY_ACT, s_act, sizeof(s_act));
    (void)nvs_set_u8(h, KEY_CHAN, (uint8_t)(s_chan < 1 ? 1 : s_chan));
    (void)nvs_commit(h);
    nvs_close(h);
}

/* ---------------------------------------------------------------- actions */

static void act_channel_step(int delta)
{
    const int n = channels_count();
    if (n <= 0) {
        ESP_LOGW(TAG, "channel %+d: no saved channels", delta);
        return;
    }
    int idx = (s_chan >= 1 && s_chan <= n) ? s_chan + delta : 1;
    if (idx < 1) {
        idx = n;
    }
    if (idx > n) {
        idx = 1;
    }

    sa818_cfg_t cfg;
    bool found = false;
    for (int tried = 0; tried < n; ++tried) {
        if (channels_get(idx - 1, NULL, 0, &cfg) && sa818_cfg_compatible(&cfg)) {
            found = true;
            break;
        }
        idx = ((idx - 1 + delta + n) % n) + 1;
    }
    if (!found) {
        ESP_LOGW(TAG, "no compatible channel for %s", sa818_band_name(sa818_cfg_band()));
        return;
    }
    char log[192] = {0};
    const esp_err_t err = sa818_cfg_update(&cfg, log, sizeof(log));
    bridge_apply();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "channel %d/%d apply failed: %s", idx, n, log);
        return;
    }
    s_chan = idx;
    settings_save();
    ESP_LOGI(TAG, "channel -> %d/%d  %s", idx, n, log);
}

/*
 * Volume keys move the five-step SPEAKER volume (voice_set_speaker_level):
 * 0 = amplifier shut down, 1..4 = -10 / -5 / 0 / +5 dB on the speaker path.
 *
 * Not the codec's DAC volume and not the receive gain: the DAC output on this
 * board also feeds the SA818's transmit input, and the receive gain only covers
 * the radio's own audio (network audio bypasses it) -- see the note in voice.h.
 */
static void act_volume_step(int delta)
{
    const int cur = voice_get_speaker_level();
    int v = cur + delta;
    if (v < VOICE_SPK_MIN) { v = VOICE_SPK_MIN; }
    if (v > VOICE_SPK_MAX) { v = VOICE_SPK_MAX; }
    if (v == cur) {
        ESP_LOGI(TAG, "speaker volume already at step %d/%d", v, VOICE_SPK_MAX);
        return;
    }
    voice_set_speaker_level(v);
    ESP_LOGI(TAG, "speaker volume %d -> %d/%d%s", cur, v, VOICE_SPK_MAX,
             v == 0 ? " (amplifier off)" : "");
}

static void act_squelch_step(int delta)
{
    sa818_cfg_t cfg;
    sa818_cfg_load(&cfg);
    int v = cfg.squelch + delta;
    if (v < 0) { v = 0; }
    if (v > 8) { v = 8; }
    if (v == cfg.squelch) {
        ESP_LOGI(TAG, "squelch already at %d/8", v);
        return;
    }
    cfg.squelch = v;
    char log[160] = {0};
    if (sa818_cfg_update(&cfg, log, sizeof(log)) == ESP_OK) {
        ESP_LOGI(TAG, "squelch -> %d/8%s", v, v == 0 ? " (open)" : "");
    } else {
        ESP_LOGW(TAG, "squelch %d failed: %s", v, log);
    }
    bridge_apply();
}

static void act_power_toggle(void)
{
    sa818_cfg_t cfg;
    sa818_cfg_load(&cfg);
    char log[160] = {0};
    if (sa818_cfg_set_power(!cfg.high_power, log, sizeof(log)) == ESP_OK) {
        ESP_LOGI(TAG, "%s", log);
    } else {
        ESP_LOGW(TAG, "RF power switch failed: %s", log);
    }
}

static void act_monitor_toggle(void)
{
    if (ptt_is_transmitting()) {
        ESP_LOGW(TAG, "monitor: refused, transmitter is keyed");
        return;
    }
    voice_monitor_operator_takeover();
    if (voice_mode() == VOICE_MONITOR) {
        (void)voice_stop();
        ESP_LOGI(TAG, "monitor -> off");
    } else {
        const esp_err_t err = voice_start(VOICE_MONITOR);
        ESP_LOGI(TAG, "monitor -> %s", err == ESP_OK ? "on" : esp_err_to_name(err));
    }
}

static void act_mode_next(void)
{
    /* local -> net-p2p -> relay -> local */
    const uint8_t next = (uint8_t)((netcom_mode() + 1) % (NETCOM_MODE_MAX + 1));
    const esp_err_t err = netcom_set_mode(next);
    ESP_LOGI(TAG, "mode -> %u (%s) %s", next, netcom_mode_name(next),
             err == ESP_OK ? "ok" : esp_err_to_name(err));
}

static void act_bridge_toggle(void)
{
    if (bridge_en()) {
        (void)bridge_set_en(false);
        ESP_LOGI(TAG, "analog bridge -> off");
        return;
    }
    int chan = bridge_chan();
    if (chan == 0 && channels_count() > 0) {
        chan = (s_chan >= 1) ? s_chan : 1;
        (void)bridge_set_chan(chan);
    }
    const esp_err_t err = bridge_set_en(true);
    ESP_LOGI(TAG, "analog bridge -> on (channel %d) %s", bridge_chan(),
             err == ESP_OK ? "ok" : esp_err_to_name(err));
}

static void act_run(uint8_t act)
{
    switch (act) {
    case FP_ACT_CH_UP:   act_channel_step(+1);  break;
    case FP_ACT_CH_DN:   act_channel_step(-1);  break;
    case FP_ACT_VOL_UP:  act_volume_step(+1);   break;
    case FP_ACT_VOL_DN:  act_volume_step(-1);   break;
    case FP_ACT_SQL_UP:  act_squelch_step(+1);  break;
    case FP_ACT_SQL_DN:  act_squelch_step(-1);  break;
    case FP_ACT_POWER:   act_power_toggle();    break;
    case FP_ACT_MONITOR: act_monitor_toggle();  break;
    case FP_ACT_MODE:    act_mode_next();       break;
    case FP_ACT_BRIDGE:  act_bridge_toggle();   break;
    case FP_ACT_OFF:
    default:
        ESP_LOGI(TAG, "key action is 'off' -- nothing to do");
        break;
    }
}

/* -------------------------------------------------------------------- PTT */

/*
 * The same path the panel's PTT button takes (see h_talk in net_http.c): stop
 * MONITOR, wait out the PTT state machine's 50 ms tail hold, then start TALK.
 * Going through voice_start() rather than driving the PTT pin keeps the
 * deadline and the safe-state logic in one place.
 */
static void ptt_key_press(void)
{
    /* All callers share voice_start admission; no pre-emptive stop. */
    const esp_err_t err = voice_start(VOICE_TALK);
    if (err == ESP_OK) {
        s_ptt_owned = true;
        s_ptt_token = voice_talk_token();
        ESP_LOGI(TAG, "PTT key: TALK started (release to end)");
    } else {
        ESP_LOGW(TAG, "PTT key: TALK refused: %s", esp_err_to_name(err));
    }
}

static void ptt_key_release(void)
{
    if (!s_ptt_owned) {
        return;     /* the panel or a network engine owns this transmission */
    }
    s_ptt_owned = false;
    /* Flag, not unkey: the talk task sends the end-of-transmission tone with
     * the transmitter still keyed and releases the PTT itself. */
    (void)voice_request_talk_end_for(s_ptt_token);
    ESP_LOGI(TAG, "PTT key released (tail + PTT release pending)");
}

/* ------------------------------------------------------------------- task */

static void frontpanel_task(void *arg)
{
    (void)arg;

    for (;;) {
        for (int i = 0; i < FP_KEY_COUNT; ++i) {
            const bool pressed = gpio_get_level(s_key_pin[i]) == 0;
            if (pressed == s_stable[i]) {
                s_count[i] = 0;
                continue;
            }
            if (++s_count[i] < FP_DEBOUNCE) {
                continue;
            }
            s_count[i] = 0;
            s_stable[i] = pressed;
            if (pressed) {
                ESP_LOGI(TAG, "key %s (GPIO%d) -> %s", s_key_label[i],
                         (int)s_key_pin[i], frontpanel_action_name(s_act[i]));
                act_run(s_act[i]);
            }
        }

        {
            const bool pressed = gpio_get_level(PIN_KEY_PTT) == 0;
            if (pressed != s_ptt_stable) {
                if (++s_ptt_count >= FP_DEBOUNCE) {
                    s_ptt_count = 0;
                    s_ptt_stable = pressed;
                    if (pressed) {
                        ptt_key_press();
                    } else {
                        ptt_key_release();
                    }
                }
            } else {
                s_ptt_count = 0;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(FP_TICK_MS));
    }
}

static void frontpanel_led_task(void *arg)
{
    (void)arg;
    TickType_t tick = xTaskGetTickCount();
    for (;;) {
        const int cos_gpio = voice_get_sq_gpio();
        const bool cos_valid = GPIO_IS_VALID_GPIO(cos_gpio) &&
            sa818_cfg_tx_ready() &&
            board_sa818_pd_level() == (SA818_PD_POWERDOWN_ACTIVE_LOW ? 1 : 0);
        const bool cos_open = cos_valid && gpio_get_level((gpio_num_t)cos_gpio) == 0;
        const uint8_t mode = netcom_mode();
        const bool net_ready = mode != NETCOM_MODE_LOCAL &&
            netcom_engine_running() && net_wifi_connected() && netcom_tx_ready();
        radio_led_state_t leds;
        radio_led_sample(board_ptt_is_asserted(), cos_valid, cos_open,
                         net_ready, mode, &leds);
        const bool rx = leds.rx;
        const bool tx = leds.tx;

        if (rx != s_led_rx) {
            s_led_rx = rx;
            gpio_set_level(PIN_LED_RX, rx ? 1 : 0);
        }
        if (tx != s_led_tx) {
            s_led_tx = tx;
            gpio_set_level(PIN_LED_TX, tx ? 1 : 0);
        }

        xTaskDelayUntil(&tick, pdMS_TO_TICKS(FP_TICK_MS));
    }
}

/* --------------------------------------------------------------- public API */

const char *frontpanel_action_name(uint8_t act)
{
    return (act < FP_ACT_COUNT) ? s_act_name[act] : "?";
}

int frontpanel_action_from_name(const char *name)
{
    if (!name || !name[0]) {
        return -1;
    }
    for (int i = 0; i < FP_ACT_COUNT; ++i) {
        if (!strcmp(name, s_act_name[i])) {
            return i;
        }
    }
    return -1;
}

const char *frontpanel_key_label(int key)
{
    return (key >= 0 && key < FP_KEY_COUNT) ? s_key_label[key] : "?";
}

uint8_t frontpanel_key_action(int key)
{
    return (key >= 0 && key < FP_KEY_COUNT) ? s_act[key] : FP_ACT_OFF;
}

esp_err_t frontpanel_key_set_action(int key, uint8_t act)
{
    if (key < 0 || key >= FP_KEY_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (act >= FP_ACT_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_act[key] == act) {
        return ESP_OK;
    }
    s_act[key] = act;
    settings_save();
    ESP_LOGI(TAG, "key %s -> %s", s_key_label[key], s_act_name[act]);
    return ESP_OK;
}

void frontpanel_get_state(frontpanel_state_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < FP_KEY_COUNT; ++i) {
        out->act[i]   = s_act[i];
        out->level[i] = s_stable[i] ? 0 : 1;
    }
    out->ptt_pressed   = s_ptt_stable;
    radio_led_get_state(&out->leds);
    out->led_rx        = out->leds.rx;
    out->led_tx        = out->leds.tx;
    out->channel       = s_chan;
    out->channel_count = channels_count();
}

void frontpanel_reset(void)
{
    memcpy(s_act, s_key_default, sizeof(s_act));
    s_chan = 1;
    settings_save();
    ESP_LOGW(TAG, "front panel reset: keys back to vol+/vol-/ch+/ch-");
}

esp_err_t frontpanel_init(void)
{
    /* The power LED (PIN_LED_STATUS == IO12) is configured and driven by
     * board_led_init()/heartbeat_task() in main.c and is deliberately left
     * alone here. */
    const gpio_config_t leds = {
        .pin_bit_mask = (1ULL << PIN_LED_RX) | (1ULL << PIN_LED_TX),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&leds);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LED config failed: %s", esp_err_to_name(err));
        return err;
    }
    gpio_set_level(PIN_LED_RX, 0);
    gpio_set_level(PIN_LED_TX, 0);

    /* Keys: switch to GND, so the internal pull-up is what idles them high. */
    const gpio_config_t keys = {
        .pin_bit_mask = (1ULL << PIN_KEY_PTT) | (1ULL << PIN_KEY_VOL_UP) |
                        (1ULL << PIN_KEY_VOL_DN) | (1ULL << PIN_KEY_CH_UP) |
                        (1ULL << PIN_KEY_CH_DN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&keys);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "key config failed: %s", esp_err_to_name(err));
        return err;
    }

    settings_load();

    /* Seed the debounce state with what the pins read now: a key that is
     * already down at boot (or a floating one reading low) must not be
     * delivered as a press the moment the task starts. */
    for (int i = 0; i < FP_KEY_COUNT; ++i) {
        s_stable[i] = gpio_get_level(s_key_pin[i]) == 0;
        s_count[i]  = 0;
    }
    s_ptt_stable = gpio_get_level(PIN_KEY_PTT) == 0;
    s_ptt_count  = 0;

    TaskHandle_t led_task = NULL;
    if (xTaskCreate(frontpanel_led_task, "panel_led", FP_LED_STACK, NULL, 4, &led_task) != pdPASS) {
        ESP_LOGE(TAG, "LED task create failed");
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(frontpanel_task, "frontpanel", FP_TASK_STACK, NULL, 4, NULL) != pdPASS) {
        vTaskDelete(led_task);
        gpio_set_level(PIN_LED_RX, 0);
        gpio_set_level(PIN_LED_TX, 0);
        ESP_LOGE(TAG, "frontpanel task create failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "v0.4 front panel ready: RX LED GPIO%d, TX LED GPIO%d, "
                  "PTT key GPIO%d, keys %d/%d/%d/%d",
             (int)PIN_LED_RX, (int)PIN_LED_TX, (int)PIN_KEY_PTT,
             (int)PIN_KEY_VOL_UP, (int)PIN_KEY_VOL_DN,
             (int)PIN_KEY_CH_UP, (int)PIN_KEY_CH_DN);
    ESP_LOGI(TAG, "key actions: %s=%s %s=%s %s=%s %s=%s",
             s_key_label[0], s_act_name[s_act[0]],
             s_key_label[1], s_act_name[s_act[1]],
             s_key_label[2], s_act_name[s_act[2]],
             s_key_label[3], s_act_name[s_act[3]]);
    return ESP_OK;
}
