#include "device_identity.h"
#include <stdio.h>
#include "esp_mac.h"
#include "esp_err.h"
static char s_id[24];
void device_identity_init(void)
{
    if (*s_id) { return; }
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    snprintf(s_id, sizeof(s_id), "orl-%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
const char *device_identity_id(void) { return s_id; }
