#include "app_control.h"
#include "control_lease.h"
#include "device_identity.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include "cJSON.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"

static control_lease_t s_lease;
static StaticSemaphore_t s_lock_storage;
static SemaphoreHandle_t s_lock;
static void (*s_revoke)(void);
static int64_t control_now(void) { return esp_timer_get_time() / 1000; }

static void expire_locked(void)
{
    if (control_lease_expired(&s_lease, control_now())) {
        control_lease_release(&s_lease);
        if (s_revoke) { s_revoke(); }
    }
}

static void lease_watch(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(250));
        if (xSemaphoreTakeRecursive(s_lock, 0) == pdTRUE) {
            expire_locked(); xSemaphoreGiveRecursive(s_lock);
        }
    }
}

esp_err_t app_control_init(void (*revoke)(void))
{
    if (s_lock) { return ESP_OK; }
    s_lock = xSemaphoreCreateRecursiveMutexStatic(&s_lock_storage);
    s_revoke = revoke;
    return xTaskCreateWithCaps(lease_watch, "control_lease", 3072, NULL, 3, NULL,
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t control_error(httpd_req_t *req, const char *status, const char *reason)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close"); /* unread POST bodies cannot become another request */
    char body[160]; snprintf(body, sizeof(body), "{\"ok\":false,\"err\":\"%s\"}", reason);
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

cJSON *app_read_json(httpd_req_t *req)
{
    char type[64] = {0};
    if (httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type)) != ESP_OK ||
        strncasecmp(type, "application/json", 16) || (type[16] && type[16] != ';')) {
        (void)control_error(req, "415 Unsupported Media Type", "application/json required"); return NULL;
    }
    if (req->content_len < 2 || req->content_len > 2048) {
        (void)control_error(req, "400 Bad Request", "JSON body must be 2..2048 bytes"); return NULL;
    }
    char *body = malloc(req->content_len + 1);
    if (!body) { (void)control_error(req, "503 Service Unavailable", "no memory"); return NULL; }
    size_t n = 0;
    while (n < req->content_len) {
        int r = httpd_req_recv(req, body+n, req->content_len-n);
        if (r <= 0) {
            free(body); (void)control_error(req, "400 Bad Request", "incomplete JSON body"); return NULL;
        }
        n += r;
    }
    body[n] = 0;
    bool embedded_nul = memchr(body, 0, n) != NULL;
    /* cJSON stores strings as C strings; reject decoded NUL rather than
     * silently accepting a truncated name, credential or parameter. */
    for (size_t i = 0; i + 1 < n; ++i) {
        if (body[i] != '\\') { continue; }
        if (i + 5 < n && !memcmp(body + i + 1, "u0000", 5)) { embedded_nul = true; }
        ++i; /* an escaped backslash cannot begin another escape */
    }
    cJSON *json = embedded_nul ? NULL : cJSON_ParseWithLengthOpts(body, n+1, NULL, true);
    free(body);
    bool valid = cJSON_IsObject(json);
    /* Reject duplicate keys instead of choosing one interpretation. */
    for (const cJSON *a = valid ? json->child : NULL; a; a = a->next) {
        for (const cJSON *b = a->next; b; b = b->next) {
            if (!strcmp(a->string, b->string)) { valid = false; }
        }
    }
    if (!valid) {
        cJSON_Delete(json); (void)control_error(req, "400 Bad Request", "invalid JSON object"); return NULL;
    }
    return json;
}

static void request_token(httpd_req_t *req, bool websocket, char token[65], char device[24])
{
    token[0] = device[0] = 0;
    if (httpd_req_get_hdr_value_str(req, "X-ORL-Control", token, 65) != ESP_OK) { token[0] = 0; }
    if (httpd_req_get_hdr_value_str(req, "X-ORL-Device", device, 24) != ESP_OK) { device[0] = 0; }
    if (websocket && !*token) {
        char query[240];
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
            if (httpd_query_key_value(query, "control", token, 65) != ESP_OK) { token[0] = 0; }
            if (httpd_query_key_value(query, "device", device, 24) != ESP_OK) { device[0] = 0; }
        }
    }
}

bool app_control_begin(httpd_req_t *req, bool websocket)
{
    /* Single-operator device: no control lease.  Mutations are still serialised
     * by the lock, which is what protects concurrent writes from interleaving. */
    (void)req; (void)websocket;
    if (!s_lock || xSemaphoreTakeRecursive(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        (void)control_error(req, "409 Conflict", "device mutation in progress"); return false;
    }
    return true;
}

bool app_control_generation_begin(uint32_t generation)
{
    (void)generation;
    if (!s_lock || xSemaphoreTakeRecursive(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) { return false; }
    return true;
}
uint32_t app_control_generation(void) { return s_lease.generation; }
void app_control_end(void) { xSemaphoreGiveRecursive(s_lock); }

esp_err_t app_control_http(httpd_req_t *req)
{
    cJSON *body = req->method == HTTP_POST ? app_read_json(req) : NULL;
    if (req->method == HTTP_POST && !body) { return ESP_FAIL; }
    if (!s_lock || xSemaphoreTakeRecursive(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        cJSON_Delete(body); return control_error(req, "409 Conflict", "device mutation in progress");
    }
    expire_locked();
    const char *error = NULL;
    if (body) {
        /* Compatibility shim only: the board has no control lease, so every op is
         * accepted and none of them changes who may write. */
        const cJSON *op = cJSON_GetObjectItemCaseSensitive(body, "op");
        const char *known[] = { "claim", "takeover", "renew", "release" };
        bool recognised = false;
        if (!cJSON_IsString(op)) { error = "op required"; }
        else {
            for (size_t i = 0; i < sizeof(known)/sizeof(known[0]); ++i) {
                if (!strcmp(op->valuestring, known[i])) { recognised = true; }
            }
            if (!recognised) { error = "unknown control operation"; }
        }
    }
    cJSON_Delete(body);
    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", !error);
    if (error) { cJSON_AddStringToObject(out, "err", error); httpd_resp_set_status(req, "409 Conflict"); }
    cJSON_AddBoolToObject(out, "held", false);
    cJSON_AddBoolToObject(out, "mine", true);   /* no lease: every client may act */
    cJSON_AddStringToObject(out, "device_id", device_identity_id());
    cJSON_AddBoolToObject(out, "manual", false);
    cJSON_AddNumberToObject(out, "generation", s_lease.generation);
    cJSON_AddNumberToObject(out, "expires_in_ms", 0);
    char *encoded = cJSON_PrintUnformatted(out); cJSON_Delete(out);
    xSemaphoreGiveRecursive(s_lock);
    if (!encoded) { return control_error(req, "503 Service Unavailable", "no memory"); }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t result = httpd_resp_send(req, encoded, HTTPD_RESP_USE_STRLEN); free(encoded);
    return result;
}
