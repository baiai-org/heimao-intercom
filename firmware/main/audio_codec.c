/*
 * ES8311 codec control -- implementation.
 */
#include "audio_codec.h"

#include <string.h>

#include "es8311.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "audio_i2s.h"
#include "board_hw.h"

static const char *TAG = "codec";

static i2c_master_dev_handle_t s_dev;
static es8311_handle_t         s_codec;
static bool                    s_ready;
static StaticSemaphore_t s_mute_lock_storage;
static SemaphoreHandle_t s_mute_lock;
static bool s_diag_mute;
static uint8_t s_original_reg31;

static esp_err_t read_reg(uint8_t address, uint8_t *value)
{
    return i2c_master_transmit_receive(s_dev, &address, 1, value, 1, 20);
}

static esp_err_t write_reg(uint8_t address, uint8_t value)
{
    const uint8_t data[] = {address, value};
    return i2c_master_transmit(s_dev, data, sizeof(data), 20);
}

static esp_err_t write_verify_reg31(uint8_t value, uint8_t *readback)
{
    esp_err_t err = write_reg(0x31, value);
    if (err != ESP_OK) { return err; }
    uint8_t actual;
    err = read_reg(0x31, &actual);
    if (err == ESP_OK) {
        if (readback) { *readback = actual; }
        if (actual != value) { err = ESP_ERR_INVALID_STATE; }
    }
    return err;
}
/*
 * ES8311 DAC digital volume.
 *
 * 60 is Espressif's own default for this codec: the i2s_es8311 example's
 * Kconfig.projbuild declares config EXAMPLE_VOICE_VOLUME with ange 0 100`r
 * and default 60, and the generated sdkconfig carries 60.
 *
 * It is a DIGITAL volume in front of the DAC (about 0.5 dB per step), so
 * raising it eats headroom rather than adding loudness for free: 88 is roughly
 * +24 dB of digital gain over 60 and can clip inside the codec.  Loudness
 * belongs to the analogue side (the LM4871's gain-setting resistors); this
 * stays at the reference value and can be nudged live with cvol.
 */
static int                     s_volume = 60;

esp_err_t audio_codec_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    if (!board_i2c_bus()) {
        ESP_LOGE(TAG, "I2C bus is not open");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = board_i2c_add_device(BOARD_ES8311_I2C_ADDR, 0, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device(0x%02X): %s",
                 BOARD_ES8311_I2C_ADDR, esp_err_to_name(err));
        return err;
    }

    s_codec = es8311_create(s_dev);
    if (!s_codec) {
        ESP_LOGE(TAG, "es8311_create failed");
        board_i2c_remove_device(s_dev);
        s_dev = NULL;
        return ESP_ERR_NO_MEM;
    }

    const es8311_clock_config_t clk = {
        .mclk_inverted      = false,
        .sclk_inverted      = false,
        .mclk_from_mclk_pin = true,   /* this board routes MCLK on GPIO4 */
        .mclk_frequency     = AUDIO_SAMPLE_RATE_HZ * AUDIO_MCLK_MULTIPLE,
        .sample_frequency   = AUDIO_SAMPLE_RATE_HZ,
    };

    err = es8311_init(s_codec, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "es8311_init: %s", esp_err_to_name(err));
        return err;
    }

    /*
     * Explicit sample-frequency configuration, exactly as Espressif's own
     * i2s_es8311 example does it.  es8311_init() does reach this internally
     * through es8311_clock_config(), but the example calls it again with the
     * MCLK frequency spelled out, and doing the same removes any dependence on
     * that internal path.
     */
    err = es8311_sample_frequency_config(s_codec,
                                         AUDIO_SAMPLE_RATE_HZ * AUDIO_MCLK_MULTIPLE,
                                         AUDIO_SAMPLE_RATE_HZ);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "es8311_sample_frequency_config: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "ES8311 configured: %d Hz, MCLK %d Hz (x%d), 16-bit",
             AUDIO_SAMPLE_RATE_HZ, AUDIO_SAMPLE_RATE_HZ * AUDIO_MCLK_MULTIPLE,
             AUDIO_MCLK_MULTIPLE);

    /* Analog MEMS microphone (MK1), not a PDM part. */
    err = es8311_microphone_config(s_codec, false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "es8311_microphone_config: %s", esp_err_to_name(err));
    }
    es8311_microphone_gain_set(s_codec, ES8311_MIC_GAIN_30DB);

    err = es8311_voice_volume_set(s_codec, s_volume, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "es8311_voice_volume_set: %s", esp_err_to_name(err));
    }
    es8311_voice_mute(s_codec, false);

    /*
     * Prove the bus really works, not just that the calls returned: read the
     * volume back out of the chip.
     */
    int readback = -1;
    if (es8311_voice_volume_get(s_codec, &readback) == ESP_OK) {
        ESP_LOGI(TAG, "ES8311 up: %d Hz, MCLK %d Hz, volume %d (read back %d)",
                 AUDIO_SAMPLE_RATE_HZ, clk.mclk_frequency, s_volume, readback);
        if (readback != s_volume) {
            ESP_LOGW(TAG, "volume read back as %d, expected %d", readback, s_volume);
        }
    } else {
        ESP_LOGW(TAG, "volume read-back failed -- I2C writes may not be landing");
    }

    s_mute_lock = xSemaphoreCreateMutexStatic(&s_mute_lock_storage);
    s_ready = true;
    return ESP_OK;
}

bool audio_codec_ready(void) { return s_ready; }

