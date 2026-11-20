#include "eq_presets.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "app_control.h"
#include "cJSON.h"
#include "device_identity.h"
#include "device_name.h"
#include "eq6.h"
#include "nvs.h"
#include "radio_activity.h"
#include "voice.h"

enum { SLOT_COUNT = 3, STORE_FORMAT = 1, SLOT_MAX_DB = 6 };

typedef struct {
    char name[DEVICE_NAME_BYTES + 1];
    int8_t db[2][EQ6_BANDS];
} eq_slot_t;

typedef struct {
    uint32_t format;
    uint32_t revision;
    eq_slot_t slots[SLOT_COUNT];
} eq_store_t;

static esp_err_t load(eq_store_t *store)
{
    *store = (eq_store_t){ .format = STORE_FORMAT };
    nvs_handle_t handle;
    esp_err_t err = nvs_open("orl", NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) { return ESP_OK; }
    if (err != ESP_OK) { return err; }
    size_t size = sizeof(*store);
    err = nvs_get_blob(handle, "eq_slots", store, &size);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) { return ESP_OK; }
    if (err != ESP_OK) { return err; }
    if (size != sizeof(*store) || store->format != STORE_FORMAT) { return ESP_FAIL; }
    for (int i = 0; i < SLOT_COUNT; ++i) {
        const eq_slot_t *slot = &store->slots[i];
        if (!memchr(slot->name, 0, sizeof(slot->name)) || !device_name_validate(slot->name)) {
            return ESP_FAIL;
        }
        if (!slot->name[0]) { continue; }
        for (int d = 0; d < 2; ++d) {
            for (int b = 0; b < EQ6_BANDS; ++b) {
                if (slot->db[d][b] < EQ6_MIN_DB || slot->db[d][b] > SLOT_MAX_DB) { return ESP_FAIL; }
            }
        }
    }
    return ESP_OK;
}

static esp_err_t save(eq_store_t *store)
{
    if (store->revision == UINT32_MAX) { return ESP_ERR_INVALID_STATE; }
    ++store->revision;
    nvs_handle_t handle;
    esp_err_t err = nvs_open("orl", NVS_READWRITE, &handle);
    if (err != ESP_OK) { return err; }
    err = nvs_set_blob(handle, "eq_slots", store, sizeof(*store));
    if (err == ESP_OK) { err = nvs_commit(handle); }
    nvs_close(handle);
    return err;
}

static bool number(const cJSON *item, double min, double max, int64_t *out)
{
    if (!cJSON_IsNumber(item) || !(item->valuedouble >= min && item->valuedouble <= max) ||
        (double)(int64_t)item->valuedouble != item->valuedouble) { return false; }
    *out = (int64_t)item->valuedouble;
    return true;
}

static bool read_bands(const cJSON *body, const char *key, int max_db, int out[EQ6_BANDS])
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(body, key);
    if (!cJSON_IsArray(item) || cJSON_GetArraySize(item) != EQ6_BANDS) { return false; }
    for (int i = 0; i < EQ6_BANDS; ++i) {
        int64_t db;
        if (!number(cJSON_GetArrayItem(item, i), EQ6_MIN_DB, max_db, &db)) { return false; }
        out[i] = (int)db;
    }
    return true;
}

static bool read_name(const cJSON *body, char out[DEVICE_NAME_BYTES + 1])
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(body, "name");
    if (!cJSON_IsString(item)) { return false; }
    const char *first = item->valuestring;
    while (*first && isspace((unsigned char)*first)) { ++first; }
    const char *end = first + strlen(first);
    while (end > first && isspace((unsigned char)end[-1])) { --end; }
    if (end == first || end - first > DEVICE_NAME_BYTES) { return false; }
    memset(out, 0, DEVICE_NAME_BYTES + 1);
    memcpy(out, first, end - first);
    return device_name_validate(out);
}

static bool add_bands(cJSON *entry, const char *key, const int db[EQ6_BANDS])
{
    cJSON *values = cJSON_CreateIntArray(db, EQ6_BANDS);
    if (values && cJSON_AddItemToObject(entry, key, values)) { return true; }
    cJSON_Delete(values);
    return false;
}

static cJSON *slot_json(const eq_slot_t *slot)
{
    if (!slot->name[0]) { return cJSON_CreateNull(); }
    cJSON *entry = cJSON_CreateObject();
    int spk[EQ6_BANDS], mic[EQ6_BANDS];
    for (int b = 0; b < EQ6_BANDS; ++b) {
        spk[b] = slot->db[0][b];
        mic[b] = slot->db[1][b];
    }
    if (cJSON_AddStringToObject(entry, "name", slot->name) &&
        add_bands(entry, "spk", spk) && add_bands(entry, "mic", mic)) { return entry; }
    cJSON_Delete(entry);
    return NULL;
}

