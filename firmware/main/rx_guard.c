#include "rx_guard.h"
#include <string.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

typedef struct { uint32_t version; rx_guard_config_t mode[2]; } saved_config_t;
static saved_config_t s_cfg = {1, {{2000, 0, 7, 7, 0}, {2000, 0, 7, 7, 0}}};
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static StaticSemaphore_t s_save_storage;
static SemaphoreHandle_t s_save_lock;
static uint8_t s_mode, s_sources, s_outputs;
static bool s_active, s_keyed;
static int64_t s_deadline_us;

static bool valid(const rx_guard_config_t *c)
{
    return c->enabled <= 1 && c->delay_ms >= 1 && c->delay_ms <= RX_GUARD_MAX_MS &&
        c->sources >= 1 && c->sources <= RX_GUARD_ALL &&
        c->outputs >= 1 && c->outputs <= RX_GUARD_ALL && c->reserved == 0;
}

void rx_guard_init(void)
{
    s_save_lock = xSemaphoreCreateMutexStatic(&s_save_storage);
    saved_config_t cfg;
    size_t size = sizeof(cfg);
    nvs_handle_t h;
    if (nvs_open("orl", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_blob(h, "rxguard1", &cfg, &size) == ESP_OK && size == sizeof(cfg) &&
            cfg.version == 1 && valid(&cfg.mode[0]) && valid(&cfg.mode[1])) {
            s_cfg = cfg;
        }
        nvs_close(h);
    }
}

void rx_guard_context(uint8_t mode, bool active)
{
    active = active && (mode == 1 || mode == 2);
    portENTER_CRITICAL(&s_mux);
    if (s_mode != mode || s_active != active) {
        s_deadline_us = 0;
        s_outputs = 0;
    }
    s_mode = mode;
    s_active = active;
    portEXIT_CRITICAL(&s_mux);
}

void rx_guard_rf_state(bool keyed, uint8_t sources)
{
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_mux);
    if (keyed) {
        if (!s_keyed) {
            s_sources = 0;
            s_deadline_us = 0;
            s_outputs = 0;
        }
        s_sources |= sources;
    } else if (s_keyed && s_active) {
        const rx_guard_config_t *c = &s_cfg.mode[s_mode - 1];
        if (c->enabled && (c->sources & s_sources)) {
            s_deadline_us = now + (int64_t)c->delay_ms * 1000;
            s_outputs = c->outputs;
        }
    }
    s_keyed = keyed;
    portEXIT_CRITICAL(&s_mux);
}

static uint8_t blocked_locked(int64_t now)
{
    return s_active && !s_keyed && s_deadline_us > now ? s_outputs : 0;
}

uint8_t rx_guard_blocked_at(int64_t block_start_us)
{
    portENTER_CRITICAL(&s_mux);
    const uint8_t result = blocked_locked(block_start_us);
    portEXIT_CRITICAL(&s_mux);
    return result;
}

uint8_t rx_guard_blocked(void)
{
    return rx_guard_blocked_at(esp_timer_get_time());
}

void rx_guard_status(rx_guard_status_t *out)
{
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_mux);
    out->p2p = s_cfg.mode[0];
    out->relay = s_cfg.mode[1];
    out->blocked = blocked_locked(now);
    out->remaining_ms = out->blocked ? (uint32_t)((s_deadline_us - now + 999) / 1000) : 0;
    portEXIT_CRITICAL(&s_mux);
}

esp_err_t rx_guard_configure(uint8_t mode, const char *value)
{
    if ((mode != 1 && mode != 2) || !value || !s_save_lock) { return ESP_ERR_INVALID_ARG; }
    uint32_t fields[4] = {0};
    const char *p = value;
    for (unsigned i = 0; i < 4; ++i) {
        if (*p < '0' || *p > '9') { return ESP_ERR_INVALID_ARG; }
        do {
            const unsigned digit = (unsigned)(*p++ - '0');
            if (fields[i] > (RX_GUARD_MAX_MS - digit) / 10u) { return ESP_ERR_INVALID_ARG; }
            fields[i] = fields[i] * 10u + digit;
        } while (*p >= '0' && *p <= '9');
        if (i < 3 ? *p++ != ',' : *p != '\0') { return ESP_ERR_INVALID_ARG; }
    }
    if (fields[0] > 1 || fields[2] > 7 || fields[3] > 7) { return ESP_ERR_INVALID_ARG; }
    const rx_guard_config_t next = {fields[1], (uint8_t)fields[0],
        (uint8_t)fields[2], (uint8_t)fields[3], 0};
    if (!valid(&next)) { return ESP_ERR_INVALID_ARG; }
    xSemaphoreTake(s_save_lock, portMAX_DELAY);
    saved_config_t cfg;
    portENTER_CRITICAL(&s_mux);
    cfg = s_cfg;
    portEXIT_CRITICAL(&s_mux);
    cfg.mode[mode - 1] = next;
    nvs_handle_t h;
    esp_err_t err = nvs_open("orl", NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, "rxguard1", &cfg, sizeof(cfg));
        if (err == ESP_OK) { err = nvs_commit(h); }
        nvs_close(h);
    }
    if (err == ESP_OK) {
        portENTER_CRITICAL(&s_mux);
        s_cfg = cfg;
        if (mode == s_mode && !next.enabled) { s_deadline_us = 0; s_outputs = 0; }
        portEXIT_CRITICAL(&s_mux);
    }
    xSemaphoreGive(s_save_lock);
    return err;
}
