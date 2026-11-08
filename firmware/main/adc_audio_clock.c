/*
 * Project-local ESP32-S3 ADC clock adaptation. The linker wraps the HAL call
 * made by adc_continuous_start(), after it takes the ADC lock and before it
 * enables conversion. The shared ESP-IDF sources stay untouched.
 *
 * IDF 5.4 requests 32 kHz with a 5 MHz controller clock and an integer trigger
 * interval of 78: 5000000 / (2 * 78) = 32051.282 Hz. A fixed 2:1 decimator
 * feeding 16 kHz I2S therefore fills the capture pool and loses a frame about
 * every ten seconds. Use an integer controller divider of 25 and interval 50:
 * 80000000 / 25 / (2 * 50) = 32000 Hz. No PCM samples are changed here.
 *
 * Keep the SDK's fractional fields at their default disabled values. Its LL
 * and register comments disagree about A/B; integer division needs neither.
 */
#include "adc_audio_clock.h"

#include "sdkconfig.h"
#include "hal/adc_hal.h"
#include "soc/apb_saradc_struct.h"

#if !CONFIG_IDF_TARGET_ESP32S3
#error "The radio ADC clock adaptation is specific to ESP32-S3"
#endif

#define AUDIO_ADC_SOURCE_HZ 80000000U
#define AUDIO_ADC_DIVISOR   25U
#define AUDIO_ADC_INTERVAL  50U

_Static_assert(AUDIO_ADC_SOURCE_HZ ==
               ADC_AUDIO_RATE_HZ * AUDIO_ADC_DIVISOR * 2U * AUDIO_ADC_INTERVAL,
               "ADC audio clock must divide exactly");

static uint32_t s_source_hz;
static bool s_applied;

void __real_adc_hal_digi_controller_config(adc_hal_dma_ctx_t *hal,
                                          const adc_hal_digi_ctrlr_cfg_t *cfg);

void __wrap_adc_hal_digi_controller_config(adc_hal_dma_ctx_t *hal,
                                          const adc_hal_digi_ctrlr_cfg_t *cfg)
{
    __real_adc_hal_digi_controller_config(hal, cfg);
    s_source_hz = cfg->clk_src_freq_hz;
    s_applied = false;
    if (cfg->clk_src != ADC_DIGI_CLK_SRC_APB ||
        cfg->clk_src_freq_hz != AUDIO_ADC_SOURCE_HZ ||
        cfg->sample_freq_hz != ADC_AUDIO_RATE_HZ ||
        cfg->conv_mode != ADC_CONV_SINGLE_UNIT_1 ||
        cfg->adc_pattern_len != 1) {
        return;
    }
    /* S3 LL represents an integer divider D as clkm_div_num = D - 1. */
    adc_ll_digi_controller_clk_div(AUDIO_ADC_DIVISOR - 1U,
                                  ADC_LL_CLKM_DIV_B_DEFAULT,
                                  ADC_LL_CLKM_DIV_A_DEFAULT);
    adc_ll_digi_set_trigger_interval(AUDIO_ADC_INTERVAL);
    s_applied = true;
}

void adc_audio_clock_read(adc_audio_clock_t *out)
{
    *out = (adc_audio_clock_t) {
        .source_hz = s_source_hz,
        .div_num = APB_SARADC.apb_adc_clkm_conf.clkm_div_num,
        .div_a = APB_SARADC.apb_adc_clkm_conf.clkm_div_a,
        .div_b = APB_SARADC.apb_adc_clkm_conf.clkm_div_b,
        .interval = APB_SARADC.ctrl2.timer_target,
    };
    out->exact = s_applied && out->source_hz == AUDIO_ADC_SOURCE_HZ &&
                 APB_SARADC.apb_adc_clkm_conf.clk_sel == 2 &&
                 out->div_num == AUDIO_ADC_DIVISOR - 1U &&
                 out->div_a == ADC_LL_CLKM_DIV_A_DEFAULT &&
                 out->div_b == ADC_LL_CLKM_DIV_B_DEFAULT &&
                 out->interval == AUDIO_ADC_INTERVAL;
    if (out->exact) {
        out->configured_hz = out->source_hz /
                             ((out->div_num + 1U) * 2U * out->interval);
    }
}
