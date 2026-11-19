#include "release_update.h"
#include "release_download.h"
#include "release_manifest.h"
#include "app_control.h"
#include "board_hw.h"
#include "device_identity.h"
#include "device_maintenance.h"
#include "maintenance_clock.h"
#include "net_wifi.h"
#include "netcom.h"
#include "ptt.h"
#include "radio_activity.h"
#include "sdr.h"
#include "voice.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UPDATE_BOOTLOADER_VERSION 10801
#define RECORD_FORMAT 1
#define DAY_US (24LL * 60 * 60 * 1000000)
#define RETRY_US (15LL * 60 * 1000000)
#define FORCE_RETRY_US (30LL * 1000000)

typedef struct {
    uint32_t format, partition;
    release_firmware_t asset;
} stage_record_t;

static const char *TAG = "release_update";
static StaticSemaphore_t s_lock_store;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static esp_timer_handle_t s_boot_timer;
static bool s_boot_pending, s_check, s_rollback_capable;
static char s_install[65];
static const char *s_state = "waiting";
static release_firmware_t s_view;
static bool s_install_available;
static stage_record_t s_staged;

static uint32_t firmware_code(const char *version)
{
    unsigned long value = 0;
    int used = 0;
    if (sscanf(version, "r11-%lu%n", &value, &used) != 1 || value > INT32_MAX) { return 0; }
    return !version[used] || !strncmp(version + used, "-g", 2) ? value : 0;
}

static void set_state(const char *state, const release_firmware_t *asset, bool install)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state = state;
    s_view = asset ? *asset : (release_firmware_t){0};
    s_install_available = install;
    xSemaphoreGive(s_lock);
}

static esp_err_t store(const char *key, void *value, size_t size)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("hm_update", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = value ? nvs_set_blob(handle, key, value, size) : nvs_erase_key(handle, key);
        if (err == ESP_OK) { err = nvs_commit(handle); }
        nvs_close(handle);
    }
    return err == ESP_ERR_NVS_NOT_FOUND && !value ? ESP_OK : err;
}

static bool load(const char *key, void *out, size_t size)
{
    nvs_handle_t handle;
    if (nvs_open("hm_update", NVS_READONLY, &handle) != ESP_OK) { return false; }
    const size_t expected = size;
    const esp_err_t err = nvs_get_blob(handle, key, out, &size);
    nvs_close(handle);
    return err == ESP_OK && size == expected;
}

static bool same_image(const release_firmware_t *a, const release_firmware_t *b)
{
    return a->code == b->code && a->size == b->size && !strcmp(a->hash, b->hash);
}

static bool digest_matches(const uint8_t digest[32], const char *hex)
{
    static const char alphabet[] = "0123456789abcdef";
    if (strlen(hex) != 64) { return false; }
    for (size_t i = 0; i < 32; ++i) {
        if (alphabet[digest[i] >> 4] != hex[i * 2] || alphabet[digest[i] & 15] != hex[i * 2 + 1]) {
            return false;
        }
    }
    return true;
}

static bool staged_matches(const esp_partition_t *part, const release_firmware_t *asset)
{
    if (!part || asset->size < 0x10000u || asset->size > part->size) { return false; }
    esp_app_desc_t desc;
    if (esp_ota_get_partition_description(part, &desc) != ESP_OK ||
        !memchr(desc.version, 0, sizeof(desc.version)) || firmware_code(desc.version) != asset->code ||
        strncmp(desc.project_name, esp_app_get_description()->project_name, sizeof(desc.project_name))) { return false; }
    uint8_t *chunk = heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!chunk) { return false; }
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    bool ok = mbedtls_sha256_starts(&sha, 0) == 0;
    for (size_t pos = 0; ok && pos < asset->size;) {
        const size_t count = asset->size - pos > 4096 ? 4096 : asset->size - pos;
        ok = esp_partition_read(part, pos, chunk, count) == ESP_OK &&
            mbedtls_sha256_update(&sha, chunk, count) == 0;
        pos += count;
        if (!(pos & 0x1ffff)) { vTaskDelay(1); }
    }
    uint8_t digest[32];
    ok = ok && !mbedtls_sha256_finish(&sha, digest) && digest_matches(digest, asset->hash);
    mbedtls_sha256_free(&sha);
    free(chunk);
    return ok;
}

