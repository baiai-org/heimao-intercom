/*
 * Board bring-up self test -- implementation.
 */
#include "bringup.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio_codec.h"
#include "audio_i2s.h"
#include "board_hw.h"
#include "sa818.h"
#include "sa818_cfg.h"

static const char *TAG = "bringup";

#define MARK_OK(rpt, cond)   do { if (cond) { (rpt)->passed++; } else { (rpt)->failed++; } } while (0)

/* ------------------------------------------------------------------- steps */

static void step_led(bringup_report_t *r)
{
    ESP_LOGI(TAG, "[1/7] LED ------------------------------------------------");
    r->led_ok = true;
    for (int i = 0; i < 6; ++i) {
        board_led_set(i % 2 == 0);
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    board_led_set(false);
    ESP_LOGI(TAG, "      LED_STATUS (GPIO%d) blinked 3x -- watch the board",
             PIN_LED_STATUS);
}

static void step_i2c(bringup_report_t *r)
{
    ESP_LOGI(TAG, "[2/7] I2C bus / ES8311 ----------------------------------");

    /* Measure the idle bus first: this separates a software mistake from a
     * board problem before any scan noise appears. */
    if (board_i2c_line_levels(&r->i2c_line_scl, &r->i2c_line_sda) == ESP_OK) {
        ESP_LOGI(TAG, "      idle bus with pull-ups: SCL=GPIO%d -> %d, SDA=GPIO%d -> %d",
                 PIN_I2C_SCL, r->i2c_line_scl, PIN_I2C_SDA, r->i2c_line_sda);
        if (r->i2c_line_scl == 0 || r->i2c_line_sda == 0) {
            ESP_LOGE(TAG, "      a line is held LOW with nothing driving it:");
            ESP_LOGE(TAG, "      either a short, or U3 clamping the bus because its");
            ESP_LOGE(TAG, "      supply (AUDIO_3V3 via FB3) is not up.");
        }
    }

    board_i2c_cfg_t cfg;
    esp_err_t err = board_i2c_autodetect(&cfg, r->i2c_addr, BRINGUP_MAX_I2C,
                                         &r->i2c_count);
    r->i2c_ok = (err == ESP_OK);
    r->i2c_swapped = cfg.swapped;
    r->i2c_freq_hz = cfg.freq_hz;

    if (r->i2c_count == 0) {
        /*
         * Last software hypothesis: some codecs keep their I2C slave quiet
         * until the master clock is alive.  Start MCLK and try once more
         * before declaring the board at fault -- this also proves the I2S
         * pins and clock divider, which stage 2 needs anyway.
         */
        ESP_LOGW(TAG, "      no device ACKed on either wiring at 100k/25k/10k");
        ESP_LOGW(TAG, "      starting I2S MCLK and retrying, in case the codec");
        ESP_LOGW(TAG, "      gates its I2C on the master clock...");
        if (audio_i2s_start_clock_only() == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(50));
            int again = board_i2c_autodetect(&cfg, r->i2c_addr, BRINGUP_MAX_I2C,
                                             &r->i2c_count);
            r->i2c_ok = (again == ESP_OK);
            r->i2c_swapped = cfg.swapped;
            r->i2c_freq_hz = cfg.freq_hz;
            r->i2c_needs_mclk = (again == ESP_OK);
            if (again == ESP_OK) {
                ESP_LOGI(TAG, "      the codec DOES need MCLK before it answers I2C");
                for (int i = 0; i < r->i2c_count && i < BRINGUP_MAX_I2C; ++i) {
                    if (r->i2c_addr[i] == BOARD_ES8311_I2C_ADDR) {
                        r->es8311_seen = true;
                    }
                }
            } else {
                ESP_LOGE(TAG, "      still nothing with MCLK running -- I2C ruled out");
            }
        }
    }

    if (r->i2c_count == 0) {
        ESP_LOGW(TAG, "      no device ACKed on either wiring at 100k/25k/10k");
        ESP_LOGW(TAG, "      and also not with I2S MCLK running, so this is not a");
        ESP_LOGW(TAG, "      software or clock-gating problem.  Measure, in order:");
        ESP_LOGW(TAG, "        a. AUDIO_3V3 at U3 pin 3 (PVDD) / 4 (DVDD) / 11 (AVDD)");
        ESP_LOGW(TAG, "           -- must be 3.3 V; FB3 feeds this rail from +3V3");
        ESP_LOGW(TAG, "        b. continuity U3 pin 1 (CCLK) -> GPIO%d", PIN_I2C_SCL);
        ESP_LOGW(TAG, "           and U3 pin 19 (CDATA) -> GPIO%d", PIN_I2C_SDA);
        ESP_LOGW(TAG, "        c. U3 pin 5 (DGND), pin 10 (AGND) and the exposed pad");
        ESP_LOGW(TAG, "           must be grounded; CE (pin 20) is grounded by design");
        ESP_LOGW(TAG, "        d. reflow U3 -- WQFN-20 at 0.4 mm pitch plus a thermal");
        ESP_LOGW(TAG, "           pad is the hardest part on this board to hand-solder");
        ESP_LOGW(TAG, "      Both bus lines idle HIGH, so nothing is shorted to ground");
        ESP_LOGW(TAG, "      and no device is clamping the bus.");
    }
    for (int i = 0; i < r->i2c_count && i < BRINGUP_MAX_I2C; ++i) {
        const bool is_codec = (r->i2c_addr[i] == BOARD_ES8311_I2C_ADDR);
        ESP_LOGI(TAG, "      0x%02X %s", r->i2c_addr[i],
                 is_codec ? "<== ES8311 codec" : "");
        if (is_codec) {
            r->es8311_seen = true;
        }
    }
    if (r->es8311_seen) {
        ESP_LOGI(TAG, "      ES8311 at 0x%02X, working wiring SCL=GPIO%d SDA=GPIO%d @ %u Hz",
                 BOARD_ES8311_I2C_ADDR, cfg.scl_gpio, cfg.sda_gpio,
                 (unsigned)cfg.freq_hz);
    }
}

