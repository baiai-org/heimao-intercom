/*
 * Board hardware layer -- implementation.
 *
 * Design rules followed here:
 *   - every controllable net is a single-line function, no magic numbers
 *   - PTT and the amplifier are fail-safe by default and never touched by
 *     anything except their own accessor
 *   - I2C uses the new (v5.x) master driver, not the deprecated legacy one
 */
#include "board_hw.h"
#include "brand.h"
#include "sa818_cfg.h"

#include <stdio.h>
#include <string.h>

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_chip_info.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "hal/gpio_ll.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board";

static i2c_master_bus_handle_t s_i2c_bus;
static int s_i2c_dev_count;
static board_i2c_cfg_t s_i2c_cfg = {
    .scl_gpio = PIN_I2C_SCL,
    .sda_gpio = PIN_I2C_SDA,
    .freq_hz  = 100000,
    .swapped  = false,
};
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_adc_cali;
static bool s_amp_on;
static bool s_amp_want;      /* last state the audio paths asked for */
static portMUX_TYPE s_amp_mux = portMUX_INITIALIZER_UNLOCKED;
static board_amp_diag_t s_amp_diag;
static bool s_ptt_tx;
static bool s_hl_high;
static bool s_hl_requested;
static bool s_hl_configured;
static portMUX_TYPE s_rf_pad_mux = portMUX_INITIALIZER_UNLOCKED;
static esp_err_t hl_init(void);

/* ------------------------------------------------------------------ safety */

esp_err_t board_safe_state_init(void)
{
    const uint64_t mask = (1ULL << PIN_AMP_SHDN) |
                          (1ULL << PIN_SA818_PTT) |
                          (1ULL << PIN_SA818_PD);

    gpio_config_t cfg = {
        .pin_bit_mask = mask,
        /* INPUT_OUTPUT, not OUTPUT: plain OUTPUT disables the pad input path
         * and gpio_get_level() then always reads 0. */
        .mode         = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    /* Preload safe latches before enabling output drivers. */
    gpio_set_level(PIN_AMP_SHDN, 1);
    gpio_set_level(PIN_SA818_PTT, 1);
    gpio_set_level(PIN_SA818_PD, 1);
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    /* amplifier off, radio in receive, radio powered */
    gpio_set_level(PIN_AMP_SHDN, 1);
    gpio_set_level(PIN_SA818_PTT, 1);
    board_sa818_power(true);
    s_amp_on = false;
    s_ptt_tx = false;
    err = hl_init();
    if (err != ESP_OK) { return err; }

    int amp = 0, ptt = 0, pd = 0;
    board_safe_state_verify(&amp, &ptt, &pd);
    ESP_LOGI(TAG, "safe state: AMP_SHDN=%d (off)  PTT=%d (RX)  PD=%d",
             amp, ptt, pd);
    return ESP_OK;
}

bool board_safe_state_verify(int *amp_shdn, int *ptt, int *pd)
{
    const int a = gpio_get_level(PIN_AMP_SHDN);
    const int p = gpio_get_level(PIN_SA818_PTT);
    const int d = gpio_get_level(PIN_SA818_PD);
    if (amp_shdn) { *amp_shdn = a; }
    if (ptt)      { *ptt = p; }
    if (pd)       { *pd = d; }
    /* safe == amp shut down (1), PTT released (1) */
    return (a == 1) && (p == 1);
}

/* --------------------------------------------------------------------- LED */

esp_err_t board_led_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PIN_LED_STATUS,
        .mode         = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err == ESP_OK) {
        gpio_set_level(PIN_LED_STATUS, 0);
    }
    return err;
}

void board_led_set(bool on)     { gpio_set_level(PIN_LED_STATUS, on ? 1 : 0); }
void board_led_toggle(void)     { gpio_set_level(PIN_LED_STATUS, !gpio_get_level(PIN_LED_STATUS)); }

/* --------------------------------------------------------- audio amplifier */

/*
 * The amplifier has two inputs: what the audio paths ask for (board_amp_enable)
 * and the operator's mute (speaker step 0, board_amp_mute_set).  The pin is
 * driven from the AND of the two, so unmuting restores whatever the running
 * audio path last asked for instead of leaving the speaker dead until the next
 * gate transition.
 */
