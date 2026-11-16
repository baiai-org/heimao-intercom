#include "peer_labels.h"
#include "device_name.h"
#include "device_maintenance.h"
#include <string.h>
#include "nvs.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct { np_access_record_t ref; char label[PEER_LABEL_BYTES + 1]; } label_entry_t;
typedef struct { uint32_t format, revision; label_entry_t entries[NP_PEER_LIMIT]; } label_store_t;
static label_store_t *s_labels;
static StaticSemaphore_t s_mutex_storage;
static SemaphoreHandle_t s_mutex;
static esp_err_t s_error = ESP_ERR_INVALID_STATE;
static bool s_dirty;

static bool same_ref(const np_access_record_t *a, const np_access_record_t *b)
{
    if (np_access_has_mac(a->mac) && np_access_has_mac(b->mac)) { return !memcmp(a->mac, b->mac, 6); }
    return a->peer.port && a->peer.port == b->peer.port && a->peer.family == b->peer.family &&
        (a->peer.family == 4 || a->peer.family == 6) &&
        !memcmp(a->peer.address, b->peer.address, a->peer.family == 4 ? 4 : 16);
}
static esp_err_t save(const label_store_t *next)
{
    if (!device_maintenance_write_begin()) { return ESP_ERR_INVALID_STATE; }
    nvs_handle_t h;
    esp_err_t err = nvs_open("hm_peers", NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, "labels1", next, sizeof(*next));
        if (err == ESP_OK) { err = nvs_commit(h); }
        nvs_close(h);
    }
    device_maintenance_write_end(); return err;
}
esp_err_t peer_labels_init(void)
{
    if (s_mutex) { return s_error; }
    s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_storage);
    s_labels = heap_caps_calloc(1, sizeof(*s_labels), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_mutex || !s_labels) { return s_error = ESP_ERR_NO_MEM; }
    s_labels->format = 1;
    nvs_handle_t h;
    s_error = nvs_open("hm_peers", NVS_READONLY, &h);
    if (s_error == ESP_ERR_NVS_NOT_FOUND) { return s_error = ESP_OK; }
    if (s_error != ESP_OK) { return s_error; }
    size_t n = sizeof(*s_labels);
    s_error = nvs_get_blob(h, "labels1", s_labels, &n);
    nvs_close(h);
    if (s_error == ESP_ERR_NVS_NOT_FOUND) {
        memset(s_labels, 0, sizeof(*s_labels)); s_labels->format = 1; return s_error = ESP_OK;
    }
    if (s_error != ESP_OK) { return s_error; }
    if (n != sizeof(*s_labels) || s_labels->format != 1) { return s_error = ESP_ERR_INVALID_STATE; }
    for (unsigned i = 0; i < NP_PEER_LIMIT; ++i) {
        const label_entry_t *e = &s_labels->entries[i];
        if (!memchr(e->label, 0, sizeof(e->label)) || !device_name_validate(e->label) ||
            (e->label[0] && !np_access_has_mac(e->ref.mac) &&
             (!e->ref.peer.port || (e->ref.peer.family != 4 && e->ref.peer.family != 6)))) {
            return s_error = ESP_ERR_INVALID_STATE;
        }
    }
    return ESP_OK;
}
bool peer_labels_ready(void) { return s_error == ESP_OK; }
uint32_t peer_labels_revision(void)
{
    if (!s_labels || !s_mutex) { return 0; }
    xSemaphoreTake(s_mutex, portMAX_DELAY); uint32_t n = s_labels->revision; xSemaphoreGive(s_mutex); return n;
}
void peer_labels_get(const np_access_record_t *ref, char out[PEER_LABEL_BYTES + 1])
{
    out[0] = 0;
    if (!peer_labels_ready() || !s_mutex) { return; }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (unsigned i = 0; i < NP_PEER_LIMIT; ++i) {
        const label_entry_t *e = &s_labels->entries[i];
        if (e->label[0] && same_ref(&e->ref, ref)) { strcpy(out, e->label); break; }
    }
    xSemaphoreGive(s_mutex);
}
esp_err_t peer_labels_set(const np_access_record_t *ref, const char *label, uint32_t expected,
                          const np_access_record_t *active, size_t count)
{
    if (!ref || !label || !device_name_validate(label) || !active || count > NP_PEER_LIMIT) { return ESP_ERR_INVALID_ARG; }
    if (!peer_labels_ready() || !s_mutex) { return ESP_ERR_INVALID_STATE; }
    label_store_t *next = heap_caps_malloc(sizeof(*next), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!next) { return ESP_ERR_NO_MEM; }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (expected != s_labels->revision || expected == UINT32_MAX) { goto done; }
    memcpy(next, s_labels, sizeof(*next));
    int found = -1, empty = -1;
    for (unsigned i = 0; i < NP_PEER_LIMIT; ++i) {
        label_entry_t *e = &next->entries[i];
        if (e->label[0]) {
            bool live = false;
            for (size_t j = 0; j < count; ++j) { if (same_ref(&e->ref, &active[j])) { live = true; break; } }
            if (!live) { memset(e, 0, sizeof(*e)); }
        }
        if (e->label[0] && same_ref(&e->ref, ref)) { found = (int)i; }
        if (!e->label[0] && empty < 0) { empty = (int)i; }
    }
    if (found < 0 && !label[0]) { err = ESP_OK; goto done; }
    if (found < 0) { found = empty; }
    if (found < 0) { err = ESP_ERR_NO_MEM; goto done; }
    label_entry_t *e = &next->entries[found];
    if (!strcmp(e->label, label) && !s_dirty && same_ref(&e->ref, ref)) { err = ESP_OK; goto done; }
    memset(e, 0, sizeof(*e));
    if (label[0]) { e->ref = *ref; strcpy(e->label, label); }
    next->revision++;
    err = save(next);
    if (err == ESP_OK) { memcpy(s_labels, next, sizeof(*next)); s_dirty = false; }
done:
    xSemaphoreGive(s_mutex); heap_caps_free(next); return err;
}
void peer_labels_bind(const np_access_record_t *old_ref, const np_access_record_t *new_ref)
{
    if (!peer_labels_ready() || !s_mutex || !np_access_has_mac(new_ref->mac)) { return; }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (unsigned i = 0; i < NP_PEER_LIMIT; ++i) {
        label_entry_t *e = &s_labels->entries[i];
        if (e->label[0] && !np_access_has_mac(e->ref.mac) && same_ref(&e->ref, old_ref)) {
            e->ref = *new_ref; s_dirty = true;
        }
    }
    xSemaphoreGive(s_mutex);
}
esp_err_t peer_labels_flush(void)
{
    if (!peer_labels_ready() || !s_mutex) { return ESP_ERR_INVALID_STATE; }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_err_t err = s_dirty ? save(s_labels) : ESP_OK;
    if (err == ESP_OK) { s_dirty = false; }
    xSemaphoreGive(s_mutex); return err;
}
esp_err_t peer_labels_reset(void)
{
    if (!s_mutex || !s_labels) { return ESP_ERR_INVALID_STATE; }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    label_store_t *next = heap_caps_calloc(1, sizeof(*next), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    esp_err_t err = ESP_ERR_NO_MEM;
    if (next) {
        next->format = 1; next->revision = s_labels->revision == UINT32_MAX ? 0 : s_labels->revision + 1;
        err = save(next);
        if (err == ESP_OK) { memcpy(s_labels, next, sizeof(*next)); s_dirty = false; s_error = ESP_OK; }
        heap_caps_free(next);
    }
    xSemaphoreGive(s_mutex); return err;
}
