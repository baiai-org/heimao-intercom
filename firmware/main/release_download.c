#include "release_download.h"
#include "release_manifest.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_timer.h"

#define URL_MAX 3072

static bool has_prefix(const char *value, const char *prefix)
{
    return !strncmp(value, prefix, strlen(prefix));
}

static bool official_url(const char *url)
{
    return has_prefix(url, RELEASE_ROOT) ||
        has_prefix(url, "https://release-assets.githubusercontent.com/") ||
        has_prefix(url, "https://objects.githubusercontent.com/");
}

static esp_err_t on_header(esp_http_client_event_t *event)
{
    if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key && event->header_value &&
        !strcasecmp(event->header_key, "Location") && strlen(event->header_value) < URL_MAX) {
        strcpy(event->user_data, event->header_value);
    }
    return ESP_OK;
}

static esp_err_t read_body(esp_http_client_handle_t client, uint8_t *buffer, size_t capacity,
                           size_t expected, size_t *received, int64_t deadline)
{
    *received = 0;
    for (;;) {
        if (esp_timer_get_time() >= deadline) { return ESP_ERR_TIMEOUT; }
        uint8_t extra;
        size_t room = capacity - *received;
        int read = esp_http_client_read(client, room ? (char *)buffer + *received : (char *)&extra,
                                        room ? (room > 4096 ? 4096 : (int)room) : 1);
        if (read < 0) { return ESP_FAIL; }
        if (!read) {
            if (!esp_http_client_is_complete_data_received(client)) { return ESP_FAIL; }
            return !expected || expected == *received ? ESP_OK : ESP_ERR_INVALID_SIZE;
        }
        if (!room) { return ESP_ERR_INVALID_SIZE; }
        *received += (size_t)read;
    }
}

esp_err_t release_download(const char *url, void *buffer, size_t capacity, size_t expected, size_t *received)
{
    if (!official_url(url) || strlen(url) >= URL_MAX || expected > capacity) { return ESP_ERR_INVALID_ARG; }
    char *addresses = malloc(URL_MAX * 2);
    if (!addresses) { return ESP_ERR_NO_MEM; }
    char *next = addresses;
    char *redirect = addresses + URL_MAX;
    strcpy(next, url);
    const int64_t deadline = esp_timer_get_time() + 120000000;
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < 6 && esp_timer_get_time() < deadline; ++i) {
        redirect[0] = 0;
        esp_http_client_config_t config = {
            .url=next, .timeout_ms=12000, .buffer_size=2048, .buffer_size_tx=3072,
            .crt_bundle_attach=esp_crt_bundle_attach, .disable_auto_redirect=true,
            .event_handler=on_header, .user_data=redirect,
        };
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) { err = ESP_ERR_NO_MEM; break; }
        int status = 0;
        err = esp_http_client_set_header(client, "Cache-Control", "no-cache");
        if (err == ESP_OK) { err = esp_http_client_set_header(client, "Accept-Encoding", "identity"); }
        if (err == ESP_OK) { err = esp_http_client_open(client, 0); }
        if (err == ESP_OK) {
            const int64_t length = esp_http_client_fetch_headers(client);
            status = esp_http_client_get_status_code(client);
            if (status == 200) {
                if (length > (int64_t)capacity || (expected && length > 0 && length != (int64_t)expected)) {
                    err = ESP_ERR_INVALID_SIZE;
                } else {
                    err = read_body(client, buffer, capacity, expected, received, deadline);
                }
            } else {
                err = status == 404 ? ESP_ERR_NOT_FOUND : ESP_FAIL;
            }
        }
        esp_http_client_cleanup(client);
        if (status != 301 && status != 302 && status != 303 && status != 307 && status != 308) { break; }
        if (redirect[0] == '/' && redirect[1] != '/' && has_prefix(next, RELEASE_ROOT) &&
            strlen(redirect) + sizeof("https://github.com") <= URL_MAX) {
            strcpy(next, "https://github.com");
            strcat(next, redirect);
        } else {
            strcpy(next, redirect);
        }
        if (!official_url(next)) { err = ESP_ERR_INVALID_ARG; break; }
    }
    free(addresses);
    return err;
}
