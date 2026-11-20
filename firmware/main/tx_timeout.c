#include "tx_timeout.h"

#include <stdatomic.h>
#include "nvs.h"

static atomic_uint s_seconds = TX_TIMEOUT_DEFAULT_S;
static const char *const KEY = "tx_timeout_s";

unsigned tx_timeout_seconds(void)
{
    return atomic_load(&s_seconds);
}

void tx_timeout_load(void)
{
    nvs_handle_t handle;
    uint16_t value = TX_TIMEOUT_DEFAULT_S;
    if (nvs_open("orl", NVS_READONLY, &handle) == ESP_OK) {
        if (nvs_get_u16(handle, KEY, &value) != ESP_OK || !tx_timeout_valid(value)) {
            value = TX_TIMEOUT_DEFAULT_S;
        }
        nvs_close(handle);
    }
    atomic_store(&s_seconds, value);
}

/* Settings writers are serialized by the HTTP control dispatcher. Audio
 * tasks only take a snapshot when a new transmission begins. */
esp_err_t tx_timeout_save(unsigned seconds, unsigned expected)
{
    if (!tx_timeout_valid(seconds) || !tx_timeout_valid(expected)) { return ESP_ERR_INVALID_ARG; }
    const unsigned current = tx_timeout_seconds();
    if (current == seconds) { return ESP_OK; }
    if (current != expected) { return ESP_ERR_INVALID_STATE; }
    nvs_handle_t handle;
    esp_err_t err = nvs_open("orl", NVS_READWRITE, &handle);
    if (err != ESP_OK) { return err; }
    err = nvs_set_u16(handle, KEY, (uint16_t)seconds);
    if (err == ESP_OK) { err = nvs_commit(handle); }
    nvs_close(handle);
    if (err == ESP_OK) { atomic_store(&s_seconds, seconds); }
    return err;
}
