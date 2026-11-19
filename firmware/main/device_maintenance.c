#include "device_maintenance.h"
#include "maintenance_clock.h"
#include "factory_reset.h"
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
enum { REBOOT_NONE, REBOOT_MANUAL, REBOOT_SCHEDULED, REBOOT_FACTORY, REBOOT_UPDATE };

static maintenance_config_t s_cfg = {.version=2, .restart_minute=240, .cleanup_enabled=1};
static int64_t s_last_slot, s_due_slot, s_previous_utc, s_previous_ms, s_retry_at;
static bool s_restart_save_failed;
static maintenance_policy_t s_policy;
static esp_err_t s_storage = ESP_ERR_INVALID_STATE;
static unsigned s_writers, s_reboot_kind, s_previous_kind;
static bool s_update_writer;
static int64_t s_reboot_at, s_clean_at, s_defer_at, s_label_flush_at;
static uint32_t s_runs, s_delays, s_clean_bytes, s_decoders;
static const char *s_result = "not_run";
static RTC_NOINIT_ATTR uint32_t s_rtc_magic, s_rtc_kind;
static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static esp_err_t load_config(nvs_handle_t h)
{
    maintenance_config_t saved;
    size_t size = sizeof(saved);
    esp_err_t err = nvs_get_blob(h, "config2", &saved, &size);
    if (err == ESP_OK) {
        if (size != sizeof(saved) || !maintenance_config_valid(&saved)) { return ESP_ERR_INVALID_STATE; }
        s_cfg = saved;
        return ESP_OK;
    }
    if (err != ESP_ERR_NVS_NOT_FOUND) { return err; }

    /* Retain automatic cleanup preferences, but an uptime interval cannot
     * be converted into a weekday and clock time. Leave the new plan off. */
    struct {
        uint32_t version, revision;
        uint16_t hours;
        uint8_t cleanup_enabled, restart_enabled;
    } legacy;
    size = sizeof(legacy);
    err = nvs_get_blob(h, "config1", &legacy, &size);
    if (err == ESP_ERR_NVS_NOT_FOUND) { return ESP_OK; }
    if (err != ESP_OK) { return err; }
    if (size != sizeof(legacy) || legacy.version != 1 || legacy.hours < 1 || legacy.hours > 168 ||
        legacy.cleanup_enabled > 1 || legacy.restart_enabled > 1) {
        return ESP_ERR_INVALID_STATE;
    }
    s_cfg.revision = legacy.revision;
    s_cfg.cleanup_enabled = legacy.cleanup_enabled;
    return ESP_OK;
}

esp_err_t device_maintenance_init(void)
{
    if (s_lock) { return s_storage; }
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
    if (!s_lock) { return s_storage = ESP_ERR_NO_MEM; }
    if (esp_reset_reason() == ESP_RST_SW && s_rtc_magic == 0x484d5231u && s_rtc_kind <= REBOOT_UPDATE) {
        s_previous_kind = s_rtc_kind;
    }
    s_rtc_magic = s_rtc_kind = 0;
    nvs_handle_t h;
    s_storage = nvs_open("hm_maint", NVS_READONLY, &h);
    if (s_storage == ESP_OK) {
        s_storage = load_config(h);
        if (s_storage == ESP_OK) {
            const esp_err_t loaded = nvs_get_i64(h, "last_slot", &s_last_slot);
            if (loaded != ESP_OK && loaded != ESP_ERR_NVS_NOT_FOUND) { s_storage = loaded; }
            if (s_last_slot < 0) { s_storage = ESP_ERR_INVALID_STATE; }
        }
        nvs_close(h);
    } else if (s_storage == ESP_ERR_NVS_NOT_FOUND) {
        s_storage = ESP_OK;
    }
    maintenance_policy_reset(&s_policy, now_ms());
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
    bool ok = !s_reboot_kind && !s_update_writer;
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
bool device_maintenance_update_idle(void)
{
    if (!s_lock || board_boot_button_pressed() || strcmp(busy_reason(), "idle")) { return false; }
    if (esp_timer_get_time() - ptt_last_activity_us() < 2000000) { return false; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool ready = s_storage == ESP_OK && !s_reboot_kind && !s_due_slot;
    xSemaphoreGive(s_lock);
    return ready;
}

bool device_maintenance_update_begin(void)
{
    if (!s_lock) { return false; }
    ptt_control_lock();
    bool ok = device_maintenance_update_idle();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ok = ok && !s_reboot_kind && !s_due_slot && !s_writers && !tail_store_busy();
    if (ok) {
        s_update_writer = true;
        ++s_writers;
        ptt_maintenance_block(true);
    }
    xSemaphoreGive(s_lock);
    ptt_control_unlock();
    return ok;
}

void device_maintenance_update_end(bool reboot)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_update_writer) {
        s_update_writer = false;
        --s_writers;
        if (reboot) {
            s_reboot_kind = REBOOT_UPDATE;
            s_reboot_at = now_ms() + 1500;
        }
    }
    xSemaphoreGive(s_lock);
}