static bool s_amp_muted;

static void amp_apply_locked(void)
{
    const bool on = s_amp_want && !s_amp_muted;
    /* LM4871 SHUTDOWN is active HIGH -> LOW means "running". */
    gpio_set_level(PIN_AMP_SHDN, on ? 0 : 1);
    if (on != s_amp_on) {
        if (on) { ++s_amp_diag.on_edges; } else { ++s_amp_diag.off_edges; }
        s_amp_diag.last_change_us = esp_timer_get_time();
    }
    s_amp_on = on;
}

void board_amp_enable(bool enable)
{
    portENTER_CRITICAL(&s_amp_mux);
    ++s_amp_diag.enable_requests;
    s_amp_want = enable;
    amp_apply_locked();
    portEXIT_CRITICAL(&s_amp_mux);
}

void board_amp_mute_set(bool muted)
{
    portENTER_CRITICAL(&s_amp_mux);
    ++s_amp_diag.mute_requests;
    if (muted != s_amp_muted) {
        s_amp_muted = muted;
        amp_apply_locked();
    }
    portEXIT_CRITICAL(&s_amp_mux);
}

bool board_amp_is_muted(void)
{
    portENTER_CRITICAL(&s_amp_mux);
    const bool muted = s_amp_muted;
    portEXIT_CRITICAL(&s_amp_mux);
    return muted;
}

bool board_amp_is_enabled(void)
{
    portENTER_CRITICAL(&s_amp_mux);
    const bool on = s_amp_on;
    portEXIT_CRITICAL(&s_amp_mux);
    return on;
}

void board_amp_diag_get(board_amp_diag_t *out)
{
    if (!out) { return; }
    portENTER_CRITICAL(&s_amp_mux);
    *out = s_amp_diag;
    out->wanted = s_amp_want;
    out->operator_muted = s_amp_muted;
    out->enabled = s_amp_on;
    out->shdn_level = gpio_get_level(PIN_AMP_SHDN);
    portEXIT_CRITICAL(&s_amp_mux);
}

/* ------------------------------------------------ SA818 radio control pins */

void board_sa818_power(bool on)
{
#if SA818_PD_POWERDOWN_ACTIVE_LOW
    /* Measured on hardware: HIGH powers the module up. */
    gpio_set_level(PIN_SA818_PD, on ? 1 : 0);
#else
    gpio_set_level(PIN_SA818_PD, on ? 0 : 1);
#endif
}

/* --------------------------------------------------- SA818 RF power level */

/* Never source voltage into H/L: zero grounds it, one releases the drain. */
static esp_err_t hl_init(void)
{
    if (s_hl_configured) { return ESP_OK; }
    gpio_set_level(PIN_SA818_HL, 0);
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_SA818_HL,
        .mode = GPIO_MODE_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    const esp_err_t e = gpio_config(&io);
    s_hl_configured = (e == ESP_OK);
    return e;
}

static void hl_apply_locked(void)
{
    if (s_hl_configured && !s_ptt_tx) {
        gpio_set_level(PIN_SA818_HL, s_hl_requested ? 1 : 0);
        s_hl_high = s_hl_requested;
    }
}

void board_sa818_high_power(bool high)
{
    portENTER_CRITICAL(&s_rf_pad_mux);
    s_hl_requested = high;
    hl_apply_locked();
    portEXIT_CRITICAL(&s_rf_pad_mux);
}

bool board_sa818_high_power_get(void)
{
    portENTER_CRITICAL(&s_rf_pad_mux);
    const bool high = s_hl_high;
    portEXIT_CRITICAL(&s_rf_pad_mux);
    return high;
}

bool board_sa818_power_pending(void)
{
    portENTER_CRITICAL(&s_rf_pad_mux);
    const bool pending = s_hl_high != s_hl_requested;
    portEXIT_CRITICAL(&s_rf_pad_mux);
    return pending;
}

void board_sa818_power_io(board_sa818_power_io_t *out)
{
    bool input, sleep;
    uint32_t drive, function, signal;
    gpio_ll_get_io_config(GPIO_LL_GET_HW(0), PIN_SA818_HL,
                         &out->pull_up, &out->pull_down, &input,
                         &out->output, &out->open_drain, &drive,
                         &function, &signal, &sleep);
}

