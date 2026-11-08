#include "radio_led.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

typedef struct {
    bool seen;
    uint8_t mode;
    int64_t last_ms;
} net_event_t;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static net_event_t s_rx_event, s_tx_event;
static radio_led_state_t s_state;
static bool s_cos_candidate;
static int64_t s_cos_since, s_rx_blink_since, s_tx_blink_since;

static void note(net_event_t *event, uint8_t mode, uint32_t *count)
{
    if (mode == 0) { return; }
    const int64_t now = esp_timer_get_time() / 1000;
    portENTER_CRITICAL(&s_mux);
    event->seen = true;
    event->mode = mode;
    event->last_ms = now;
    ++*count;
    portEXIT_CRITICAL(&s_mux);
}

void radio_led_note_net_rx(uint8_t mode)
{
    note(&s_rx_event, mode, &s_state.net_rx_events);
}

void radio_led_note_net_tx(uint8_t mode)
{
    note(&s_tx_event, mode, &s_state.net_tx_events);
}

void radio_led_reset_net(void)
{
    portENTER_CRITICAL(&s_mux);
    s_rx_event.seen = s_tx_event.seen = false;
    s_state.net_rx = s_state.net_tx = false;
    portEXIT_CRITICAL(&s_mux);
}

static bool fresh(const net_event_t *event, uint8_t mode, int64_t now)
{
    return event->seen && event->mode == mode && now >= event->last_ms &&
           now - event->last_ms < RADIO_LED_NET_HOLD_MS;
}

static bool blink(bool active, bool was_active, int64_t *since, int64_t now)
{
    if (!active) { return false; }
    if (!was_active) { *since = now; }
    return ((now - *since) / RADIO_LED_BLINK_MS) % 2 == 0;
}

void radio_led_sample(bool rf_tx, bool cos_valid, bool cos_open,
                      bool network_ready, uint8_t mode, radio_led_state_t *out)
{
    const int64_t now = esp_timer_get_time() / 1000;
    portENTER_CRITICAL(&s_mux);
    s_state.rf_tx = rf_tx;
    s_state.cos_valid = cos_valid;
    s_state.cos_open = cos_valid && cos_open;
    /* The half-duplex RF module cannot receive while keyed. Discard the COS
     * history on TX/invalid input; release needs a fresh, stable COS sample. */
    if (rf_tx || !cos_valid) {
        s_state.rf_rx = s_cos_candidate = false;
        s_cos_since = now;
    } else {
        if (cos_open != s_cos_candidate) {
            s_cos_candidate = cos_open;
            s_cos_since = now;
        }
        const int delay = cos_open ? RADIO_LED_COS_OPEN_MS : RADIO_LED_COS_CLOSE_MS;
        if (now - s_cos_since >= delay) { s_state.rf_rx = cos_open; }
    }
    if (!network_ready || mode == 0) {
        s_rx_event.seen = s_tx_event.seen = false;
    }
    const bool rx = network_ready && mode != 0 && fresh(&s_rx_event, mode, now);
    const bool tx = network_ready && mode != 0 && fresh(&s_tx_event, mode, now);
    const bool rx_flash = blink(rx, s_state.net_rx, &s_rx_blink_since, now);
    const bool tx_flash = blink(tx, s_state.net_tx, &s_tx_blink_since, now);
    s_state.net_rx = rx;
    s_state.net_tx = tx;
    s_state.rx_mode = s_state.rf_rx ? RADIO_LED_RF : rx ? RADIO_LED_NET : RADIO_LED_OFF;
    s_state.tx_mode = rf_tx ? RADIO_LED_RF : tx ? RADIO_LED_NET : RADIO_LED_OFF;
    s_state.rx = s_state.rf_rx || rx_flash;
    s_state.tx = rf_tx || tx_flash;
    *out = s_state;
    portEXIT_CRITICAL(&s_mux);
}

void radio_led_get_state(radio_led_state_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_state;
    portEXIT_CRITICAL(&s_mux);
}

const char *radio_led_mode_name(radio_led_mode_t mode)
{
    return mode == RADIO_LED_RF ? "rf" : mode == RADIO_LED_NET ? "net" : "off";
}