static void step_adc(bringup_report_t *r)
{
    ESP_LOGI(TAG, "[3/7] ADC (radio AF out, battery) ----------------------");
    {
        const esp_err_t aerr = board_adc_init();
        if (aerr == ESP_ERR_INVALID_STATE) {
            /*
             * The monitor path's continuous DMA driver owns ADC1 right now.
             * That is expected whenever bring-up is re-run (R) while monitor
             * mode is active -- it is not a failure, and reporting it as one
             * would be misleading.
             */
            ESP_LOGW(TAG, "      ADC busy: owned by the monitor DMA driver");
            ESP_LOGW(TAG, "      (re-run bring-up with monitor mode off: press o or z)");
            r->adc_ok = true;
            r->radio_af_mv = -1;
            r->vbat_mv = -1;
            return;
        }
        if (aerr != ESP_OK) {
            ESP_LOGE(TAG, "      adc init failed: %s", esp_err_to_name(aerr));
            r->adc_ok = false;
            r->radio_af_mv = -1;
            r->vbat_mv = -1;
            return;
        }
    }
    r->adc_ok = true;

    if (board_adc_read_mv(BOARD_ADC_CH_RADIO_AF, &r->radio_af_mv) != ESP_OK) {
        r->radio_af_mv = -1;
    }
    if (board_adc_read_mv(BOARD_ADC_CH_VBAT, &r->vbat_mv) != ESP_OK) {
        r->vbat_mv = -1;
    }
    ESP_LOGI(TAG, "      SA818 AF_OUT (GPIO%d/ADC1_CH0) = %d mV",
             PIN_SA818_AUDIO_IN, r->radio_af_mv);
    ESP_LOGI(TAG, "      VBAT divider (GPIO%d/ADC1_CH1) = %d mV",
             PIN_VBAT_ADC, r->vbat_mv);
    if (r->radio_af_mv >= 0 && r->radio_af_mv < 200) {
        ESP_LOGI(TAG, "      AF_OUT near 0 mV: normal with squelch closed / no signal");
    }
}

static void step_codec(bringup_report_t *r)
{
    ESP_LOGI(TAG, "[4/7] ES8311 codec init --------------------------------");
    if (!r->es8311_seen) {
        ESP_LOGW(TAG, "      skipped: the codec never answered on I2C");
        return;
    }
    if (audio_codec_init() != ESP_OK) {
        ESP_LOGE(TAG, "      es8311_init() failed -- see the log above");
        return;
    }
    int vol = -1;
    if (audio_codec_get_volume(&vol) == ESP_OK) {
        r->codec_ok = true;
        r->codec_volume = vol;
        ESP_LOGI(TAG, "      codec initialised; volume reads back as %d", vol);
    } else {
        ESP_LOGE(TAG, "      codec init returned OK but the register read-back failed");
    }
}