void board_sa818_pd_raw(int level) { gpio_set_level(PIN_SA818_PD, level ? 1 : 0); }
int  board_sa818_pd_level(void)    { return gpio_get_level(PIN_SA818_PD); }

void board_ptt_assert(bool tx)
{
    portENTER_CRITICAL(&s_rf_pad_mux);
    /* Last guard also covers direct console callers and bridge keying. */
    tx = tx && sa818_cfg_tx_ready();
    gpio_set_level(PIN_SA818_PTT, tx ? 0 : 1);
    s_ptt_tx = tx;
    hl_apply_locked();
    portEXIT_CRITICAL(&s_rf_pad_mux);
}

bool board_ptt_is_asserted(void)
{
    portENTER_CRITICAL(&s_rf_pad_mux);
    const bool keyed = s_ptt_tx;
    portEXIT_CRITICAL(&s_rf_pad_mux);
    return keyed;
}

/* --------------------------------------------------------------------- I2C */

/*
 * Deleting a bus that still has devices attached is invalid, and in practice
 * leaves the slave holding SDA low -- which then shows up as a bogus "bus stuck"
 * fault on the next line-level check.  Track attachments so that can't happen.
 */
esp_err_t board_i2c_close(void)
{
    if (!s_i2c_bus) {
        return ESP_OK;
    }
    if (s_i2c_dev_count > 0) {
        ESP_LOGW(TAG, "not deleting the I2C bus: %d device(s) still attached",
                 s_i2c_dev_count);
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = i2c_del_master_bus(s_i2c_bus);
    s_i2c_bus = NULL;
    return err;
}

esp_err_t board_i2c_open(int scl_gpio, int sda_gpio, uint32_t freq_hz)
{
    if (s_i2c_bus && s_i2c_dev_count > 0) {
        /* Pins cannot be re-routed while a device is attached. */
        if (s_i2c_cfg.scl_gpio == scl_gpio && s_i2c_cfg.sda_gpio == sda_gpio) {
            s_i2c_cfg.freq_hz = freq_hz;
            return ESP_OK;
        }
        ESP_LOGW(TAG, "cannot re-route I2C to SCL=GPIO%d SDA=GPIO%d: device attached",
                 scl_gpio, sda_gpio);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_i2c_bus) {
        board_i2c_close();
    }
    i2c_master_bus_config_t cfg = {
        .i2c_port          = I2C_NUM_0,
        .sda_io_num        = sda_gpio,
        .scl_io_num        = scl_gpio,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        /* Mandatory on this board: no external pull-ups are fitted. */
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&cfg, &s_i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus: %s", esp_err_to_name(err));
        s_i2c_bus = NULL;
        return err;
    }
    s_i2c_cfg.scl_gpio = scl_gpio;
    s_i2c_cfg.sda_gpio = sda_gpio;
    s_i2c_cfg.freq_hz  = freq_hz;
    s_i2c_cfg.swapped  = (scl_gpio == PIN_I2C_SDA);
    ESP_LOGI(TAG, "I2C0 up: SCL=GPIO%d SDA=GPIO%d @ %u Hz (internal pull-ups)",
             scl_gpio, sda_gpio, (unsigned)freq_hz);
    return ESP_OK;
}

i2c_master_bus_handle_t board_i2c_bus(void) { return s_i2c_bus; }

void board_i2c_get_cfg(board_i2c_cfg_t *cfg_out)
{
    if (cfg_out) {
        *cfg_out = s_i2c_cfg;
    }
}

/*
 * Presence check via i2c_master_probe(): address + W bit, expect ACK.
 * This is a write-only transaction and is the canonical I2C detection.
 *
 * Do NOT add a read-based probe back here.  A read that gets NACKed leaves
 * the driver's bus-level `contains_read` flag set (it is only cleared when
 * the ISR actually drains the RX FIFO).  The next write-only probe's
 * completion interrupt then takes the ISR's "receive" path and drains the
 * FIFO through the previous transaction's stale ops[].data pointer --
 * observed as a StoreProhibited crash in i2c_ll_read_rxfifo (board #2,
 * 2026-10, triggered by the `i` console key).  The former 1-byte-read
 * fallback also ran at the same 100 kHz as the probe, so it bought no
 * sensitivity on this bus.
 */
static bool i2c_probe_any(i2c_master_bus_handle_t bus, uint8_t addr, uint32_t freq_hz)
{
    (void)freq_hz;
    return i2c_master_probe(bus, addr, 50) == ESP_OK;
}

int board_i2c_scan(uint8_t *found, int max_found)
{
    int n = 0;
    if (!s_i2c_bus) {
        return 0;
    }
    /* An empty-bus scan NACKs every address and floods the log at ERROR level;
     * that is expected behaviour here, not a fault worth printing. */
    esp_log_level_set("i2c.master", ESP_LOG_NONE);
    for (uint8_t addr = 0x08; addr <= 0x77; ++addr) {
        if (i2c_probe_any(s_i2c_bus, addr, s_i2c_cfg.freq_hz)) {
            if (found && n < max_found) {
                found[n] = addr;
            }
            ++n;
        }
    }
    esp_log_level_set("i2c.master", ESP_LOG_ERROR);
    return n;
}

esp_err_t board_i2c_add_device(uint8_t addr, uint32_t freq_hz,
                               i2c_master_dev_handle_t *out)
{
    if (!s_i2c_bus || !out) {
        return ESP_ERR_INVALID_STATE;
    }
    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = freq_hz ? freq_hz : s_i2c_cfg.freq_hz,
    };
    esp_err_t err = i2c_master_bus_add_device(s_i2c_bus, &dc, out);
    if (err == ESP_OK) {
        ++s_i2c_dev_count;
    }
    return err;
}