static esp_err_t fetch_asset(release_firmware_t *asset, uint32_t installed)
{
    char *body = malloc(RELEASE_MANIFEST_MAX + 1);
    if (!body) { return ESP_ERR_NO_MEM; }
    size_t length = 0;
    esp_err_t err = release_download(RELEASE_MANIFEST_URL, body, RELEASE_MANIFEST_MAX, 0, &length);
    if (err == ESP_OK) {
        body[length] = '\0';
        err = release_manifest_read(body, length, installed, asset);
        if (err == ESP_ERR_NOT_FOUND) { err = ESP_ERR_NOT_SUPPORTED; }
    }
    free(body);
    return err;
}

static esp_err_t download_image(const release_firmware_t *asset, uint8_t **out)
{
    *out = NULL;
    uint8_t *image = heap_caps_malloc(asset->size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!image) { return ESP_ERR_NO_MEM; }
    char url[sizeof(RELEASE_ROOT) + sizeof(asset->tag) + sizeof(asset->file) + 12];
    snprintf(url, sizeof(url), RELEASE_ROOT "download/%s/%s", asset->tag, asset->file);
    size_t length = 0;
    esp_err_t err = release_download(url, image, asset->size, asset->size, &length);
    if (err == ESP_OK) {
        uint8_t digest[32];
        if (mbedtls_sha256(image, length, digest, 0) || !digest_matches(digest, asset->hash)) {
            err = ESP_ERR_INVALID_CRC;
        }
    }
    if (err == ESP_OK) {
        esp_app_desc_t desc;
        memcpy(&desc, image + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t), sizeof(desc));
        if (desc.magic_word != ESP_APP_DESC_MAGIC_WORD || !memchr(desc.version, 0, sizeof(desc.version)) ||
            firmware_code(desc.version) != asset->code ||
            strncmp(desc.project_name, esp_app_get_description()->project_name, sizeof(desc.project_name))) {
            err = ESP_ERR_INVALID_VERSION;
        }
    }
    if (err == ESP_OK) { *out = image; }
    else { free(image); }
    return err;
}

typedef struct {
    bool sdr_on, resume_engine, resume_voice;
} radio_pause_t;

static void end_flash(const radio_pause_t *previous, bool reboot)
{
    if (!reboot) {
        ptt_maintenance_block(false);
        if (previous->resume_voice) { (void)voice_start(VOICE_MONITOR); }
        if (previous->resume_engine) { (void)netcom_update_resume(); }
        if (previous->sdr_on) { (void)sdr_set_enabled(true); }
    }
    device_maintenance_update_end(reboot);
    app_control_end();
}

static esp_err_t begin_flash(radio_pause_t *previous)
{
    if (!app_control_generation_begin(0)) { return ESP_ERR_INVALID_STATE; }
    if (!device_maintenance_update_begin()) {
        app_control_end();
        return ESP_ERR_INVALID_STATE;
    }
    previous->sdr_on = sdr_enabled();
    previous->resume_engine = netcom_engine_running();
    previous->resume_voice = voice_monitor_operator_active();
    esp_err_t err = netcom_update_pause();
    if (err == ESP_OK) { err = sdr_set_enabled(false); }
    if (err == ESP_OK) { err = voice_stop(); }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Cannot stop audio for update: %s", esp_err_to_name(err));
        end_flash(previous, false);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t stage_image(const release_firmware_t *asset, uint8_t **image)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    if (!part || !running || !boot || boot->address != running->address ||
        part->address == running->address || asset->size > part->size) { return ESP_FAIL; }
    uint8_t *chunk = heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!chunk) { return ESP_ERR_NO_MEM; }
    radio_pause_t previous;
    esp_err_t err = begin_flash(&previous);
    if (err == ESP_OK) {
        set_state("staging", asset, false);
        esp_ota_handle_t handle;
        err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &handle);
        if (err == ESP_OK) {
            for (size_t pos = 0; err == ESP_OK && pos < asset->size;) {
                const size_t count = asset->size - pos > 4096 ? 4096 : asset->size - pos;
                memcpy(chunk, *image + pos, count);
                err = esp_ota_write(handle, chunk, count);
                pos += count;
                if (!(pos & 0xffff)) { vTaskDelay(1); }
            }
            if (err == ESP_OK) { err = esp_ota_end(handle); }
            else { esp_ota_abort(handle); }
        }
        if (err == ESP_OK && !staged_matches(part, asset)) { err = ESP_ERR_INVALID_CRC; }
        if (err == ESP_OK) {
            s_staged = (stage_record_t){.format=RECORD_FORMAT, .partition=part->address, .asset=*asset};
            err = store("stage", &s_staged, sizeof(s_staged));
            if (err != ESP_OK) { s_staged.format = 0; }
        }
        free(*image);
        *image = NULL;
        free(chunk);
        end_flash(&previous, false);
        return err;
    }
    free(chunk);
    return err;
}

