/*
 * Board hardware layer -- OpenRadioLink-v0.3 (ESP32-S3-WROOM-1-N16R8)
 *
 * Everything that touches a board net lives here so the rest of the firmware
 * never hardcodes a GPIO.  Pin macros come from the *generated* board_pins.h,
 * which is produced from the measured netlist by tools/gen_pinmap.py.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"

#include "board_pins.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ safety */
/*
 * MUST be the first board call in app_main.
 *
 * The two lines that can hurt a human or the hardware are driven to their safe
 * state immediately, before any driver init that could stall:
 *   AMP_SHDN     -> HIGH  (LM4871 shutdown is ACTIVE HIGH, R14 pulls it up)
 *   SA818_PTT    -> HIGH  (PTT is ACTIVE LOW, R15 pulls it up => receive)
 * Both already sit in the safe state thanks to those pull-ups; this just makes
 * the intent explicit and independent of the pad's reset state.
 *
 * Every controllable pin is configured as INPUT_OUTPUT on purpose: with plain
 * GPIO_MODE_OUTPUT the ESP32 disables the pad's input path, so gpio_get_level()
 * silently returns 0 for a pin that is actually driven high.  That mistake made
 * the first bring-up run report a bogus PTT fault.
 */
esp_err_t board_safe_state_init(void);

/* --------------------------------------------------------------------- LED */
/* LED_STATUS (GPIO12) -> R5 1k -> LED -> GND : ACTIVE HIGH. */
esp_err_t board_led_init(void);
void      board_led_set(bool on);
void      board_led_toggle(void);

/* ------------------------------------------------- LM4871 audio amplifier */
/* ACTIVE-HIGH shutdown: enable == drive AMP_SHDN LOW. */
void board_amp_enable(bool enable);
bool board_amp_is_enabled(void);
/*
 * Operator mute (speaker volume step 0).  While set, the amplifier stays shut
 * down whatever board_amp_enable() asks for; clearing it restores the state the
 * running audio path last requested.
 */
void board_amp_mute_set(bool muted);
bool board_amp_is_muted(void);

typedef struct {
    bool wanted, operator_muted, enabled;
    int shdn_level;
    uint32_t enable_requests, mute_requests, on_edges, off_edges;
    int64_t last_change_us;
} board_amp_diag_t;
/* Software transitions and a single GPIO sample; not an electrical trace. */
void board_amp_diag_get(board_amp_diag_t *out);

/* ------------------------------------------------ SA818 radio control pins */
/*
 * PD (power down) -- polarity MEASURED ON HARDWARE, not assumed.
 *
 * The pin is an ACTIVE-LOW power-down: driving it LOW switches the module's RF
 * section off, HIGH powers it up.
 *
 * This was originally implemented the other way round, and the failure mode is
 * nasty enough to be worth spelling out: with PD held low the module still
 * answers every AT command and ACKs every setting, because only the RF half is
 * off.  The symptom is therefore "configuration all succeeds, RSSI reads fine,
 * PTT reads 0 while keyed, and not a whisper of RF" -- and no amount of
 * firmware inspection finds it.
 */
#define SA818_PD_POWERDOWN_ACTIVE_LOW 1
void board_sa818_power(bool on);

/*
 * RF power level via the module's H/L pin.
 *
 * Open drain: low power grounds H/L, high power releases it with both internal
 * pulls disabled. Requires the GPIO10 connection (old boards grounded H/L).
 * A change while physically transmitting is applied after PTT releases.
 */
void board_sa818_high_power(bool high);
bool board_sa818_high_power_get(void);
bool board_sa818_power_pending(void);
typedef struct {
    bool open_drain, pull_up, pull_down, output;
} board_sa818_power_io_t;
/* Read GPIO configuration registers; does not measure the H/L voltage. */
void board_sa818_power_io(board_sa818_power_io_t *out);
void board_sa818_pd_raw(int level);
int  board_sa818_pd_level(void);

/* PTT: assert(true) == TRANSMIT == pin LOW.  Never call this casually. */
void board_ptt_assert(bool tx);
bool board_ptt_is_asserted(void);

/* --------------------------------------------------------- pin diagnostics */
/* Actual pad levels of the safety-critical outputs (meaningful now that the
 * pads are INPUT_OUTPUT).  Returns true when they match the safe state. */
bool board_safe_state_verify(int *amp_shdn, int *ptt, int *pd);

/* --------------------------------------------------------------------- I2C */
/*
 * SCL = GPIO47, SDA = GPIO48 on the schematic, ES8311 at 7-bit 0x18.
 *
 * Two facts drive the design of this block:
 *   1. The bus has NO external pull-up resistors -- the netlist shows only
 *      I2C_SCL {U2.24, U3.1} and I2C_SDA {U2.25, U3.19}, so it relies purely on
 *      the ESP32's (~45k) internal pull-ups.  That is marginal, so the driver
 *      must enable them and the clock must be allowed to drop.
 *   2. i2c_master_probe() hardcodes 100 kHz, so scanning at other speeds needs
 *      a real device handle with scl_speed_hz set.
 */
