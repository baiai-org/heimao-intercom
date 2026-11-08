/*
 * SA818S-U / SA818S-V radio module driver -- UART/AT control.
 *
 * Facts taken from the vendor command set (project report appendix A):
 *   9600 8N1, TTL 3.3 V, every command is "AT..." terminated by CR LF,
 *   and the module must ACK before the next command is sent.
 *
 * The one thing the schematic cannot tell us is which way round the UART is
 * wired: the nets are named from the MODULE's point of view
 * (SA818_TXD = module pin 17 = the module's transmitter), which makes
 * GPIO17 the ESP32 *receive* line -- the same naming convention that makes
 * I2S_DIN an output on this board.  Rather than bet the bring-up on that
 * reading, sa818_bringup() probes both wirings and reports which one answers.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SA818_BAUD          9600
#define SA818_UART_PORT     UART_NUM_1

typedef struct {
    int   bandwidth;      /* 0 = 12.5 kHz narrow, 1 = 25 kHz wide   */
    float tx_mhz;         /* transmit frequency                      */
    float rx_mhz;         /* receive frequency (may differ -> repeater) */
    char  tx_ctcss[8];    /* "0000" = off, NS code, or DCS like "754N" */
    int   squelch;        /* 0..8, 0 = monitor                        */
    char  rx_ctcss[8];
} sa818_profile_t;

typedef struct {
    bool inited;
    bool txrx_swapped;    /* true => working wiring is TX=GPIO17, RX=GPIO18 */
    bool handshake_ok;
    char version[40];     /* e.g. "V4.2" */
    int  rssi;            /* last read, -1 when unknown */
} sa818_state_t;

/* Probe both UART wirings, handshake and read the firmware version.
 * Never transmits: PTT stays released throughout. */
esp_err_t sa818_bringup(sa818_state_t *st);

/* Send one AT command and collect the reply.  `cmd` is written verbatim;
 * CR LF is appended automatically.  Returns ESP_OK when bytes came back. */
esp_err_t sa818_cmd(const char *cmd, char *reply, size_t reply_len, int timeout_ms);

esp_err_t sa818_set_group(const sa818_profile_t *p, char *reply, size_t reply_len);
esp_err_t sa818_set_volume(int vol, char *reply, size_t reply_len);
esp_err_t sa818_set_tail(bool on, char *reply, size_t reply_len);
esp_err_t sa818_set_filter(int pre_deemph, int highpass, int lowpass,
                           char *reply, size_t reply_len);
esp_err_t sa818_read_rssi(int *rssi_out);

/* Current state (never NULL after sa818_bringup). */
const sa818_state_t *sa818_state(void);

/* Which GPIOs ended up as TX/RX, for the bring-up report. */
void sa818_report_pins(int *tx_gpio, int *rx_gpio);

#ifdef __cplusplus
}
#endif
