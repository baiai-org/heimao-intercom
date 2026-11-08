#include "radio_activity.h"
#include <inttypes.h>
#include <stdio.h>
#include "board_hw.h"
#include "device_identity.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "netcom.h"
#include "ptt.h"
#include "radio_led.h"

static char s_boot[9];

void radio_activity_init(void)
{
    if (!s_boot[0]) { snprintf(s_boot, sizeof(s_boot), "%08" PRIx32, esp_random()); }
}

uint32_t radio_activity_json(char *buf, size_t n)
{
    /* Timestamp the start of sampling, so a late HTTP reply cannot replace a
     * newer WS/ACK snapshot. The boot nonce separates ESP timer epochs. */
    const int64_t sample_us = esp_timer_get_time();
    radio_led_state_t leds;
    radio_led_get_state(&leds);
    const int mode = netcom_mode();
    const bool ready = netcom_tx_ready();
    const bool rf_tx = board_ptt_is_asserted();
    const bool rf_rx = !rf_tx && leds.cos_valid && leds.rf_rx;
    const bool net_tx = mode != NETCOM_MODE_LOCAL && ready && leds.net_tx;
    const bool net_rx = mode != NETCOM_MODE_LOCAL && ready && leds.net_rx;
    /* RF can be keyed by a bridge, and logical network TX can have RF off. */
    const bool tx = rf_tx || net_tx || ptt_get_state() == PTT_TX;
    const bool rx = rf_rx || net_rx;
    snprintf(buf, n,
        "{\"type\":\"activity\",\"device_id\":\"%s\",\"boot_id\":\"%s\","
        "\"sample_us\":%" PRId64 ",\"rf_rx\":%s,\"rf_tx\":%s,\"net_rx\":%s,\"net_tx\":%s,"
        "\"tx_active\":%s,\"rx_active\":%s,\"ic_mode\":%d,\"net_ready\":%s}",
        device_identity_id(), s_boot, sample_us,
        rf_rx ? "true" : "false", rf_tx ? "true" : "false",
        net_rx ? "true" : "false", net_tx ? "true" : "false",
        tx ? "true" : "false", rx ? "true" : "false", mode, ready ? "true" : "false");
    return (uint32_t)rf_rx | ((uint32_t)rf_tx << 1) | ((uint32_t)net_rx << 2) |
        ((uint32_t)net_tx << 3) | ((uint32_t)tx << 4) | ((uint32_t)rx << 5) |
        ((uint32_t)mode << 8) | ((uint32_t)ready << 16);
}