static cJSON *snapshot(const eq_store_t *store)
{
    cJSON *out = cJSON_CreateObject();
    cJSON *slots = cJSON_AddArrayToObject(out, "slots");
    int active[2][EQ6_BANDS];
    voice_eq_snapshot(active);
    if (!slots || !cJSON_AddBoolToObject(out, "ok", true) ||
        !cJSON_AddStringToObject(out, "device_id", device_identity_id()) ||
        !cJSON_AddStringToObject(out, "boot_id", radio_activity_boot_id()) ||
        !cJSON_AddNumberToObject(out, "revision", store->revision) ||
        !add_bands(out, "active_spk", active[0]) || !add_bands(out, "active_mic", active[1])) {
        cJSON_Delete(out);
        return NULL;
    }
    for (int i = 0; i < SLOT_COUNT; ++i) {
        cJSON *entry = slot_json(&store->slots[i]);
        if (!entry || !cJSON_AddItemToArray(slots, entry)) {
            cJSON_Delete(entry);
            cJSON_Delete(out);
            return NULL;
        }
    }
    return out;
}

static esp_err_t reply(httpd_req_t *req, cJSON *out)
{
    char *json = out ? cJSON_PrintUnformatted(out) : NULL;
    cJSON_Delete(out);
    if (!json) { return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory"); }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t err = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return err;
}

static esp_err_t failure(httpd_req_t *req, const char *status, const char *message)
{
    httpd_resp_set_status(req, status);
    cJSON *out = cJSON_CreateObject();
    if (!cJSON_AddBoolToObject(out, "ok", false) || !cJSON_AddStringToObject(out, "err", message)) {
        cJSON_Delete(out);
        out = NULL;
    }
    return reply(req, out);
}

static esp_err_t mutate(eq_store_t *store, const cJSON *body)
{
    const cJSON *op = cJSON_GetObjectItemCaseSensitive(body, "op");
    const cJSON *boot = cJSON_GetObjectItemCaseSensitive(body, "boot_id");
    int64_t index, revision;
    if (!cJSON_IsString(op) || !cJSON_IsString(boot) ||
        !number(cJSON_GetObjectItemCaseSensitive(body, "slot"), 1, SLOT_COUNT, &index) ||
        !number(cJSON_GetObjectItemCaseSensitive(body, "revision"), 0, UINT32_MAX, &revision)) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((uint32_t)revision != store->revision || strcmp(boot->valuestring, radio_activity_boot_id())) {
        return ESP_ERR_INVALID_STATE;
    }
    eq_slot_t *slot = &store->slots[index - 1];
    int curves[2][EQ6_BANDS];
    if (!strcmp(op->valuestring, "save")) {
        if (!read_name(body, slot->name) ||
            !read_bands(body, "spk", SLOT_MAX_DB, curves[0]) ||
            !read_bands(body, "mic", SLOT_MAX_DB, curves[1])) { return ESP_ERR_INVALID_ARG; }
        for (int d = 0; d < 2; ++d) {
            for (int b = 0; b < EQ6_BANDS; ++b) { slot->db[d][b] = (int8_t)curves[d][b]; }
        }
    } else if (!slot->name[0]) {
        return ESP_ERR_INVALID_STATE;
    } else if (!strcmp(op->valuestring, "rename")) {
        if (!read_name(body, slot->name)) { return ESP_ERR_INVALID_ARG; }
    } else if (!strcmp(op->valuestring, "delete")) {
        *slot = (eq_slot_t){0};
    } else if (!strcmp(op->valuestring, "apply")) {
        int expected[2][EQ6_BANDS];
        if (!read_bands(body, "expected_spk", EQ6_MAX_DB, expected[0]) ||
            !read_bands(body, "expected_mic", EQ6_MAX_DB, expected[1])) { return ESP_ERR_INVALID_ARG; }
        for (int d = 0; d < 2; ++d) {
            for (int b = 0; b < EQ6_BANDS; ++b) { curves[d][b] = slot->db[d][b]; }
        }
        return voice_eq_replace(expected, curves);
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    return save(store);
}

esp_err_t eq_presets_http(httpd_req_t *req)
{
    eq_store_t store;
    if (req->method == HTTP_GET) {
        if (load(&store) == ESP_OK) { return reply(req, snapshot(&store)); }
        return failure(req, "503 Service Unavailable", "读取 EQ 保存栏失败");
    }
    char id[24];
    if (httpd_req_get_hdr_value_str(req, "X-ORL-Device", id, sizeof(id)) != ESP_OK ||
        strcmp(id, device_identity_id())) {
        httpd_resp_set_hdr(req, "Connection", "close");
        return failure(req, "409 Conflict", "设备身份已变化，请重新连接");
    }
    cJSON *body = app_read_json(req);
    if (!body) { return ESP_FAIL; }
    esp_err_t err = load(&store);
    if (err == ESP_OK) { err = mutate(&store, body); }
    cJSON_Delete(body);
    if (err == ESP_OK) { return reply(req, snapshot(&store)); }
    if (err == ESP_ERR_INVALID_ARG) {
        return failure(req, "400 Bad Request", "名称或 EQ 参数无效");
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return failure(req, "409 Conflict", "预设或当前 EQ 已变化，请核对后重试");
    }
    return failure(req, "503 Service Unavailable", "EQ 写入未确认，已保存数据可重新读取");
}
