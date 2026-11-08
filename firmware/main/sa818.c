/*
 * SA818S-U / SA818S-V UART/AT driver -- implementation.
 */
#include "sa818.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "board_hw.h"       /* PIN_SA818_TXD / PIN_SA818_RXD */

static const char *TAG = "sa818";

#define RX_BUF          1024
#define TX_BUF          256
#define REPLY_MAX       256

static sa818_state_t s_st = { .rssi = -1 };
static bool s_driver_installed;
/* Serialises whole command/reply exchanges -- see the note in sa818_cmd(). */
static SemaphoreHandle_t s_uart_mutex;
static bool s_pins_set;
static int  s_tx_gpio = -1;
static int  s_rx_gpio = -1;
static char s_reply[REPLY_MAX];

/* ------------------------------------------------------------------ wiring */

/*
 * Net naming is from the module's side:
 *   net SA818_TXD -> module pin 17 (module transmitter) -> ESP32 RX
 *   net SA818_RXD -> module pin 16 (module receiver)    -> ESP32 TX
 * "swapped" means the opposite reading turned out to be the true one.
 *
 * Measured on this board: the as-drawn reading is correct, so GPIO18 is the
 * ESP32 transmit line and GPIO17 is its receive line.
 */
static void sa818_apply_pins(bool swapped)
{
    const int tx = swapped ? PIN_SA818_TXD : PIN_SA818_RXD;
    const int rx = swapped ? PIN_SA818_RXD : PIN_SA818_TXD;

    if (s_pins_set && tx == s_tx_gpio && rx == s_rx_gpio) {
        return;   /* uart_set_pin() would log a bogus "pin already used" warning */
    }
    s_tx_gpio = tx;
    s_rx_gpio = rx;
    s_pins_set = true;
    uart_set_pin(SA818_UART_PORT, s_tx_gpio, s_rx_gpio,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    ESP_LOGI(TAG, "uart pins: TX=GPIO%d RX=GPIO%d%s",
             s_tx_gpio, s_rx_gpio, swapped ? "  (swapped vs schematic naming)" : "");
}

void sa818_report_pins(int *tx_gpio, int *rx_gpio)
{
    if (tx_gpio) { *tx_gpio = s_tx_gpio; }
    if (rx_gpio) { *rx_gpio = s_rx_gpio; }
}

/* ------------------------------------------------------------------- setup */

static esp_err_t sa818_install_driver(void)
{
    if (s_driver_installed) {
        return ESP_OK;
    }
    const uart_config_t cfg = {
        .baud_rate  = SA818_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(SA818_UART_PORT, RX_BUF, TX_BUF, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install: %s", esp_err_to_name(err));
        return err;
    }
    err = uart_param_config(SA818_UART_PORT, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config: %s", esp_err_to_name(err));
        return err;
    }
    sa818_apply_pins(false);
    if (!s_uart_mutex) {
        s_uart_mutex = xSemaphoreCreateMutex();
    }
    s_driver_installed = true;
    return ESP_OK;
}

/* ------------------------------------------------------------------ helpers */

static void sa818_flush_input(void)
{
    uart_flush_input(SA818_UART_PORT);
}

/*
 * Read until either a terminator arrives or the line goes quiet for
 * `quiet_ms`.  Returns the number of bytes collected.
 */
static int sa818_read_reply(char *out, size_t out_len, int quiet_ms, int total_ms)
{
    size_t used = 0;
    int waited_quiet = 0;
    int waited_total = 0;
    bool got_any = false;

    if (out_len == 0) {
        return 0;
    }
    out[0] = '\0';

    while (waited_total < total_ms) {
        uint8_t chunk[64];
        int n = uart_read_bytes(SA818_UART_PORT, chunk, sizeof(chunk),
                                pdMS_TO_TICKS(10));
        waited_total += 10;
        if (n > 0) {
            got_any = true;
            waited_quiet = 0;
            for (int i = 0; i < n && used + 1 < out_len; ++i) {
                out[used++] = (char)chunk[i];
            }
            out[used] = '\0';
            /* SA818 replies end with CR LF; stop as soon as we see the LF. */
            if (used > 0 && out[used - 1] == '\n') {
                break;
            }
        } else if (got_any) {
            waited_quiet += 10;
            if (waited_quiet >= quiet_ms) {
                break;
            }
        }
    }
    /* trim CR/LF and surrounding spaces in place */
    while (used > 0 && (out[used - 1] == '\r' || out[used - 1] == '\n' ||
                        out[used - 1] == ' ')) {
        out[--used] = '\0';
    }
    return (int)used;
}

esp_err_t sa818_cmd(const char *cmd, char *reply, size_t reply_len, int timeout_ms)
{
    if (!s_driver_installed) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Serialise the whole transaction, not just the write.
     *
     * The command and its reply form one atomic exchange on a half-duplex
     * link.  Two callers exist: the console/self-test, and the RSSI poll that
     * runs from the PTT task every 500 ms.  Without this lock they interleave
     * -- one caller flushes the input just as the other's reply arrives -- and
     * both transactions fail.  The visible symptom was "SA818 profile FAIL not
     * applied": AT+VERSION stopped answering during the bring-up, purely
     * because the RSSI poll was running concurrently.
     */
    if (s_uart_mutex) {
        xSemaphoreTake(s_uart_mutex, portMAX_DELAY);
    }

    sa818_flush_input();

    char line[192];
    int n = snprintf(line, sizeof(line), "%s\r\n", cmd);
    if (n < 0 || n >= (int)sizeof(line)) {
        if (s_uart_mutex) { xSemaphoreGive(s_uart_mutex); }
        return ESP_ERR_INVALID_ARG;
    }
    uart_write_bytes(SA818_UART_PORT, line, (size_t)n);
    uart_wait_tx_done(SA818_UART_PORT, pdMS_TO_TICKS(200));

    char local[REPLY_MAX];
    int got = sa818_read_reply(local, sizeof(local), 60, timeout_ms);
    if (reply && reply_len) {
        strncpy(reply, local, reply_len - 1);
        reply[reply_len - 1] = '\0';
    }

    if (s_uart_mutex) {
        xSemaphoreGive(s_uart_mutex);
    }

    /*
     * Log the exact bytes at INFO level: when a setting silently does not take
     * effect, seeing the literal command string is the fastest way to rule the
     * firmware out.
     */
    ESP_LOGI(TAG, "> %s", cmd);
    ESP_LOGD(TAG, "< %s (%d bytes)", local, got);
    return got > 0 ? ESP_OK : ESP_ERR_TIMEOUT;
}

/* ------------------------------------------------------------------ bringup */

esp_err_t sa818_bringup(sa818_state_t *st)
{
    memset(&s_st, 0, sizeof(s_st));
    s_st.rssi = -1;

    esp_err_t err = sa818_install_driver();
    if (err != ESP_OK) {
        if (st) { *st = s_st; }
        return err;
    }

    /* PTT must be released for the whole probe -- the radio must not key up. */
    board_ptt_assert(false);

    for (int attempt = 0; attempt < 2; ++attempt) {
        const bool swapped = (attempt == 1);
        sa818_apply_pins(swapped);
        vTaskDelay(pdMS_TO_TICKS(50));

        /* AT+VERSION is the cheapest command that always answers. */
        if (sa818_cmd("AT+VERSION", s_reply, sizeof(s_reply), 400) == ESP_OK &&
            s_reply[0] != '\0') {
            s_st.inited = true;
            s_st.txrx_swapped = swapped;
            strncpy(s_st.version, s_reply, sizeof(s_st.version) - 1);
            ESP_LOGI(TAG, "SA818 answered on attempt %d: \"%s\"", attempt + 1, s_reply);

            /* Handshake is informational; a version reply already proves the link. */
            char hs[REPLY_MAX];
            s_st.handshake_ok =
                (sa818_cmd("AT+DMOCONNECT", hs, sizeof(hs), 400) == ESP_OK &&
                 strstr(hs, "DMOCONNECT") != NULL);
            break;
        }
        ESP_LOGW(TAG, "no reply with TX=GPIO%d RX=GPIO%d",
                 s_tx_gpio, s_rx_gpio);
    }

    if (st) { *st = s_st; }
    return s_st.inited ? ESP_OK : ESP_ERR_NOT_FOUND;
}

/* --------------------------------------------------------------- commands */

esp_err_t sa818_set_group(const sa818_profile_t *p, char *reply, size_t reply_len)
{
    if (!p) {
        return ESP_ERR_INVALID_ARG;
    }
    char cmd[128];
    /* NOTE the field order: ...Tx_CTCSS,SQ,Rx_CTCSS -- squelch is 5th, NOT last. */
    snprintf(cmd, sizeof(cmd), "AT+DMOSETGROUP=%d,%.4f,%.4f,%s,%d,%s",
             p->bandwidth, p->tx_mhz, p->rx_mhz,
             p->tx_ctcss[0] ? p->tx_ctcss : "0000",
             p->squelch,
             p->rx_ctcss[0] ? p->rx_ctcss : "0000");
    return sa818_cmd(cmd, reply, reply_len, 600);
}

esp_err_t sa818_set_volume(int vol, char *reply, size_t reply_len)
{
    if (vol < 1) { vol = 1; }
    if (vol > 8) { vol = 8; }
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+DMOSETVOLUME=%d", vol);
    return sa818_cmd(cmd, reply, reply_len, 500);
}

esp_err_t sa818_set_tail(bool on, char *reply, size_t reply_len)
{
    char cmd[24];
    snprintf(cmd, sizeof(cmd), "AT+SETTAIL=%d", on ? 1 : 0);
    return sa818_cmd(cmd, reply, reply_len, 500);
}

esp_err_t sa818_set_filter(int pre_deemph, int highpass, int lowpass,
                           char *reply, size_t reply_len)
{
    char cmd[48];
    snprintf(cmd, sizeof(cmd), "AT+SETFILTER=%d,%d,%d",
             pre_deemph ? 1 : 0, highpass ? 1 : 0, lowpass ? 1 : 0);
    return sa818_cmd(cmd, reply, reply_len, 500);
}

esp_err_t sa818_read_rssi(int *rssi_out)
{
    char reply[REPLY_MAX];
    esp_err_t err = sa818_cmd("RSSI?", reply, sizeof(reply), 400);
    if (err != ESP_OK) {
        return err;
    }
    /*
     * The reply framing varies between module firmware revisions: it may be a
     * bare number or something like "+RSSI=123".  Take the first integer found
     * anywhere rather than insisting on a bare number, otherwise a perfectly
     * good signal reads as an error and gets mistaken for "no reception".
     */
    const char *p = reply;
    while (*p && !isdigit((unsigned char)*p)) {
        ++p;
    }
    if (*p == '\0') {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const int v = atoi(p);
    if (v < 0 || v > 255) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    s_st.rssi = v;
    if (rssi_out) {
        *rssi_out = v;
    }
    return ESP_OK;
}

const sa818_state_t *sa818_state(void) { return &s_st; }
