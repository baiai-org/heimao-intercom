#include "channel_control.h"
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "channels.h"
#include "sa818_cfg.h"
#include "bridge.h"

static SemaphoreHandle_t s_select_mux;
static atomic_int s_selected;

void channel_control_init(void)
{
    if (!s_select_mux) { s_select_mux = xSemaphoreCreateMutex(); }
}

static bool same_profile(const sa818_cfg_t *a, const sa818_cfg_t *b)
{
    /* apply_profile always disables the module tail and bypasses its high-pass.
     * Compare fields, not struct padding or those normalised legacy values. */
    return a->tx_mhz == b->tx_mhz && a->rx_mhz == b->rx_mhz
        && a->bandwidth == b->bandwidth && a->squelch == b->squelch
        && a->volume == b->volume && a->high_power == b->high_power
        && !strcmp(a->tx_ctcss, b->tx_ctcss) && !strcmp(a->rx_ctcss, b->rx_ctcss)
        && a->filter_pre_deemph == b->filter_pre_deemph
        && a->filter_lowpass == b->filter_lowpass;
}

int channel_current(void)
{
    sa818_cfg_t current, preset;
    if (!sa818_cfg_tx_ready() || sa818_cfg_load(&current) != ESP_OK) { return 0; }
    const int count = channels_count();
    const int selected = atomic_load(&s_selected);
    if (selected >= 1 && channels_get(selected - 1, NULL, 0, &preset)
        && same_profile(&current, &preset)) { return selected; }
    for (int i = 0; i < count; ++i) {
        if (channels_get(i, NULL, 0, &preset) && same_profile(&current, &preset)) {
            return i + 1;
        }
    }
    return 0;
}

esp_err_t channel_select(int index, bool bind_bridge, char *log, size_t log_len)
{
    if (log && log_len) { log[0] = 0; }
    if (!s_select_mux || xSemaphoreTake(s_select_mux, pdMS_TO_TICKS(2500)) != pdTRUE) {
        if (log && log_len) { snprintf(log, log_len, "信道正在切换，请稍后重试"); }
        return ESP_ERR_INVALID_STATE;
    }
    sa818_cfg_t cfg;
    esp_err_t err;
    if (index == 0 && bind_bridge) {
        err = bridge_channel_selected(0, true);
    } else if (index < 1 || !channels_get(index - 1, NULL, 0, &cfg)) {
        err = ESP_ERR_INVALID_ARG;
        if (log && log_len) { snprintf(log, log_len, "信道不存在，请刷新信道库"); }
    } else {
        err = sa818_cfg_update(&cfg, log, log_len);
        if (err == ESP_OK) {
            atomic_store(&s_selected, index);
            err = bridge_channel_selected(index, bind_bridge);
            if (err != ESP_OK && log && log_len) {
                snprintf(log, log_len, "信道已应用，但桥接选择保存失败，请重试");
            }
        }
        bridge_apply();
    }
    xSemaphoreGive(s_select_mux);
    return err;
}