esp_err_t device_maintenance_set(const maintenance_config_t *cfg, uint32_t expected)
{
    if (!s_lock || !maintenance_config_valid(cfg)) { return ESP_ERR_INVALID_ARG; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = s_storage;
    if (err == ESP_OK && (expected != s_cfg.revision || expected == UINT32_MAX ||
                          s_reboot_kind == REBOOT_MANUAL || s_reboot_kind == REBOOT_FACTORY ||
                          s_reboot_kind == REBOOT_UPDATE || s_update_writer)) {
        err = ESP_ERR_INVALID_STATE;
    }
    if (err == ESP_OK) {
        maintenance_config_t next = *cfg;
        next.revision = expected + 1;
        nvs_handle_t h;
        err = nvs_open("hm_maint", NVS_READWRITE, &h);
        if (err == ESP_OK) {
            err = nvs_set_blob(h, "config2", &next, sizeof(next));
            if (err == ESP_OK) { err = nvs_commit(h); }
            nvs_close(h);
        }
        if (err == ESP_OK) {
            const bool changed = next.restart_enabled != s_cfg.restart_enabled ||
                next.restart_days != s_cfg.restart_days || next.restart_minute != s_cfg.restart_minute;
            s_cfg = next;
            maintenance_policy_reset(&s_policy, now_ms());
            if (changed) {
                s_previous_utc = s_due_slot = s_retry_at = 0;
                s_restart_save_failed = false;
                s_reboot_kind = REBOOT_NONE;
                s_reboot_at = 0;
            }
        }
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t device_maintenance_reboot(void)
{
    if (!s_lock) { return ESP_ERR_INVALID_STATE; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (s_writers || tail_store_busy() || s_reboot_kind == REBOOT_FACTORY || s_reboot_kind == REBOOT_UPDATE) {
        err = ESP_ERR_INVALID_STATE;
    } else if (s_reboot_kind != REBOOT_MANUAL) {
        s_reboot_kind = REBOOT_MANUAL;
        s_reboot_at = now_ms() + 1500;
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t device_maintenance_factory_reset(void)
{
    if (!s_lock || !app_control_generation_begin(0)) { return ESP_ERR_INVALID_STATE; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (!s_writers && !tail_store_busy()) {
        if (s_reboot_kind == REBOOT_FACTORY) {
            err = ESP_OK;
        } else if (s_reboot_kind == REBOOT_NONE) {
            err = factory_reset_prepare();
            if (err == ESP_OK) {
                s_reboot_kind = REBOOT_FACTORY;
                s_reboot_at = now_ms() + 1500;
            }
        }
    }
    xSemaphoreGive(s_lock);
    app_control_end();
    return err;
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
static void observe_schedule(int64_t now)
{
    const maintenance_clock_t clock = maintenance_clock_get();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_storage == ESP_OK && !s_due_slot && !s_reboot_kind &&
        maintenance_restart_due(&s_cfg, s_previous_utc, clock.utc, now - s_previous_ms, s_last_slot)) {
        s_due_slot = clock.utc / 60;
    }
    if (!clock.utc) {
        s_due_slot = s_retry_at = 0;
        s_restart_save_failed = false;
    }
    s_previous_utc = clock.utc;
    s_previous_ms = now;
    xSemaphoreGive(s_lock);
}

static void arm_due_restart(int64_t now)
{
    if (!s_due_slot || s_reboot_kind || s_writers || tail_store_busy() ||
        board_boot_button_pressed() || now < s_retry_at) { return; }
    /* Record this occurrence before restarting. A clock correction or boot
     * in the same minute must not cause another reboot, even after power loss. */
    nvs_handle_t h;
    esp_err_t err = nvs_open("hm_maint", NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_i64(h, "last_slot", s_due_slot);
        if (err == ESP_OK) { err = nvs_commit(h); }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        s_restart_save_failed = true;
        s_retry_at = now + 5000;
        return;
    }
    s_last_slot = s_due_slot;
    s_due_slot = s_retry_at = 0;
    s_restart_save_failed = false;
    s_reboot_kind = REBOOT_SCHEDULED;
    s_reboot_at = now;
}

void device_maintenance_tick(void)
{
    if (!s_lock) { return; }
    int64_t now = now_ms();
    /* Observe the wall-clock crossing even when an OTA upload owns the
     * mutation lock. An OTA that completes restarts the board itself. */
    observe_schedule(now);
    if (!app_control_generation_begin(0)) { return; }
    now = now_ms();
    const bool busy = strcmp(busy_reason(), "idle") != 0;
    int64_t last_activity = ptt_last_activity_us() / 1000;
    if (last_activity > now) { last_activity = now; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    arm_due_restart(now);
    const unsigned reboot = s_reboot_kind;
    const bool execute = reboot && now >= s_reboot_at && !s_writers;
    if (!busy && s_policy.idle_since_ms >= 0 && last_activity > s_policy.idle_since_ms) {
        s_policy.idle_since_ms = last_activity;
    }
    const bool cleanup = s_storage == ESP_OK && !reboot &&
        maintenance_policy_cleanup_due(&s_policy, &s_cfg, now, busy);
    if (!reboot && s_cfg.cleanup_enabled && now >= s_policy.next_cleanup_ms && busy &&
        (!s_defer_at || now - s_defer_at >= MAINTENANCE_CLEANUP_MS)) {
        ++s_delays;
        s_defer_at = now;
    }
    xSemaphoreGive(s_lock);
    /* GPIO0 must be released before reset so the ROM starts the app. */
    if (execute && !tail_store_busy() && !board_boot_button_pressed()) {
        s_rtc_kind = reboot;
        s_rtc_magic = 0x484d5231u;
        ptt_prepare_reboot();
        (void)sdr_set_enabled(false);
        (void)voice_stop();
        board_amp_enable(false);
        board_sa818_power(false);
        vTaskDelay(pdMS_TO_TICKS(50));
        esp_restart();
    } else if (cleanup) {
        (void)device_maintenance_cleanup();
    }
    if (!reboot && !busy && now >= s_label_flush_at) {
        (void)peer_labels_flush();
        s_label_flush_at = now + 30000;
    }
    app_control_end();
}

void device_maintenance_json(char *out, size_t capacity)
{
    if (!s_lock) { snprintf(out, capacity, "{\"version\":2,\"ready\":false}"); return; }
    const int64_t now = now_ms();
    const char *busy = busy_reason();
    const maintenance_clock_t clock = maintenance_clock_get();
    const int64_t last_activity = ptt_last_activity_us() / 1000;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int64_t next = s_due_slot ? s_due_slot * 60 : maintenance_next_restart(&s_cfg, clock.utc, s_last_slot);
    const int64_t since = last_activity > s_policy.idle_since_ms ? last_activity : s_policy.idle_since_ms;
    const int64_t idle = s_policy.idle_since_ms >= 0 && now >= since && !strcmp(busy, "idle") ?
        (now - since) / 1000 : 0;
    const char *reason = s_previous_kind == REBOOT_FACTORY ? "factory" :
        s_previous_kind == REBOOT_MANUAL ? "manual" : s_previous_kind == REBOOT_SCHEDULED ? "scheduled" : s_previous_kind == REBOOT_UPDATE ? "update" : "other";
    snprintf(out, capacity,
        "{\"version\":2,\"ready\":%s,\"revision\":%lu,\"cleanup_enabled\":%s,\"restart_enabled\":%s,"
        "\"restart_days\":%u,\"restart_minute\":%u,\"timezone\":\"Asia/Shanghai\","
        "\"clock_state\":\"%s\",\"last_sync_utc\":%lld,\"next_restart_utc\":%lld,"
        "\"restart_waiting\":%s,\"restart_save_failed\":%s,\"reboot_pending\":%s,\"last_reboot\":\"%s\","
        "\"cleanup_interval_s\":300,\"idle_required_s\":60,\"idle_s\":%lld,\"busy\":\"%s\","
        "\"cleanup_runs\":%lu,\"cleanup_delays\":%lu,\"last_cleanup_uptime_s\":%lld,\"released_buffer_bytes\":%lu,"
        "\"released_decoders\":%lu,\"result\":\"%s\",\"internal_free\":%u,\"internal_largest\":%u,\"psram_free\":%u}",
        s_storage == ESP_OK ? "true" : "false", (unsigned long)s_cfg.revision,
        s_cfg.cleanup_enabled ? "true" : "false", s_cfg.restart_enabled ? "true" : "false",
        (unsigned)s_cfg.restart_days, (unsigned)s_cfg.restart_minute,
        clock.state, (long long)clock.last_sync_utc, (long long)next,
        s_due_slot ? "true" : "false", s_restart_save_failed ? "true" : "false",
        s_reboot_kind ? "true" : "false", reason, (long long)idle, busy,
        (unsigned long)s_runs, (unsigned long)s_delays, (long long)(s_clean_at / 1000),
        (unsigned long)s_clean_bytes, (unsigned long)s_decoders, s_result,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    xSemaphoreGive(s_lock);
}
