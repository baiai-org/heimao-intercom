#include "device_maintenance.h"
#include <stdio.h>
#include <string.h>
#include "nvs.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "app_control.h"
#include "board_hw.h"
#include "ptt.h"
#include "voice.h"
#include "voice_tail.h"
#include "speaker_diag.h"
#include "sdr.h"
#include "netp2pnet.h"
#include "netrelay.h"
#include "net_http.h"
#include "peer_labels.h"

static StaticSemaphore_t s_lock_storage;
static SemaphoreHandle_t s_lock;
static maintenance_config_t s_cfg = {.version=1, .restart_hours=24, .cleanup_enabled=1};
static maintenance_policy_t s_policy;
static esp_err_t s_storage = ESP_ERR_INVALID_STATE;
static unsigned s_writers, s_reboot_kind, s_previous_kind;
static int64_t s_reboot_at, s_clean_at, s_defer_at, s_label_flush_at;
static uint32_t s_runs, s_delays, s_clean_bytes, s_decoders;
static const char *s_result = "not_run";
static RTC_NOINIT_ATTR uint32_t s_rtc_magic, s_rtc_kind;
static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

esp_err_t device_maintenance_init(void)
{
    if (s_lock) { return s_storage; }
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
    if (!s_lock) { return s_storage = ESP_ERR_NO_MEM; }
    if (esp_reset_reason() == ESP_RST_SW && s_rtc_magic == 0x484d5231u && s_rtc_kind <= 2) {
        s_previous_kind = s_rtc_kind;
    }
    s_rtc_magic = s_rtc_kind = 0;
    nvs_handle_t h;
    s_storage = nvs_open("hm_maint", NVS_READONLY, &h);
    if (s_storage == ESP_OK) {
        maintenance_config_t saved; size_t n = sizeof(saved);
        s_storage = nvs_get_blob(h, "config1", &saved, &n); nvs_close(h);
        if (s_storage == ESP_OK) {
            if (n != sizeof(saved) || !maintenance_config_valid(&saved)) { s_storage = ESP_ERR_INVALID_STATE; }
            else { s_cfg = saved; }
        }
    }
    if (s_storage == ESP_ERR_NVS_NOT_FOUND) { s_storage = ESP_OK; }
    maintenance_policy_reset(&s_policy, &s_cfg, now_ms());
    return s_storage;
}
void device_maintenance_config(maintenance_config_t *out)
{
    if (!s_lock) { *out = s_cfg; return; }
    xSemaphoreTake(s_lock, portMAX_DELAY); *out = s_cfg; xSemaphoreGive(s_lock);
}
bool device_maintenance_reboot_pending(void)
{
    if (!s_lock) { return false; }
    xSemaphoreTake(s_lock, portMAX_DELAY); bool pending = s_reboot_kind != 0; xSemaphoreGive(s_lock); return pending;
}
bool device_maintenance_write_begin(void)
{
    if (!s_lock) { return true; } /* boot-time callers before maintenance init */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = !s_reboot_kind;
    if (ok) { ++s_writers; }
    xSemaphoreGive(s_lock); return ok;
}
void device_maintenance_write_end(void)
{
    if (!s_lock) { return; }
    xSemaphoreTake(s_lock, portMAX_DELAY); if (s_writers) { --s_writers; } xSemaphoreGive(s_lock);
}
static const char *busy_reason(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY); bool writing = s_writers != 0; xSemaphoreGive(s_lock);
    if (writing || tail_store_busy()) { return "writing"; }
    if (ptt_get_state() != PTT_IDLE || board_ptt_is_asserted() || ptt_owner() != PTT_OWNER_NONE ||
        ptt_local_audio_active() || voice_mode() == VOICE_TALK) { return "transmitting"; }
    if (voice_rf_carrier_present() || voice_network_busy()) { return "receiving"; }
    if (voice_bridge_level() || voice_sdr_tail_active() || tail_preview_active() || speaker_diag_output_active()) { return "audio_pending"; }
    return "idle";
}
esp_err_t device_maintenance_set(const maintenance_config_t *cfg, uint32_t expected)
{
    if (!s_lock || !maintenance_config_valid(cfg)) { return ESP_ERR_INVALID_ARG; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = s_storage;
    if (err == ESP_OK && (expected != s_cfg.revision || expected == UINT32_MAX || s_reboot_kind == 1)) { err = ESP_ERR_INVALID_STATE; }
    if (err == ESP_OK) {
        maintenance_config_t next = *cfg; next.revision = expected + 1;
        nvs_handle_t h; err = nvs_open("hm_maint", NVS_READWRITE, &h);
        if (err == ESP_OK) {
            err = nvs_set_blob(h, "config1", &next, sizeof(next));
            if (err == ESP_OK) { err = nvs_commit(h); }
            nvs_close(h);
        }
        if (err == ESP_OK) {
            s_cfg = next; maintenance_policy_reset(&s_policy, &s_cfg, now_ms());
            s_reboot_kind = 0; s_reboot_at = 0; /* cancel an old scheduled request */
        }
    }
    xSemaphoreGive(s_lock); return err;
}
esp_err_t device_maintenance_reboot(bool manual)
{
    if (!s_lock) { return ESP_ERR_INVALID_STATE; }
    if (tail_store_busy()) { return ESP_ERR_INVALID_STATE; }
    const bool idle = manual || !strcmp(busy_reason(), "idle");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (s_writers || !idle) { err = ESP_ERR_INVALID_STATE; }
    else if (!s_reboot_kind) { s_reboot_kind = manual ? 1 : 2; s_reboot_at = now_ms() + 1500; }
    else if (manual && s_reboot_kind == 2) { s_reboot_kind = 1; s_reboot_at = now_ms() + 1500; }
    xSemaphoreGive(s_lock); return err;
}
esp_err_t device_maintenance_cleanup(void)
{
    if (!s_lock || strcmp(busy_reason(), "idle")) { return ESP_ERR_INVALID_STATE; }
    const int64_t now = now_ms();
    const int64_t last_activity = ptt_last_activity_us() / 1000;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool idle = s_policy.idle_since_ms >= 0 && now - s_policy.idle_since_ms >= MAINTENANCE_IDLE_MS &&
        now - last_activity >= MAINTENANCE_IDLE_MS && !s_reboot_kind;
    xSemaphoreGive(s_lock);
    if (!idle) { return ESP_ERR_INVALID_STATE; }
    unsigned decoders = 0;
    const size_t bytes = netp2pnet_cleanup_idle(now, MAINTENANCE_IDLE_MS, &decoders) +
        net_http_cleanup_idle();
    decoders += netrelay_cleanup_idle(now, MAINTENANCE_IDLE_MS);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ++s_runs; s_clean_at = now; s_clean_bytes = (uint32_t)bytes; s_decoders = decoders;
    s_result = bytes || decoders ? "reclaimed" : "nothing_to_reclaim";
    s_policy.next_cleanup_ms = now + MAINTENANCE_CLEANUP_MS;
    xSemaphoreGive(s_lock); return ESP_OK;
}
void device_maintenance_tick(void)
{
    if (!s_lock) { return; }
    /* Serialize against HTTP mutations/uploads, with a bounded acquisition. */
    if (!app_control_generation_begin(0)) { return; }
    const int64_t now = now_ms();
    const bool busy = strcmp(busy_reason(), "idle") != 0;
    int64_t last_activity = ptt_last_activity_us() / 1000;
    if (last_activity > now) { last_activity = now; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!busy && s_policy.idle_since_ms >= 0 && last_activity > s_policy.idle_since_ms) { s_policy.idle_since_ms = last_activity; }
    const int action = s_storage == ESP_OK ? maintenance_policy_tick(&s_policy, &s_cfg, now, busy, false) : MAINTENANCE_NONE;
    if (s_cfg.cleanup_enabled && now >= s_policy.next_cleanup_ms && busy && (!s_defer_at || now - s_defer_at >= MAINTENANCE_CLEANUP_MS)) {
        ++s_delays; s_defer_at = now;
    }
    unsigned reboot = s_reboot_kind;
    bool execute = reboot && now >= s_reboot_at && !s_writers;
    /* Recheck scheduled requests at execution; fresh activity postpones them. */
    if (execute && reboot == 2 && (busy || s_policy.idle_since_ms < 0 || now - s_policy.idle_since_ms < MAINTENANCE_IDLE_MS)) {
        s_reboot_kind = 0; s_reboot_at = 0; execute = false;
    }
    xSemaphoreGive(s_lock);
    if (execute && !tail_store_busy()) {
        s_rtc_kind = reboot; s_rtc_magic = 0x484d5231u;
        ptt_prepare_reboot();
        (void)sdr_set_enabled(false);
        (void)voice_stop();
        board_amp_enable(false);
        esp_restart();
    } else if (!reboot && action == MAINTENANCE_RESTART) {
        (void)device_maintenance_reboot(false);
    } else if (!reboot && action == MAINTENANCE_CLEANUP) {
        (void)device_maintenance_cleanup();
    }
    if (!reboot && !busy && now >= s_label_flush_at) {
        (void)peer_labels_flush(); s_label_flush_at = now + 30000;
    }
    app_control_end();
}
void device_maintenance_json(char *out, size_t capacity)
{
    if (!s_lock) { snprintf(out, capacity, "{\"version\":1,\"ready\":false}"); return; }
    const int64_t now = now_ms(); const char *busy = busy_reason();
    const int64_t last_activity = ptt_last_activity_us() / 1000;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int64_t remaining = s_cfg.restart_enabled && s_policy.next_restart_ms > now ? (s_policy.next_restart_ms - now + 999) / 1000 : 0;
    const int64_t since = last_activity > s_policy.idle_since_ms ? last_activity : s_policy.idle_since_ms;
    const int64_t idle = s_policy.idle_since_ms >= 0 && now >= since && !strcmp(busy, "idle") ? (now - since) / 1000 : 0;
    snprintf(out, capacity,
        "{\"version\":1,\"ready\":%s,\"revision\":%lu,\"cleanup_enabled\":%s,\"restart_enabled\":%s,"
        "\"restart_hours\":%u,\"cleanup_interval_s\":300,\"idle_required_s\":60,\"idle_s\":%lld,\"busy\":\"%s\","
        "\"restart_remaining_s\":%lld,\"restart_waiting\":%s,\"reboot_pending\":%s,\"last_reboot\":\"%s\","
        "\"cleanup_runs\":%lu,\"cleanup_delays\":%lu,\"last_cleanup_uptime_s\":%lld,\"released_buffer_bytes\":%lu,"
        "\"released_decoders\":%lu,\"result\":\"%s\",\"internal_free\":%u,\"internal_largest\":%u,\"psram_free\":%u}",
        s_storage == ESP_OK ? "true" : "false", (unsigned long)s_cfg.revision,
        s_cfg.cleanup_enabled ? "true" : "false", s_cfg.restart_enabled ? "true" : "false", s_cfg.restart_hours,
        (long long)idle, busy, (long long)remaining, s_policy.restart_waiting ? "true" : "false", s_reboot_kind ? "true" : "false",
        s_previous_kind == 1 ? "manual" : s_previous_kind == 2 ? "scheduled" : "other",
        (unsigned long)s_runs, (unsigned long)s_delays, (long long)(s_clean_at / 1000), (unsigned long)s_clean_bytes,
        (unsigned long)s_decoders, s_result, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    xSemaphoreGive(s_lock);
}