#define BOARD_ES8311_I2C_ADDR  0x18

typedef struct {
    int      scl_gpio;
    int      sda_gpio;
    uint32_t freq_hz;
    bool     swapped;      /* SCL/SDA are swapped relative to the net names */
} board_i2c_cfg_t;

esp_err_t board_i2c_open(int scl_gpio, int sda_gpio, uint32_t freq_hz);
esp_err_t board_i2c_close(void);
i2c_master_bus_handle_t board_i2c_bus(void);
void      board_i2c_get_cfg(board_i2c_cfg_t *cfg_out);

/* Probe 0x08..0x77 on the open bus and fill `found`; returns the count. */
int board_i2c_scan(uint8_t *found, int max_found);

/* Attach a device at `addr` to the open bus (freq 0 = the bus default). */
esp_err_t board_i2c_add_device(uint8_t addr, uint32_t freq_hz,
                               i2c_master_dev_handle_t *out);
esp_err_t board_i2c_remove_device(i2c_master_dev_handle_t dev);

/*
 * Look at SCL/SDA with nothing driving them.
 *
 * A healthy idle I2C bus reads 1/1 (the pull-ups win).  A line stuck at 0 means
 * a short to ground, a device clamping the bus, or a device whose supply rail is
 * down -- which is the difference between "wrong software" and "rework the
 * board", so it is worth measuring before blaming the code.
 */
esp_err_t board_i2c_line_levels(int *scl_level, int *sda_level);

/*
 * Try, in order: as drawn @100k, swapped @100k, as drawn @25k, swapped @25k.
 * Keeps the first combination that sees at least one device and reports it.
 * Returns ESP_OK on success, ESP_ERR_NOT_FOUND when the bus stays empty.
 */
esp_err_t board_i2c_autodetect(board_i2c_cfg_t *cfg_out, uint8_t *found,
                               int max_found, int *count_out);

/* --------------------------------------------------------------------- ADC */
/*
 * SA818_AUDIO_IN = GPIO1 = ADC1 channel 0  (radio AF_OUT -- RX audio)
 * VBAT_ADC       = GPIO2 = ADC1 channel 1  (battery divider, mostly unpopulated)
 */
#define BOARD_ADC_CH_RADIO_AF  0
#define BOARD_ADC_CH_VBAT      1

esp_err_t board_adc_init(void);
/* Reads channel (0/1), calibrated millivolts into *mv.  Returns ESP_OK. */
esp_err_t board_adc_read_mv(int channel, int *mv);

/*
 * Sample a channel for `ms` milliseconds (~1 kHz) and summarise it.
 *
 * Used for the radio's AF_OUT, which sits on a DC bias: the useful number is
 * the AC RMS (after removing the mean), not the raw reading.
 */
typedef struct {
    int samples;
    int mean_mv;      /* DC bias                        */
    int ac_rms_mv;    /* RMS of (sample - mean)         */
    int min_mv;
    int max_mv;
    int pp_mv;        /* peak-to-peak                   */
} board_adc_stats_t;

/*
 * ADC ownership.
 *
 * ADC1 on the ESP32-S3 is shared hardware.  The one-shot driver (used for the
 * bring-up measurements and the `w` command) and the continuous DMA driver
 * (used for the monitor audio) both claim it, and switching between them at
 * runtime was observed to leave the continuous driver reading essentially
 * nothing on a channel the one-shot driver measures at 513 mV.
 *
 * Turning the one-shot side off entirely makes the continuous driver the sole
 * owner, which is what the monitor path needs.
 */
void board_adc_oneshot_set_enabled(bool on);
bool board_adc_oneshot_enabled(void);

/* Hand ADC1 to the continuous DMA driver (and back).  See board_hw.c. */
esp_err_t board_adc_dma_begin(void);

/*
 * The BOOT button (SW1) as a user control.
 *
 * It is on GPIO0, which is also a strapping pin: holding it during reset puts
 * the ROM into download mode and the application never runs.  So it can only
 * be a RUNTIME button -- press and hold while the firmware is already up --
 * which is exactly what the factory-reset gesture needs.
 */
void board_boot_button_init(void);
bool board_boot_button_pressed(void);
void      board_adc_dma_end(void);

esp_err_t board_adc_measure(int channel, int ms, board_adc_stats_t *out);

/*
 * Release ADC1 so another driver can own it.
 *
 * The audio monitor path samples the radio's AF_OUT through the ADC *continuous*
 * (DMA) driver, which cannot coexist with the oneshot unit used above, so the
 * streaming path calls this first and board_adc_init() lazily re-creates the
 * oneshot unit afterwards.
 */
esp_err_t board_adc_release(void);

/* ------------------------------------------------------------ charge pins */
/* CHG_EN = GPIO9, CHG_STAT = GPIO3; both only reach the J2 battery header. */
esp_err_t board_charge_pins_init(void);
void      board_charge_enable(bool on);
bool      board_charge_stat(void);

/* -------------------------------------------------------------------- info */
void board_print_banner(void);

#ifdef __cplusplus
}
#endif