static bool network_ready(void)
{
    return net_wifi_connected() && maintenance_clock_get().utc > 0;
}

static bool can_rollback(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!s_rollback_capable || !running) { return false; }
    esp_ota_img_states_t state;
    const esp_err_t err = esp_ota_get_state_partition(running, &state);
    /* Blank otadata after a USB full flash already boots the first slot. */
    if (err == ESP_ERR_NOT_FOUND) {
        const esp_partition_t *first = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
        return first && first->address == running->address;
    }
    return err == ESP_OK && (state == ESP_OTA_IMG_VALID || state == ESP_OTA_IMG_UNDEFINED);
}

static void show_staged(void)
{
    const release_firmware_t *asset = &s_staged.asset;
    if (!asset->force) { set_state("ready", asset, true); }
    else if (can_rollback()) { set_state("force_wait", asset, false); }
    else { set_state("rollback_required", asset, true); }
}

static esp_err_t activate_image(bool automatic)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!part || !boot || !running || boot->address != running->address ||
        part->address != s_staged.partition || (automatic && !can_rollback())) { return ESP_FAIL; }
    radio_pause_t previous;
    esp_err_t err = begin_flash(&previous);
    if (err != ESP_OK) { return err; }
    if (!staged_matches(part, &s_staged.asset)) {
        err = ESP_ERR_INVALID_CRC;
    } else {
        if (s_rollback_capable) {
            err = store("attempt", s_staged.asset.hash, sizeof(s_staged.asset.hash));
        }
        if (err == ESP_OK) { err = esp_ota_set_boot_partition(part); }
        if (err != ESP_OK && s_rollback_capable) { (void)store("attempt", NULL, 0); }
    }
    if (err == ESP_OK) { set_state("rebooting", &s_staged.asset, false); }
    end_flash(&previous, err == ESP_OK);
    return err;
}

static void clear_stage(void)
{
    s_staged.format = 0;
    (void)store("stage", NULL, 0);
}

static void revoke_install(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_install[0] = 0;
    xSemaphoreGive(s_lock);
}

static bool same_failed_image(const release_firmware_t *asset)
{
    char attempt[65];
    if (!load("attempt", attempt, sizeof(attempt)) || attempt[64] || strcmp(attempt, asset->hash)) { return false; }
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    esp_ota_img_states_t state;
    return part && esp_ota_get_state_partition(part, &state) == ESP_OK &&
        (state == ESP_OTA_IMG_INVALID || state == ESP_OTA_IMG_ABORTED);
}

