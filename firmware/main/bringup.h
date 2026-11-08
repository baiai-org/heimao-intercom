/*
 * Board bring-up self test.
 *
 * Runs once at boot and produces a PASS/FAIL table that maps one-to-one onto
 * the accept criteria in the design report, so a fresh board can be signed off
 * from the serial console alone:
 *
 *   LED        -> visible blink on the board
 *   I2C bus    -> ES8311 must answer at 0x18
 *   codec      -> ES8311 must accept its init sequence and read it back
 *   ADC        -> GPIO1 (radio AF_OUT) and GPIO2 (VBAT) must read plausibly
 *   SA818 link -> AT+VERSION must answer on one of the two UART wirings
 *   PTT        -> must stay RELEASED (receive) at all times during bring-up
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BRINGUP_MAX_I2C 16

typedef struct {
    bool    safe_state_ok;
    bool    led_ok;

    bool    i2c_ok;
    int     i2c_count;
    uint8_t i2c_addr[BRINGUP_MAX_I2C];
    bool    es8311_seen;
    bool    i2c_swapped;
    uint32_t i2c_freq_hz;
    int     i2c_line_scl;
    int     i2c_line_sda;
    bool    i2c_needs_mclk;

    bool    adc_ok;
    int     radio_af_mv;
    int     vbat_mv;

    bool    codec_ok;
    int     codec_volume;

    bool    radio_ok;
    bool    radio_swapped;
    bool    radio_handshake;
    char    radio_version[40];
    int     radio_rssi;

    bool    radio_cfg_ok;
    char    radio_cfg_summary[80];

    bool    ptt_safe;
    int     pin_amp_shdn;
    int     pin_ptt;
    int     pin_pd;

    int     passed;
    int     failed;
} bringup_report_t;

/* Execute the whole sequence.  Never keys the transmitter. */
esp_err_t bringup_run(bringup_report_t *rpt);

/* Print the collected result as a table. */
void bringup_print(const bringup_report_t *rpt);

#ifdef __cplusplus
}
#endif