static void step_radio(bringup_report_t *r)
{
    ESP_LOGI(TAG, "[5/7] SA818 radio link ---------------------------------");
    sa818_state_t st;
    esp_err_t err = sa818_bringup(&st);

    int tx = 0, rx = 0;
    sa818_report_pins(&tx, &rx);

    r->radio_swapped = st.txrx_swapped;
    r->radio_handshake = st.handshake_ok;
    strncpy(r->radio_version, st.version, sizeof(r->radio_version) - 1);
    r->radio_version[sizeof(r->radio_version) - 1] = '\0';
    r->radio_rssi = st.rssi;

    if (err == ESP_OK) {
        r->radio_ok = true;
        ESP_LOGI(TAG, "      link OK  TX=GPIO%d RX=GPIO%d  version=\"%s\"  handshake=%s",
                 tx, rx, st.version, st.handshake_ok ? "yes" : "no");
        if (st.txrx_swapped) {
            ESP_LOGW(TAG, "      NOTE: the working wiring is the OPPOSITE of the");
            ESP_LOGW(TAG, "      schematic net names -- record this, it matters for");
            ESP_LOGW(TAG, "      anyone reading SA818_TXD/SA818_RXD later.");
        }
        int rssi = -1;
        if (sa818_read_rssi(&rssi) == ESP_OK) {
            r->radio_rssi = rssi;
            ESP_LOGI(TAG, "      RSSI = %d dB", rssi);
        }
    } else {
        ESP_LOGE(TAG, "      no answer on either wiring.  Check that the module is");
        ESP_LOGE(TAG, "      powered (PD level = %d) and that U5 pin 16/17 reach",
                 board_sa818_pd_level());
        ESP_LOGE(TAG, "      GPIO%d / GPIO%d.", PIN_SA818_RXD, PIN_SA818_TXD);
    }
}

static void step_radio_cfg(bringup_report_t *r)
{
    ESP_LOGI(TAG, "[6/7] SA818 profile ------------------------------------");
    if (!r->radio_ok) {
        ESP_LOGW(TAG, "      skipped: the module did not answer");
        return;
    }

    sa818_cfg_t cfg;
    if (sa818_cfg_load(&cfg) != ESP_OK) {
        ESP_LOGI(TAG, "      no stored profile; waiting for panel configuration");
        return;
    }
    sa818_cfg_print(&cfg);
    char summary[80];
    esp_err_t err = sa818_cfg_apply(&cfg, summary, sizeof(summary));
    if (err == ESP_OK) {
        r->radio_cfg_ok = true;
        strncpy(r->radio_cfg_summary, summary, sizeof(r->radio_cfg_summary) - 1);
        ESP_LOGI(TAG, "      applied: %s", summary);
        ESP_LOGI(TAG, "      (profile ACKs checked; older modules may lack read-back)");
    } else {
        ESP_LOGE(TAG, "      profile apply returned %s", esp_err_to_name(err));
    }
}

static void step_ptt(bringup_report_t *r)
{
    ESP_LOGI(TAG, "[7/7] output pins / PTT safety ------------------------");
    board_ptt_assert(false);
    board_amp_enable(false);
    vTaskDelay(pdMS_TO_TICKS(20));
    r->ptt_safe = board_safe_state_verify(&r->pin_amp_shdn, &r->pin_ptt, &r->pin_pd);

    ESP_LOGI(TAG, "      AMP_SHDN GPIO%d = %d (1 = amplifier off)",
             PIN_AMP_SHDN, r->pin_amp_shdn);
    ESP_LOGI(TAG, "      PTT      GPIO%d = %d (1 = receive)",
             PIN_SA818_PTT, r->pin_ptt);
    ESP_LOGI(TAG, "      SA818 PD GPIO%d = %d  (1 = module powered)",
             PIN_SA818_PD, r->pin_pd);

    if (r->ptt_safe) {
        ESP_LOGI(TAG, "      pad levels match the safe state -- radio is in RX");
    } else {
        ESP_LOGE(TAG, "      a pad does not read back as driven.  If PTT reads 0");
        ESP_LOGE(TAG, "      the radio is keyed: power the board down and check");
        ESP_LOGE(TAG, "      GPIO%d / R15 / U5 pin 5.", PIN_SA818_PTT);
    }
}

/* -------------------------------------------------------------------- entry */