esp_err_t board_i2c_remove_device(i2c_master_dev_handle_t dev)
{
    if (!dev) {
        return ESP_OK;
    }
    esp_err_t err = i2c_master_bus_rm_device(dev);
    if (err == ESP_OK && s_i2c_dev_count > 0) {
        --s_i2c_dev_count;
    }
    return err;
}

esp_err_t board_i2c_line_levels(int *scl_level, int *sda_level)
{
    if (!s_i2c_bus) {
        /*
         * Nothing owns the pins yet, so set them up as plain inputs with
         * pull-ups just long enough to look at them.
         */
        gpio_config_t cfg = {
            .pin_bit_mask = (1ULL << PIN_I2C_SCL) | (1ULL << PIN_I2C_SDA),
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        esp_err_t err = gpio_config(&cfg);
        if (err != ESP_OK) {
            return err;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    /*
     * With the bus open the pads stay routed to the I2C peripheral, but their
     * input path is enabled, so the real pad level can be read directly -- no
     * need to tear the bus down (which is what used to hang SDA low when a
     * device handle was still attached).
     */
    if (scl_level) { *scl_level = gpio_get_level(PIN_I2C_SCL); }
    if (sda_level) { *sda_level = gpio_get_level(PIN_I2C_SDA); }
    return ESP_OK;
}

esp_err_t board_i2c_autodetect(board_i2c_cfg_t *cfg_out, uint8_t *found,
                               int max_found, int *count_out)
{
    /* As drawn first; then the swapped wiring; then both again at a clock slow
     * enough to tolerate the missing external pull-ups. */
    static const struct { int scl; int sda; uint32_t hz; } tries[] = {
        { PIN_I2C_SCL, PIN_I2C_SDA, 100000 },
        { PIN_I2C_SDA, PIN_I2C_SCL, 100000 },
        { PIN_I2C_SCL, PIN_I2C_SDA,  25000 },
        { PIN_I2C_SDA, PIN_I2C_SCL,  25000 },
        { PIN_I2C_SCL, PIN_I2C_SDA,  10000 },
    };

    /*
     * Once a real device (the codec) is attached, the pins cannot be re-routed
     * any more, so just report what is on the current bus.  The wiring and speed
     * were settled the first time round.
     */
    if (s_i2c_bus && s_i2c_dev_count > 0) {
        int n = board_i2c_scan(found, max_found);
        if (cfg_out) { *cfg_out = s_i2c_cfg; }
        if (count_out) { *count_out = n; }
        return n > 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
    }

    for (size_t i = 0; i < sizeof(tries) / sizeof(tries[0]); ++i) {
        if (board_i2c_open(tries[i].scl, tries[i].sda, tries[i].hz) != ESP_OK) {
            continue;
        }
        int n = board_i2c_scan(found, max_found);
        if (n > 0) {
            if (cfg_out) { *cfg_out = s_i2c_cfg; }
            if (count_out) { *count_out = n; }
            ESP_LOGI(TAG, "I2C found %d device(s) at SCL=GPIO%d SDA=GPIO%d %u Hz%s",
                     n, tries[i].scl, tries[i].sda, (unsigned)tries[i].hz,
                     (tries[i].scl == PIN_I2C_SDA) ? "  (SWAPPED vs net names)" : "");
            return ESP_OK;
        }
        ESP_LOGW(TAG, "I2C silent at SCL=GPIO%d SDA=GPIO%d %u Hz",
                 tries[i].scl, tries[i].sda, (unsigned)tries[i].hz);
    }

    /* Leave the bus on the documented wiring so later code has something. */
    board_i2c_open(PIN_I2C_SCL, PIN_I2C_SDA, 100000);
    if (cfg_out) { *cfg_out = s_i2c_cfg; }
    if (count_out) { *count_out = 0; }
    return ESP_ERR_NOT_FOUND;
}

/* ------------------------------------------------------- BOOT button (SW1) */

/*
 * SW1 shorts GPIO0 to ground; R7 holds it high when released, so this is a
 * plain active-low input with no external work needed.
 */
void board_boot_button_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_BOOT_IO0,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    (void)gpio_config(&io);
}

bool board_boot_button_pressed(void)
{
    return gpio_get_level(PIN_BOOT_IO0) == 0;
}

/* --------------------------------------------------------------------- ADC */

static bool s_adc_oneshot_off;
static bool s_adc_dma_active;

void board_adc_oneshot_set_enabled(bool on) { s_adc_oneshot_off = !on; }
bool board_adc_oneshot_enabled(void) { return !s_adc_oneshot_off; }

/*
 * Hand ADC1 over to the continuous DMA driver, and take it back afterwards.
 * The one-shot unit cannot exist at the same time, so these two calls bracket
 * the monitor path's use of the ADC.
 */
esp_err_t board_adc_dma_begin(void)
{
    s_adc_dma_active = true;
    return board_adc_release();
}

void board_adc_dma_end(void) { s_adc_dma_active = false; }

esp_err_t board_adc_init(void)
{
    if (s_adc_oneshot_off || s_adc_dma_active) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_adc) {
        return ESP_OK;
    }
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id  = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_adc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_new_unit: %s", esp_err_to_name(err));
        return err;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten    = ADC_ATTEN_DB_12,
    };
    for (int ch = 0; ch <= 1; ++ch) {
        err = adc_oneshot_config_channel(s_adc, (adc_channel_t)ch, &chan_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "adc config channel %d: %s", ch, esp_err_to_name(err));
            return err;
        }
    }

    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id  = ADC_UNIT_1,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_adc_cali) != ESP_OK) {
        ESP_LOGW(TAG, "ADC calibration unavailable; raw values only");
        s_adc_cali = NULL;
    }
    return ESP_OK;
}