/*
 * Microphone capture gain, and the ADC volume that sits beside it.
 *
 * These are separate controls on the ES8311 and both matter for how hot the
 * captured signal is:
 *
 *   REG16  mic PGA, 0/6/12/18/24/30/36/42 dB  (es8311_microphone_gain_set)
 *   REG17  ADC volume, written as 0xC8 by the vendor's own
 *          es8311_microphone_config() -- their comment calls it "ADC gain"
 *
 * They are exposed because the first microphone measurement came back at a
 * peak of 199 out of 32767, and it has to be established whether the capsule
 * responds at all before blaming anything else.
 */
static int s_mic_gain_db = 30;      /* encoder value 0..7 -> 0/6/.../42 dB */
static int s_adc_volume  = 0xC8;

esp_err_t audio_codec_set_mic_gain(int gain_index)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (gain_index < 0) { gain_index = 0; }
    if (gain_index > 7) { gain_index = 7; }
    esp_err_t err = es8311_microphone_gain_set(s_codec,
                                               (es8311_mic_gain_t)gain_index);
    if (err == ESP_OK) {
        s_mic_gain_db = gain_index * 6;
    }
    return err;
}

int audio_codec_get_mic_gain(int *gain_db)
{
    if (gain_db) { *gain_db = s_mic_gain_db; }
    return s_mic_gain_db / 6;
}

esp_err_t audio_codec_set_adc_volume(int volume)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (volume < 0)   { volume = 0; }
    if (volume > 255) { volume = 255; }
    esp_err_t err = es8311_adc_volume_set(s_codec, (uint8_t)volume);
    if (err == ESP_OK) {
        s_adc_volume = volume;
    }
    return err;
}

int audio_codec_get_adc_volume(void) { return s_adc_volume; }

esp_err_t audio_codec_set_volume(int volume)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    int applied = -1;
    esp_err_t err = es8311_voice_volume_set(s_codec, volume, &applied);
    if (err == ESP_OK) {
        s_volume = applied;
    }
    return err;
}

esp_err_t audio_codec_get_volume(int *volume)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    return es8311_voice_volume_get(s_codec, volume);
}

esp_err_t audio_codec_mute(bool mute)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_mute_lock, portMAX_DELAY);
    const esp_err_t err = s_diag_mute ? ESP_ERR_INVALID_STATE : es8311_voice_mute(s_codec, mute);
    xSemaphoreGive(s_mute_lock);
    return err;
}

esp_err_t audio_codec_diag_mute_begin(uint8_t *original)
{
    if (!s_ready || !original) { return ESP_ERR_INVALID_STATE; }
    xSemaphoreTake(s_mute_lock, portMAX_DELAY);
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (!s_diag_mute) {
        err = read_reg(0x31, &s_original_reg31);
        /* A muted baseline cannot provide an ordinary-zero comparison.
         * Never unmute something the operator had deliberately muted. */
        if (err == ESP_OK && (s_original_reg31 & 0x60)) { err = ESP_ERR_INVALID_STATE; }
        if (err == ESP_OK) { s_diag_mute = true; *original = s_original_reg31; }
    }
    xSemaphoreGive(s_mute_lock);
    return err;
}

esp_err_t audio_codec_diag_mute_set(bool mute, uint8_t *readback)
{
    if (!s_ready) { return ESP_ERR_INVALID_STATE; }
    xSemaphoreTake(s_mute_lock, portMAX_DELAY);
    const esp_err_t err = s_diag_mute ?
        write_verify_reg31(mute ? s_original_reg31 | 0x60 : s_original_reg31, readback) :
        ESP_ERR_INVALID_STATE;
    xSemaphoreGive(s_mute_lock);
    return err;
}

esp_err_t audio_codec_diag_mute_end(uint8_t *readback)
{
    if (!s_ready) { return ESP_ERR_INVALID_STATE; }
    xSemaphoreTake(s_mute_lock, portMAX_DELAY);
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (s_diag_mute) {
        err = write_verify_reg31(s_original_reg31, readback);
        if (err == ESP_OK) { s_diag_mute = false; }
    }
    xSemaphoreGive(s_mute_lock);
    return err;
}

esp_err_t audio_codec_diag_read(audio_codec_diag_t *out)
{
    if (!out) { return ESP_ERR_INVALID_ARG; }
    memset(out, 0, sizeof(*out));
    if (!s_ready) { return out->error = ESP_ERR_INVALID_STATE; }
    static const uint8_t registers[AUDIO_CODEC_DIAG_REG_COUNT] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
        0x0d, 0x0e, 0x10, 0x11, 0x12, 0x13, 0x14, 0x16, 0x17, 0x1c,
        0x31, 0x32, 0x33, 0x34, 0x35, 0x37
    };
    /* Expected bytes from this build's vendor initialization at 16 kHz.
     * Report other registers raw; do not infer undocumented bit meanings. */
    static const uint8_t expected[] = {0x80, 0x3f, 0, 0x10, 0x10, 0, 3, 0, 0xff, 0x0c, 0x0c};
    xSemaphoreTake(s_mute_lock, portMAX_DELAY);
    esp_err_t err = s_diag_mute ? ESP_ERR_INVALID_STATE : ESP_OK;
    for (unsigned i = 0; err == ESP_OK && i < AUDIO_CODEC_DIAG_REG_COUNT; ++i) {
        out->address[i] = registers[i];
        err = read_reg(registers[i], &out->value[i]);
        if (err == ESP_OK) { ++out->count; }
    }
    out->error = err;
    out->clock_format_match = err == ESP_OK && !memcmp(out->value, expected, sizeof(expected));
    xSemaphoreGive(s_mute_lock);
    return err;
}

void audio_codec_dump(void)
{
    if (!s_ready) {
        printf("  codec not initialised\n");
        return;
    }
    es8311_register_dump(s_codec);
}
