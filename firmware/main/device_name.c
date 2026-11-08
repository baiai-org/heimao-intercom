#include "device_name.h"
#include <string.h>
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static device_name_t s_name = {.format = 1};
static StaticSemaphore_t s_lock_storage;
static SemaphoreHandle_t s_lock;
static esp_err_t s_storage = ESP_OK;

bool device_name_validate(const char *name)
{
    if (!name) { return false; }
    const size_t n = strlen(name);
    if (n > DEVICE_NAME_BYTES) { return false; }
    size_t chars = 0;
    for (size_t i = 0; i < n;) {
        const unsigned char c = (unsigned char)name[i++];
        uint32_t cp;
        unsigned more;
        uint32_t minimum;
        if (c < 0x80) { cp = c; more = 0; minimum = 0; }
        else if (c >= 0xc2 && c <= 0xdf) { cp = c & 0x1f; more = 1; minimum = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { cp = c & 0x0f; more = 2; minimum = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { cp = c & 7; more = 3; minimum = 0x10000; }
        else { return false; }
        if (i + more > n) { return false; }
        while (more--) {
            const unsigned char b = (unsigned char)name[i++];
            if ((b & 0xc0) != 0x80) { return false; }
            cp = (cp << 6) | (b & 0x3f);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) ||
            cp < 0x20 || (cp >= 0x7f && cp <= 0x9f) || ++chars > 24) { return false; }
    }
    return true;
}

esp_err_t device_name_init(void)
{
    if (s_lock) { return s_storage; }
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
    if (!s_lock) { return ESP_ERR_NO_MEM; }
    nvs_handle_t h;
    s_storage = nvs_open("hm_identity", NVS_READONLY, &h);
    if (s_storage == ESP_ERR_NVS_NOT_FOUND) { s_storage = ESP_OK; return ESP_OK; }
    if (s_storage != ESP_OK) { return s_storage; }
    device_name_t stored = {0}; size_t size = sizeof(stored);
    s_storage = nvs_get_blob(h, "name", &stored, &size);
    nvs_close(h);
    if (s_storage == ESP_ERR_NVS_NOT_FOUND) { s_storage = ESP_OK; }
    else if (s_storage == ESP_OK) {
        if (size != sizeof(stored) || stored.format != 1 ||
            !memchr(stored.name, 0, sizeof(stored.name)) || !device_name_validate(stored.name)) {
            s_storage = ESP_ERR_INVALID_STATE; /* retain corrupt storage for recovery */
        } else { s_name = stored; }
    }
    return s_storage;
}

void device_name_get(device_name_t *out)
{
    if (s_lock) { xSemaphoreTake(s_lock, portMAX_DELAY); }
    *out = s_name;
    if (s_lock) { xSemaphoreGive(s_lock); }
}

esp_err_t device_name_set(const char *name, uint32_t expected, device_name_t *out)
{
    if (!s_lock || !device_name_validate(name)) { return ESP_ERR_INVALID_ARG; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = s_storage;
    if (err == ESP_OK && (expected != s_name.revision || s_name.revision == UINT32_MAX)) { err = ESP_ERR_INVALID_STATE; }
    if (err == ESP_OK && strcmp(name, s_name.name)) {
        device_name_t next = {.format = 1, .revision = s_name.revision + 1};
        strcpy(next.name, name);
        nvs_handle_t h;
        err = nvs_open("hm_identity", NVS_READWRITE, &h);
        if (err == ESP_OK) {
            err = nvs_set_blob(h, "name", &next, sizeof(next));
            if (err == ESP_OK) { err = nvs_commit(h); }
            if (err == ESP_OK) { s_name = next; }
            nvs_close(h);
        }
    }
    *out = s_name;
    xSemaphoreGive(s_lock);
    return err;
}