esp_err_t board_adc_read_mv(int channel, int *mv)
{
    if (!s_adc || !mv) {
        return ESP_ERR_INVALID_STATE;
    }
    int raw = 0;
    esp_err_t err = adc_oneshot_read(s_adc, (adc_channel_t)channel, &raw);
    if (err != ESP_OK) {
        return err;
    }
    if (s_adc_cali) {
        return adc_cali_raw_to_voltage(s_adc_cali, raw, mv);
    }
    *mv = raw;   /* uncalibrated fallback */
    return ESP_OK;
}

esp_err_t board_adc_release(void)
{
    if (s_adc_cali) {
        adc_cali_delete_scheme_curve_fitting(s_adc_cali);
        s_adc_cali = NULL;
    }
    if (s_adc) {
        esp_err_t err = adc_oneshot_del_unit(s_adc);
        s_adc = NULL;
        return err;
    }
    return ESP_OK;
}

esp_err_t board_adc_measure(int channel, int ms, board_adc_stats_t *out)
{
    if (!s_adc || !out) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(out, 0, sizeof(*out));
    out->min_mv = 100000;
    out->max_mv = -100000;

    int64_t sum = 0;
    int n = 0;
    int *store = NULL;

    /* The AC RMS needs the mean first, so keep the samples. */
    const int want = ms > 0 ? ms : 1;
    store = (int *)malloc(sizeof(int) * (size_t)want);
    if (!store) {
        return ESP_ERR_NO_MEM;
    }

    for (int i = 0; i < want; ++i) {
        int mv = 0;
        if (board_adc_read_mv(channel, &mv) == ESP_OK) {
            store[n++] = mv;
            sum += mv;
            if (mv < out->min_mv) { out->min_mv = mv; }
            if (mv > out->max_mv) { out->max_mv = mv; }
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    out->samples = n;
    if (n > 0) {
        out->mean_mv = (int)(sum / n);
        out->pp_mv = out->max_mv - out->min_mv;
        int64_t sumsq = 0;
        for (int i = 0; i < n; ++i) {
            const int d = store[i] - out->mean_mv;
            sumsq += (int64_t)d * d;
        }
        /* integer sqrt */
        int64_t v = sumsq / n;
        int r = 0;
        while ((int64_t)(r + 1) * (r + 1) <= v) { ++r; }
        out->ac_rms_mv = r;
    } else {
        out->min_mv = 0;
        out->max_mv = 0;
    }
    free(store);
    return ESP_OK;
}

/* ------------------------------------------------------------ charge pins */

esp_err_t board_charge_pins_init(void)
{
    gpio_config_t out_cfg = {
        .pin_bit_mask = 1ULL << PIN_CHG_EN,
        .mode         = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&out_cfg);
    if (err != ESP_OK) {
        return err;
    }
    gpio_set_level(PIN_CHG_EN, 0);

    gpio_config_t in_cfg = {
        .pin_bit_mask = 1ULL << PIN_CHG_STAT,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    return gpio_config(&in_cfg);
}

void board_charge_enable(bool on) { gpio_set_level(PIN_CHG_EN, on ? 1 : 0); }
bool board_charge_stat(void)      { return gpio_get_level(PIN_CHG_STAT) != 0; }

/* -------------------------------------------------------------------- info */

void board_print_banner(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    printf("\n");
    printf("  ==========================================================\n");
    printf("   " HM_PROJECT_NAME " · " HM_DEVICE_NAME "   ESP32-S3-WROOM-1-N16R8\n");
    printf("  ==========================================================\n");
    printf("   cores        : %d\n", chip.cores);
    printf("   silicon rev  : v%d.%d\n", chip.revision / 100, chip.revision % 100);
    printf("   features     : %s%s%s%s\n",
           (chip.features & CHIP_FEATURE_WIFI_BGN) ? "WiFi " : "",
           (chip.features & CHIP_FEATURE_BLE) ? "BLE " : "",
           (chip.features & CHIP_FEATURE_EMB_FLASH) ? "emb-flash " : "",
           (chip.features & CHIP_FEATURE_EMB_PSRAM) ? "emb-psram" : "");
    printf("   flash        : %s   (partition table: custom, OTA)\n",
           CONFIG_ESPTOOLPY_FLASHSIZE);
#if CONFIG_SPIRAM
    printf("   psram        : %u bytes (octal)\n", (unsigned)esp_psram_get_size());
#else
    printf("   psram        : disabled in sdkconfig\n");
#endif
    printf("   MAC (STA)    : %02X:%02X:%02X:%02X:%02X:%02X\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    const char *rr = "?";
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:  rr = "power-on";            break;
        case ESP_RST_EXT:      rr = "external pin";        break;
        case ESP_RST_SW:       rr = "software";            break;
        case ESP_RST_PANIC:    rr = "PANIC (crash)";       break;
        case ESP_RST_INT_WDT:  rr = "interrupt WDT";       break;
        case ESP_RST_TASK_WDT: rr = "task WDT";            break;
        case ESP_RST_WDT:      rr = "other WDT";           break;
        case ESP_RST_BROWNOUT: rr = "BROWNOUT";            break;
        case ESP_RST_DEEPSLEEP:rr = "deep sleep";          break;
        case ESP_RST_USB:      rr = "USB peripheral";      break;
        case ESP_RST_JTAG:     rr = "JTAG";                break;
        default:               rr = "unknown";             break;
    }
    printf("   reset reason : %s\n", rr);
    printf("  ==========================================================\n\n");
}