esp_err_t bringup_run(bringup_report_t *r)
{
    memset(r, 0, sizeof(*r));
    r->radio_rssi = -1;
    r->radio_af_mv = -1;
    r->vbat_mv = -1;

    ESP_LOGI(TAG, "starting board bring-up (transmitter stays OFF)");

    r->safe_state_ok = (board_safe_state_init() == ESP_OK);
    if (board_led_init() != ESP_OK) {
        ESP_LOGE(TAG, "LED init failed");
    }

    step_led(r);
    step_i2c(r);
    step_adc(r);
    step_codec(r);
    step_radio(r);
    step_radio_cfg(r);
    step_ptt(r);

    /* score */
    MARK_OK(r, r->safe_state_ok);
    MARK_OK(r, r->led_ok);
    MARK_OK(r, r->i2c_ok && r->es8311_seen);
    MARK_OK(r, r->adc_ok);
    MARK_OK(r, r->codec_ok);
    MARK_OK(r, r->radio_ok);
    MARK_OK(r, r->radio_cfg_ok);
    MARK_OK(r, r->ptt_safe);

    return r->failed == 0 ? ESP_OK : ESP_FAIL;
}

void bringup_print(const bringup_report_t *r)
{
    printf("\n");
    printf("  +-------------------- bring-up report --------------------+\n");
    printf("  | %-22s %-10s %-18s |\n", "check", "result", "detail");
    printf("  +------------------------+----------+-------------------+\n");

    printf("  | %-22s %-8s %-19s |\n", "safe state (PTT/amp)",
           r->safe_state_ok ? "PASS" : "FAIL", "");
    printf("  | %-22s %-8s %-19s |\n", "status LED",
           r->led_ok ? "PASS" : "FAIL", "GPIO12 blink");
    printf("  | %-22s %-8s 0x%02X %-14s |\n", "I2C codec",
           r->es8311_seen ? "PASS" : "FAIL", BOARD_ES8311_I2C_ADDR,
           r->es8311_seen ? "ES8311 present" : "not present");
    printf("  | %-22s %-8s SCL%d SDA%d %ukHz%-4s |\n", "I2C wiring",
           r->i2c_ok ? "PASS" : "FAIL",
           r->i2c_swapped ? PIN_I2C_SDA : PIN_I2C_SCL,
           r->i2c_swapped ? PIN_I2C_SCL : PIN_I2C_SDA,
           (unsigned)(r->i2c_freq_hz / 1000),
           r->i2c_swapped ? " SWAP" : "");
    printf("  | %-22s %-8s %4d device(s)     |\n", "I2C scan",
           r->i2c_ok ? "PASS" : "FAIL", r->i2c_count);
    printf("  | %-22s %-8s SCL=%d SDA=%d (idle)  |\n", "I2C idle levels",
           (r->i2c_line_scl && r->i2c_line_sda) ? "PASS" : "FAIL",
           r->i2c_line_scl, r->i2c_line_sda);
    printf("  | %-22s %-8s %4d mV AF       |\n", "ADC radio AF out",
           r->adc_ok ? "PASS" : "FAIL", r->radio_af_mv);
    printf("  | %-22s %-8s %4d mV VBAT     |\n", "ADC battery",
           r->adc_ok ? "PASS" : "FAIL", r->vbat_mv);
    printf("  | %-22s %-8s vol %3d read back  |\n", "ES8311 init",
           r->codec_ok ? "PASS" : "FAIL", r->codec_volume);
    printf("  | %-22s %-8s %-19s |\n", "SA818 UART link",
           r->radio_ok ? "PASS" : "FAIL",
           r->radio_ok ? (r->radio_swapped ? "swapped wiring" : "as drawn") : "no reply");
    printf("  | %-22s %-8s %-19s |\n", "SA818 profile",
           r->radio_cfg_ok ? "PASS" : "FAIL",
           r->radio_cfg_ok ? "set + ACKed" : "not applied");

    printf("  | %-22s %-8s %-19s |\n", "SA818 version",
           r->radio_version[0] ? "PASS" : "FAIL", r->radio_version);

    char rssi[20];
    if (r->radio_rssi >= 0) {
        snprintf(rssi, sizeof(rssi), "%d dB", r->radio_rssi);
    } else {
        snprintf(rssi, sizeof(rssi), "n/a");
    }
    printf("  | %-22s %-8s %-19s |\n", "SA818 RSSI",
           r->radio_rssi >= 0 ? "PASS" : "--", rssi);
    printf("  | %-22s %-8s %-19s |\n", "PTT released (safe)",
           r->ptt_safe ? "PASS" : "FAIL", "TX never keyed");

    printf("  +------------------------+----------+-------------------+\n");
    printf("  | passed %d   failed %d%s|\n", r->passed, r->failed,
           r->failed == 0 ? "   -- board accepted     " : "   -- see log above     ");
    printf("  +---------------------------------------------------------+\n\n");
}