static void run_update(void *unused)
{
    (void)unused;
    const uint32_t installed = firmware_code(esp_app_get_description()->version);
    int64_t check_at = 0, force_at = 0, stage_until = 0;
    uint8_t *image = NULL;
    release_firmware_t desired = {0};
    const esp_partition_t *spare = esp_ota_get_next_update_partition(NULL);
    if (!load("stage", &s_staged, sizeof(s_staged)) || s_staged.format != RECORD_FORMAT ||
        !spare || s_staged.partition != spare->address || s_staged.asset.code <= installed ||
        s_staged.asset.hash[64] || s_staged.asset.notes[RELEASE_NOTES_MAX]) {
        s_staged.format = 0;
    } else if (same_failed_image(&s_staged.asset)) {
        set_state("rolled_back", &s_staged.asset, false);
    } else if (staged_matches(spare, &s_staged.asset)) {
        show_staged();
    } else {
        clear_stage();
    }
    for (;;) {
        int64_t now = esp_timer_get_time();
        char manual[65];
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_check) { check_at = 0; s_check = false; }
        strcpy(manual, s_install);
        xSemaphoreGive(s_lock);
        if (network_ready() && now >= check_at) {
            set_state("checking", s_staged.format ? &s_staged.asset : NULL, false);
            release_firmware_t latest;
            esp_err_t err = fetch_asset(&latest, installed);
            check_at = esp_timer_get_time() + (err == ESP_OK || err == ESP_ERR_NOT_FOUND ||
                err == ESP_ERR_NOT_SUPPORTED ? DAY_US : RETRY_US);
            if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_NOT_SUPPORTED) {
                free(image); image = NULL;
                clear_stage();
                revoke_install();
                set_state(err == ESP_ERR_NOT_FOUND ? "no_manifest" : "current", NULL, false);
            } else if (err != ESP_OK) {
                ESP_LOGW(TAG, "Manifest read failed: %s", esp_err_to_name(err));
                if (!s_staged.format && !image) { set_state("error", NULL, false); }
                else if (!image && same_failed_image(&s_staged.asset)) {
                    set_state("rolled_back", &s_staged.asset, false);
                } else if (!image) { show_staged(); }
            } else if (same_failed_image(&latest)) {
                free(image); image = NULL;
                revoke_install();
                set_state("rolled_back", &latest, false);
            } else if (s_staged.format && same_image(&s_staged.asset, &latest)) {
                if (s_staged.asset.force != latest.force || strcmp(s_staged.asset.notes, latest.notes)) {
                    s_staged.asset = latest;
                    if (store("stage", &s_staged, sizeof(s_staged)) != ESP_OK) { clear_stage(); }
                }
                if (s_staged.format) { show_staged(); }
                else { set_state("error", &latest, false); check_at = now + RETRY_US; }
            } else if (image && same_image(&desired, &latest)) {
                desired = latest;
            } else {
                free(image); image = NULL;
                clear_stage();
                revoke_install();
                set_state("downloading", &latest, false);
                err = download_image(&latest, &image);
                if (err == ESP_OK) {
                    desired = latest;
                    stage_until = esp_timer_get_time() + RETRY_US;
                } else {
                    ESP_LOGW(TAG, "Image download failed: %s", esp_err_to_name(err));
                    set_state("error", &latest, false);
                    check_at = esp_timer_get_time() + RETRY_US;
                }
            }
        }
        now = esp_timer_get_time();
        if (image) {
            set_state("waiting_idle", &desired, false);
            esp_err_t err = stage_image(&desired, &image);
            if (err != ESP_ERR_INVALID_STATE || now >= stage_until) {
                free(image); image = NULL;
                if (err == ESP_OK) { show_staged(); }
                else {
                    ESP_LOGW(TAG, "Stage failed: %s", esp_err_to_name(err));
                    set_state("error", &desired, false);
                    check_at = esp_timer_get_time() + RETRY_US;
                }
            }
        }
        if (s_staged.format && !same_failed_image(&s_staged.asset)) {
            const bool allowed_manual = *manual && !strcmp(manual, s_staged.asset.hash) &&
                (!s_staged.asset.force || !can_rollback());
            const bool automatic = s_staged.asset.force && can_rollback() && network_ready() && now >= force_at;
            if (automatic && device_maintenance_update_idle()) {
                force_at = now + FORCE_RETRY_US;
                release_firmware_t fresh;
                const esp_err_t err = fetch_asset(&fresh, installed);
                if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_NOT_SUPPORTED ||
                    (err == ESP_OK && !same_image(&fresh, &s_staged.asset))) {
                    clear_stage();
                    revoke_install();
                    set_state("checking", NULL, false);
                    check_at = 0;
                } else if (err == ESP_OK && !fresh.force) {
                    s_staged.asset = fresh;
                    if (store("stage", &s_staged, sizeof(s_staged)) != ESP_OK) { clear_stage(); }
                    if (s_staged.format) { show_staged(); }
                } else if (err == ESP_OK) {
                    esp_err_t result = activate_image(true);
                    if (result == ESP_OK) { break; }
                    if (result != ESP_ERR_INVALID_STATE) {
                        clear_stage();
                        check_at = esp_timer_get_time() + RETRY_US;
                        set_state("error", &fresh, false);
                    }
                }
            } else if (allowed_manual) {
                const esp_err_t result = activate_image(false);
                if (result == ESP_OK) { break; }
                if (result != ESP_ERR_INVALID_STATE) {
                    revoke_install();
                    clear_stage();
                    check_at = esp_timer_get_time() + RETRY_US;
                    set_state("error", NULL, false);
                }
            }
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000));
    }
    vTaskDelete(NULL);
}

