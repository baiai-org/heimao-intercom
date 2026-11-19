#include "maintenance_clock.h"
#include <time.h>
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#define FRESH_SYNC_US (2LL * 60 * 60 * 1000000)
#define MAX_HOLDOVER_US (24LL * 60 * 60 * 1000000)
#define MIN_VALID_UTC 1704067200LL /* 2024-01-01 UTC */

static const char *TAG = "maintenance_clock";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_sync_us, s_sync_utc;
static bool s_started; /* Owned by the Wi-Fi worker. */

static void synced(struct timeval *value)
{
    if (value->tv_sec < MIN_VALID_UTC) { return; }
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    s_sync_us = now;
    s_sync_utc = value->tv_sec;
    portEXIT_CRITICAL(&s_lock);
}

void maintenance_clock_network_ready(void)
{
    esp_err_t err;
    if (s_started) {
        err = esp_netif_sntp_start();
    } else {
        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
            2, ESP_SNTP_SERVER_LIST("ntp.aliyun.com", "pool.ntp.org"));
        config.sync_cb = synced;
        config.wait_for_sync = false;
        err = esp_netif_sntp_init(&config);
        s_started = err == ESP_OK;
    }
    if (err != ESP_OK) { ESP_LOGW(TAG, "start SNTP: %s", esp_err_to_name(err)); }
}

maintenance_clock_t maintenance_clock_get(void)
{
    maintenance_clock_t clock = {.state = "waiting"};
    portENTER_CRITICAL(&s_lock);
    const int64_t sync_us = s_sync_us;
    clock.last_sync_utc = s_sync_utc;
    portEXIT_CRITICAL(&s_lock);
    if (!clock.last_sync_utc) { return clock; }

    const int64_t age = esp_timer_get_time() - sync_us;
    const int64_t utc = time(NULL);
    if (age >= MAX_HOLDOVER_US || utc < MIN_VALID_UTC) {
        clock.state = "stale";
    } else {
        clock.utc = utc;
        clock.state = age >= FRESH_SYNC_US ? "holdover" : "synced";
    }
    return clock;
}
