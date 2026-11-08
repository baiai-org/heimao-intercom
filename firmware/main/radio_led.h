/* RF is steady; actual network voice traffic blinks at 5 Hz. No audio gating. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define RADIO_LED_BLINK_MS 100
#define RADIO_LED_NET_HOLD_MS 200
#define RADIO_LED_COS_OPEN_MS 40
#define RADIO_LED_COS_CLOSE_MS 60

typedef enum {
    RADIO_LED_OFF = 0,
    RADIO_LED_RF,
    RADIO_LED_NET,
} radio_led_mode_t;

typedef struct {
    bool rx, tx;
    bool rf_rx, rf_tx, net_rx, net_tx;
    bool cos_valid, cos_open;
    radio_led_mode_t rx_mode, tx_mode;
    uint32_t net_rx_events, net_tx_events;
} radio_led_state_t;

/* Called only after a voice frame was accepted / successfully handed to the
 * transport. Queued PCM, controls, end-only frames and PLC are not events.
 * Mode tags prevent a late event from the other engine lighting this one. */
void radio_led_note_net_rx(uint8_t mode);
void radio_led_note_net_tx(uint8_t mode);
void radio_led_reset_net(void);

/* Front-panel task samples hardware every 20 ms, independently of audio tasks.
 * network_ready is false in LOCAL, while disconnected, or with no engine. */
void radio_led_sample(bool rf_tx, bool cos_valid, bool cos_open,
                      bool network_ready, uint8_t mode, radio_led_state_t *out);
void radio_led_get_state(radio_led_state_t *out);
const char *radio_led_mode_name(radio_led_mode_t mode);