static void unconfirmed_boot(void *unused)
{
    (void)unused;
    ESP_LOGE(TAG, "OTA startup did not complete");
    esp_restart();
}

bool release_update_watch_boot(void)
{
    esp_ota_img_states_t state;
    const esp_partition_t *running = esp_ota_get_running_partition();
    s_boot_pending = running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY;
    if (s_boot_pending) {
        const esp_timer_create_args_t timer = {.callback=unconfirmed_boot, .name="ota_health"};
        ESP_ERROR_CHECK(esp_timer_create(&timer, &s_boot_timer));
        ESP_ERROR_CHECK(esp_timer_start_once(s_boot_timer, 120000000));
    }
    return s_boot_pending;
}

void release_update_start(bool boot_healthy)
{
    if (s_boot_pending) {
        if (!boot_healthy || esp_ota_mark_app_valid_cancel_rollback() != ESP_OK) {
            ESP_LOGE(TAG, "OTA startup checks failed");
            (void)esp_ota_mark_app_invalid_rollback_and_reboot();
            esp_restart();
        }
        (void)esp_timer_stop(s_boot_timer);
        (void)esp_timer_delete(s_boot_timer);
    }
    esp_bootloader_desc_t desc;
    s_rollback_capable = esp_ota_get_bootloader_description(NULL, &desc) == ESP_OK &&
        desc.version == UPDATE_BOOTLOADER_VERSION;
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_store);
    if (!s_lock || xTaskCreate(run_update, "gh_update", 9216, NULL, 2, &s_task) != pdPASS) {
        s_task = NULL;
        ESP_LOGW(TAG, "Cannot start online updater");
    }
}

static esp_err_t status_reply(httpd_req_t *req)
{
    if (!s_lock) { return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "online update unavailable"); }
    cJSON *out = cJSON_CreateObject();
    if (!out) { return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory"); }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    char version[24] = "";
    if (s_view.code) { snprintf(version, sizeof(version), "r11-%lu", (unsigned long)s_view.code); }
    cJSON_AddBoolToObject(out, "ok", true);
    cJSON_AddStringToObject(out, "device_id", device_identity_id());
    cJSON_AddStringToObject(out, "boot_id", radio_activity_boot_id());
    cJSON_AddStringToObject(out, "state", s_task ? s_state : "error");
    cJSON_AddStringToObject(out, "version", version);
    cJSON_AddStringToObject(out, "sha256", s_view.hash);
    cJSON_AddStringToObject(out, "notes", s_view.notes);
    cJSON_AddBoolToObject(out, "install", s_install_available);
    xSemaphoreGive(s_lock);
    char *body = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    if (!body) { return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory"); }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t err = httpd_resp_sendstr(req, body);
    free(body);
    return err;
}

esp_err_t release_update_http(httpd_req_t *req)
{
    if (req->method == HTTP_GET) { return status_reply(req); }
    char identity[24];
    if (!s_task || httpd_req_get_hdr_value_str(req, "X-ORL-Device", identity, sizeof(identity)) != ESP_OK ||
        strcmp(identity, device_identity_id())) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "device identity required");
    }
    cJSON *body = app_read_json(req);
    if (!body) { return ESP_FAIL; }
    const cJSON *boot = cJSON_GetObjectItemCaseSensitive(body, "boot_id");
    const cJSON *op = cJSON_GetObjectItemCaseSensitive(body, "op");
    const cJSON *hash = cJSON_GetObjectItemCaseSensitive(body, "sha256");
    bool accepted = false;
    if (cJSON_IsString(boot) && !strcmp(boot->valuestring, radio_activity_boot_id()) && cJSON_IsString(op)) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (!strcmp(op->valuestring, "check")) {
            s_check = true;
            accepted = true;
        } else if (!strcmp(op->valuestring, "install") && cJSON_IsString(hash) &&
                   s_install_available && !strcmp(hash->valuestring, s_view.hash)) {
            strcpy(s_install, s_view.hash);
            s_install_available = false;
            s_state = "install_wait";
            accepted = true;
        }
        xSemaphoreGive(s_lock);
    }
    cJSON_Delete(body);
    if (!accepted) { return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "update state changed"); }
    xTaskNotifyGive(s_task);
    return status_reply(req);
}
