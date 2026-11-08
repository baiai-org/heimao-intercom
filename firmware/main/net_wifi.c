/* Saved Wi-Fi management. All driver transitions run in one worker. */
#include "net_wifi.h"
#include "brand.h"
#include "wifi_policy.h"
#include "device_identity.h"
#include "device_name.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "dns_server.h"
#include "mdns.h"

static const char *TAG = "wifi";
#define NVS_NS_NET "orl"
#define NVS_KEY_PROFILES "wprofiles1"
#define SCAN_MAX 32
#define EV_SCAN BIT0
#define EV_DISC BIT1
#define EV_IP BIT2
#define EV_ASSOC BIT3
#define EV_LOST_IP BIT4
#define EV_AP_JOIN BIT5

typedef struct {
    wifi_profile_store_t store;
    wifi_policy_t policy;
    wifi_ap_record_t records[SCAN_MAX];
    char scan_json[4000];
} wifi_manager_t;

static wifi_manager_t *s_manager; /* PSRAM: profiles, results and JSON */
static StaticSemaphore_t s_mutex_storage;
static SemaphoreHandle_t s_mutex;
static TaskHandle_t s_worker;
static portMUX_TYPE s_event_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_pending_events;
static uint32_t s_disc_count;
static int s_disc_reason = -1;
static int64_t s_disc_ms;
static esp_netif_t *s_netif, *s_ap_netif;
static bool s_started, s_connected, s_ap_on, s_mdns_up, s_storage_fault;
static bool s_debug_stop_pending, s_debug_radio_off;
static bool s_scan_running, s_scan_requested, s_request_ap, s_request_disconnect;
static bool s_scan_discard;
static bool s_request_ap_close, s_ap_suppressed;
static uint32_t s_apply_id;
static int64_t s_apply_at, s_hold_until, s_next_scan, s_last_scan = -1, s_scan_until, s_disconnect_at;
static int64_t s_debug_at, s_announce_at;
static int64_t s_config_until;
static int64_t s_setup_until;
static char s_setup_owner[65];
static char s_ip[16], s_ssid[33], s_hostname[64] = "orl", s_ap_ssid[33];
static char s_state[24] = "off";
static int s_rssi;
static dns_server_handle_t s_dns;

static int64_t wifi_now(void) { return esp_timer_get_time() / 1000; }
static bool wifi_lock(void) { return s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(2000)) == pdTRUE; }
static void wifi_unlock(void) { xSemaphoreGive(s_mutex); }

/* No credentials are emitted by any JSON response. */
static void wifi_escape(const char *in, char *out, size_t len)
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 7 < len; ++p) {
        if (*p == '"' || *p == '\\') { out[o++] = '\\'; out[o++] = (char)*p; }
        else if (*p < 32) { o += (size_t)snprintf(out + o, len - o, "\\u%04x", *p); }
        else { out[o++] = (char)*p; }
    }
    out[o] = 0;
}

static esp_err_t persist_profiles(const wifi_profile_store_t *store)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS_NET, NVS_READWRITE, &h);
    if (err != ESP_OK) { return err; }
    err = nvs_set_blob(h, NVS_KEY_PROFILES, store, sizeof(*store));
    if (err == ESP_OK) { err = nvs_commit(h); }
    nvs_close(h);
    return err;
}

static esp_err_t load_profiles(void)
{
    wifi_profile_store_t *store = &s_manager->store;
    store->version = 1; store->next_id = 1;
    const char *identity = device_identity_id();
    /* Short factory name; the setup page forces the operator to pick their own
     * domain before provisioning, so this one is only the pre-setup fallback. */
    snprintf(s_hostname, sizeof(s_hostname), "heimao");
    strcpy(store->hostname, s_hostname);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS_NET, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) { return ESP_OK; }
    if (err != ESP_OK) { return err; }
    size_t len = sizeof(s_hostname);
    const bool legacy_host = nvs_get_str(h, "whost", s_hostname, &len) == ESP_OK;
    if (!legacy_host) { snprintf(s_hostname, sizeof(s_hostname), "heimao"); }
    len = sizeof(*store);
    err = nvs_get_blob(h, NVS_KEY_PROFILES, store, &len);
    if (err == ESP_OK) {
        nvs_close(h);
        if (len != sizeof(*store) || !wifi_profiles_valid(store)) { return ESP_ERR_INVALID_STATE; }
        if (store->hostname[0]) { strcpy(s_hostname, store->hostname); }
        return ESP_OK;
    }
    if (err != ESP_ERR_NVS_NOT_FOUND) { nvs_close(h); return err; }
    memset(store, 0, sizeof(*store)); store->version = 1; store->next_id = 1;
    wifi_profile_t *first = &store->profiles[0];
    len = sizeof(first->ssid);
    err = nvs_get_str(h, "wssid", first->ssid, &len);
    if (err == ESP_OK && first->ssid[0]) {
        if (!legacy_host) { strcpy(s_hostname, "orl"); } /* preserve the old implicit name */
        len = sizeof(first->password);
        err = nvs_get_str(h, "wpass", first->password, &len);
        if (err == ESP_ERR_NVS_NOT_FOUND) { first->password[0] = 0; err = ESP_OK; }
        if (err == ESP_OK) { first->id = store->next_id++; first->auto_join = 1; }
    } else if (err == ESP_ERR_NVS_NOT_FOUND) { err = ESP_OK; }
    nvs_close(h);
    strcpy(store->hostname, s_hostname);
    /* Do not erase legacy credentials: they remain available for rollback.
     * Even an empty versioned store prevents re-importing forgotten profiles. */
    if (err == ESP_OK) { err = persist_profiles(store); }
    return err;
}

static void start_mdns(void)
{
    if (s_mdns_up) { return; }
    if (mdns_init() != ESP_OK) { return; }
    mdns_hostname_set(s_hostname);
    mdns_instance_name_set(HM_DEVICE_NAME);
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    mdns_txt_item_t txt[] = {{"device_id", device_identity_id()}, {"api", "2"}, {"https", "443"},
        {"project", HM_PROJECT_NAME}, {"device_name", HM_DEVICE_NAME}};
    char display[64];
    const char *id = device_identity_id();
    snprintf(display, sizeof(display), HM_DEVICE_NAME "-%s", id + strlen(id) - 6);
    mdns_service_add(display, "_openradiolink", "_tcp", 80, txt, 5);
    s_mdns_up = true;
    net_wifi_name_changed();
}

void net_wifi_name_changed(void)
{
    device_name_t name; device_name_get(&name);
    if (s_mdns_up) {
        (void)mdns_service_txt_item_set("_openradiolink", "_tcp", "custom_name", name.name);
    }
    /* Worker advertises to phone gateways every ten seconds. */
}

static void ip6_ensure_linklocal(void)
{
    esp_ip6_addr_t a;
    if (s_netif && esp_netif_get_ip6_linklocal(s_netif, &a) != ESP_OK) {
        (void)esp_netif_create_ip6_linklocal(s_netif);
    }
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    uint32_t bits = 0;
    if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) { bits = EV_SCAN; }
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) { bits = EV_AP_JOIN; }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) { bits = EV_ASSOC; }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        portENTER_CRITICAL(&s_event_mux);
        ++s_disc_count; s_disc_reason = d ? d->reason : -1; s_disc_ms = wifi_now();
        portEXIT_CRITICAL(&s_event_mux);
        bits = EV_DISC;
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) { bits = EV_IP; }
    if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) { bits = EV_LOST_IP; }
    portENTER_CRITICAL(&s_event_mux);
    s_pending_events |= bits;
    portEXIT_CRITICAL(&s_event_mux);
    if (s_worker) { xTaskNotifyGive(s_worker); }
}

static void ap_stop(void)
{
    if (!s_ap_on) { return; }
    if (s_dns) { stop_dns_server(s_dns); s_dns = NULL; }
    (void)esp_wifi_set_mode(WIFI_MODE_STA);
    s_ap_on = false;
    s_setup_until = 0; s_setup_owner[0] = 0;
}

static esp_err_t ap_start(void)
{
    if (s_ap_on) { return ESP_OK; }
    wifi_ap_record_t associated;
    if (esp_wifi_sta_get_ap_info(&associated) == ESP_OK) { return ESP_ERR_INVALID_STATE; }
    if (!s_ap_netif) { s_ap_netif = esp_netif_create_default_wifi_ap(); }
    if (!s_ap_netif) { return ESP_ERR_NO_MEM; }
    wifi_config_t ap = {0};
    memcpy(ap.ap.ssid, s_ap_ssid, strlen(s_ap_ssid));
    ap.ap.ssid_len = strlen(s_ap_ssid); ap.ap.channel = 1;
    ap.ap.max_connection = 4; ap.ap.authmode = WIFI_AUTH_OPEN;
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) { err = esp_wifi_set_config(WIFI_IF_AP, &ap); }
    if (err != ESP_OK) { return err; }
    s_ap_on = true;
    static dns_server_config_t cfg = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
    if (!s_dns) { s_dns = start_dns_server(&cfg); }
    esp_netif_set_default_netif(s_netif);
    s_next_scan = wifi_now() + 1500;
    strcpy(s_state, "setup AP");
    ESP_LOGW(TAG, "open provisioning AP enabled; saved profiles preserved");
    return ESP_OK;
}

static void disconnect_for_switch(int64_t now)
{
    if (s_scan_running) { (void)esp_wifi_scan_stop(); s_scan_discard = true; }
    (void)esp_wifi_disconnect();
    s_connected = false; s_ip[0] = 0; s_rssi = 0;
    s_manager->policy.active = -1; s_manager->policy.attempt_until = 0;
    wifi_policy_offline(&s_manager->policy, now);
    /* Drain the old disconnect/scan events before a new association. APSTA
     * remains unassociated; this board's known AP+STA crash is not reintroduced. */
    s_hold_until = now + 750;
    s_request_disconnect = false;
}

static esp_err_t begin_candidate(int index, int64_t now)
{
    const wifi_profile_t *p = &s_manager->store.profiles[index];
    if (s_scan_running) { return ESP_ERR_INVALID_STATE; }
    ap_stop();
    wifi_config_t wc = {0};
    memcpy(wc.sta.ssid, p->ssid, strlen(p->ssid));
    memcpy(wc.sta.password, p->password, strlen(p->password));
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
#if CONFIG_ESP_WIFI_ENABLE_WPA3_SAE
    wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
#endif
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    snprintf(s_ssid, sizeof(s_ssid), "%s", p->ssid);
    wifi_policy_begin(&s_manager->policy, index, now);
    strcpy(s_state, "connecting");
    if (err == ESP_OK) { err = esp_wifi_connect(); }
    if (err != ESP_OK) { wifi_policy_failed(&s_manager->policy, now); }
    ESP_LOGI(TAG, "joining saved profile %lu (%s)", (unsigned long)p->id, esp_err_to_name(err));
    return err;
}

static bool auto_profiles(void)
{
    for (int i = 0; i < WIFI_PROFILE_MAX; ++i) {
        const wifi_profile_t *p = &s_manager->store.profiles[i];
        if (p->id && p->auto_join) { return true; }
    }
    return false;
}

static void collect_scan(int64_t now)
{
    uint16_t count = SCAN_MAX;
    const esp_err_t err = esp_wifi_scan_get_ap_records(&count, s_manager->records);
    (void)esp_wifi_clear_ap_list();
    s_scan_running = false;
    if (s_scan_discard || err != ESP_OK) { s_scan_discard = false; return; }
    wifi_policy_scan_clear(&s_manager->policy);
    size_t o = 1; s_manager->scan_json[0] = '[';
    int emitted = 0;
    for (int i = 0; i < count; ++i) {
        const wifi_ap_record_t *a = &s_manager->records[i];
        char ssid[33]; memcpy(ssid, a->ssid, 32); ssid[32] = 0;
        if (!ssid[0]) { continue; }
        for (int j = 0; j < WIFI_PROFILE_MAX; ++j) {
            if (s_manager->store.profiles[j].id && !strcmp(ssid, s_manager->store.profiles[j].ssid)) {
                wifi_policy_seen(&s_manager->policy, j, a->rssi);
            }
        }
        bool duplicate = false;
        for (int j = 0; j < i; ++j) {
            if (!memcmp(a->ssid, s_manager->records[j].ssid, 32)) { duplicate = true; break; }
        }
        if (duplicate || o + 260 >= sizeof(s_manager->scan_json)) { continue; }
        char name[200]; wifi_escape(ssid, name, sizeof(name));
        o += (size_t)snprintf(s_manager->scan_json + o, sizeof(s_manager->scan_json) - o,
            "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d}", emitted++ ? "," : "", name, a->rssi, a->authmode);
    }
    strcpy(s_manager->scan_json + o, "]");
    s_last_scan = now; s_next_scan = now + WIFI_SCAN_INTERVAL_MS;
}

static void announce_gateway(int64_t now)
{
    if (now < s_announce_at) { return; }
    s_announce_at = now + 10000;
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(s_netif, &info) != ESP_OK || !info.gw.addr) { return; }
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) { return; }
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(54322)};
    to.sin_addr.s_addr = info.gw.addr;
    device_name_t name; device_name_get(&name);
    char escaped[200]; wifi_escape(name.name, escaped, sizeof(escaped));
    char body[640];
    const int n = snprintf(body, sizeof(body),
        "{\"service\":\"OpenRadioLink\",\"project\":\"" HM_PROJECT_NAME "\",\"device_name\":\"" HM_DEVICE_NAME "\","
        "\"device_id\":\"%s\",\"custom_name\":\"%s\",\"api\":2,\"ip\":\"%s\",\"http\":80,\"https\":443}",
        device_identity_id(), escaped, s_ip);
    if (n > 0 && (size_t)n < sizeof(body)) {
        (void)sendto(fd, body, n, MSG_DONTWAIT, (struct sockaddr *)&to, sizeof(to));
    }
    close(fd);
}

static void manager_step(uint32_t events, int64_t now)
{
    wifi_policy_t *policy = &s_manager->policy;
    if (events & EV_SCAN) { collect_scan(now); }
    if (s_scan_running && now >= s_scan_until) {
        (void)esp_wifi_scan_stop(); (void)esp_wifi_clear_ap_list();
        s_scan_running = false; s_scan_discard = true;
    }
    if (s_debug_stop_pending && now >= s_debug_at) {
        if (s_scan_running) { (void)esp_wifi_scan_stop(); s_scan_discard = true; }
        ap_stop(); (void)esp_wifi_stop();
        s_started = s_connected = false; s_ip[0] = 0;
        s_debug_radio_off = true; s_debug_stop_pending = false;
        strcpy(s_state, "debug-off");
    }
    if (s_debug_stop_pending || s_debug_radio_off) { return; }
    if (s_request_ap_close) {
        s_request_ap_close = false; s_ap_suppressed = true; s_config_until = 0;
        ap_stop();
    }
    /* DHCP, the system connection dialog and first TLS verification need a
     * short grace period before the App can establish its renewable hold. */
    if ((events & EV_AP_JOIN) && s_ap_on && !s_apply_id && !s_ap_suppressed &&
        now + 30000 > s_config_until) { s_config_until = now + 30000; }
    if ((s_request_disconnect && now >= s_disconnect_at) || s_request_ap ||
        (s_apply_id && now >= s_apply_at && !s_hold_until)) {
        disconnect_for_switch(now);
        if (s_request_ap) {
            s_request_ap = false; s_apply_id = 0; policy->recovery_ap = true;
            wifi_policy_scan_clear(policy); s_last_scan = -1;
        }
    }
    /* GOT_IP is success only for the currently associated target. Stale IP
     * notifications cannot complete a different profile or reset recovery. */
    wifi_ap_record_t ap;
    const bool associated = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    if ((events & EV_ASSOC) && associated && s_netif) { (void)esp_netif_dhcpc_start(s_netif); }
    if ((events & EV_IP) && !s_hold_until && !s_ap_on && policy->active >= 0 && associated &&
        !strncmp((char *)ap.ssid, s_ssid, sizeof(ap.ssid))) {
        esp_netif_ip_info_t info;
        if (esp_netif_get_ip_info(s_netif, &info) == ESP_OK && info.ip.addr) {
            snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&info.ip));
            s_connected = true; wifi_policy_online(policy, now);
            strcpy(s_state, "up"); ip6_ensure_linklocal(); s_announce_at = now;
            ESP_LOGI(TAG, "saved profile acquired IP %s", s_ip);
        }
    }
    esp_netif_ip_info_t current_ip = {0};
    if (events & EV_LOST_IP) { (void)esp_netif_get_ip_info(s_netif, &current_ip); }
    if ((events & (EV_DISC | EV_LOST_IP)) && !s_hold_until &&
        ((!associated) || ((events & EV_LOST_IP) && !current_ip.ip.addr))) {
        const bool was_online = s_connected;
        s_connected = false; s_ip[0] = 0; s_rssi = 0;
        if (was_online) { wifi_policy_offline(policy, now); policy->active = -1; s_ap_suppressed = false; }
        else { wifi_policy_failed(policy, now); }
        if (associated) { disconnect_for_switch(now); }
        s_next_scan = now;
    }
    if (s_connected) { announce_gateway(now); return; } /* No WAN health input. */
    if (s_hold_until) {
        if (now < s_hold_until || associated) {
            if (associated && now >= s_hold_until) { (void)esp_wifi_disconnect(); s_hold_until = now + 250; }
            return;
        }
        s_hold_until = 0;
    }
    if (!policy->recovery_ap && wifi_policy_ap_due(policy, now)) {
        policy->recovery_ap = true;
        wifi_policy_failed(policy, now);
        disconnect_for_switch(now);
        wifi_policy_scan_clear(policy); s_last_scan = -1;
        return;
    }
    if (policy->active >= 0) {
        if (now >= policy->attempt_until) {
            wifi_policy_failed(policy, now); disconnect_for_switch(now); s_next_scan = now;
        }
        return;
    }
    if (s_apply_id && now >= s_apply_at && !s_scan_running) {
        const int index = wifi_profiles_find(&s_manager->store, s_apply_id);
        s_apply_id = 0;
        if (index >= 0) { (void)begin_candidate(index, now); return; }
    }
    if (s_apply_id) { return; } /* Explicit request waiting for its ACK delay. */
    if (!s_ap_suppressed && (policy->recovery_ap || !auto_profiles() || s_storage_fault)) { (void)ap_start(); }
    const bool configuring = s_ap_on && (now < s_config_until || now < s_setup_until);
    if (!configuring && !s_scan_running && s_last_scan >= 0 && now - s_last_scan <= 15000) {
        const int index = wifi_policy_pick(policy, &s_manager->store, now);
        if (index >= 0) { (void)begin_candidate(index, now); return; }
    }
    if (!s_scan_running && (!s_ap_on || now >= s_next_scan) &&
        (s_scan_requested || (!configuring && auto_profiles() && now >= s_next_scan))) {
        wifi_scan_config_t cfg = {0}; cfg.show_hidden = true;
        cfg.scan_time.active.min = 40; cfg.scan_time.active.max = 80;
        if (esp_wifi_scan_start(&cfg, false) == ESP_OK) {
            s_scan_running = true; s_scan_requested = false; s_scan_discard = false; s_scan_until = now + 5000;
        }
        s_next_scan = now + WIFI_SCAN_INTERVAL_MS;
    }
    if (!s_ap_on) { strcpy(s_state, "searching"); }
}

static void wifi_worker(void *arg)
{
    (void)arg;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
        if (!wifi_lock()) { continue; }
        portENTER_CRITICAL(&s_event_mux);
        uint32_t events = s_pending_events; s_pending_events = 0;
        portEXIT_CRITICAL(&s_event_mux);
        if (s_connected && s_scan_requested && !s_scan_running && !s_debug_stop_pending && !s_debug_radio_off) {
            wifi_scan_config_t scan = {0}; scan.show_hidden = true;
            scan.scan_time.active.min = 40; scan.scan_time.active.max = 80;
            if (esp_wifi_scan_start(&scan, false) == ESP_OK) {
                s_scan_running = true; s_scan_requested = false; s_scan_discard = false; s_scan_until = wifi_now() + 5000;
            }
        }
        manager_step(events, wifi_now());
        wifi_unlock();
    }
}

esp_err_t net_wifi_start(void)
{
    if (s_manager) { return s_debug_radio_off ? ESP_ERR_INVALID_STATE : ESP_OK; }
    s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_storage);
    s_manager = heap_caps_calloc(1, sizeof(*s_manager), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_manager) { return ESP_ERR_NO_MEM; }
    strcpy(s_manager->scan_json, "[]");
    device_identity_init();
    const esp_err_t storage = load_profiles();
    (void)device_name_init();
    if (storage != ESP_OK) {
        /* Preserve unknown/corrupt storage for recovery; never silently replace it. */
        s_storage_fault = true; memset(&s_manager->store, 0, sizeof(s_manager->store));
        s_manager->store.version = 1; s_manager->store.next_id = 1;
        ESP_LOGE(TAG, "Wi-Fi storage needs recovery: %s", esp_err_to_name(storage));
    }
    wifi_policy_init(&s_manager->policy, wifi_now());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();
    if (!s_netif) { return ESP_ERR_NO_MEM; }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    wifi_config_t empty = {0}; ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &empty));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL, NULL));
    device_identity_init();
    uint8_t mac[6]; ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_AP, mac));
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), HM_SETUP_PREFIX "%02X%02X%02X", mac[3], mac[4], mac[5]);
    start_mdns();
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    s_started = true; s_scan_requested = true;
    if (xTaskCreateWithCaps(wifi_worker, "wifi_mgr", 6144, NULL, 3, &s_worker,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) { return ESP_ERR_NO_MEM; }
    return ESP_OK;
}

static bool valid_host(const char *host, char normalized[64])
{
    if (!host || !*host) { normalized[0] = 0; return true; }
    const size_t n = strlen(host);
    if (n > 63 || !isalnum((unsigned char)host[0]) || !isalnum((unsigned char)host[n-1])) { return false; }
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)host[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')) { return false; }
        normalized[i] = (char)tolower(c);
    }
    normalized[n] = 0; return true;
}

esp_err_t net_wifi_profile_save(uint32_t *id, const char *ssid, const char *password,
                               bool auto_join, int priority, const char *host)
{
    char normalized[64];
    if (!id || (ssid && (!*ssid || strlen(ssid) > 32)) ||
        (password && strlen(password) > 64) || priority < -100 || priority > 100 || !valid_host(host, normalized)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!wifi_lock()) { return ESP_ERR_TIMEOUT; }
    if (s_storage_fault) { wifi_unlock(); return ESP_ERR_INVALID_STATE; }
    wifi_profile_store_t next = s_manager->store;
    int index = wifi_profiles_find(&next, *id);
    if (*id && index < 0) { wifi_unlock(); return ESP_ERR_NOT_FOUND; }
    if (!*id && ssid) {
        for (int i = 0; i < WIFI_PROFILE_MAX; ++i) {
            if (next.profiles[i].id && !strcmp(next.profiles[i].ssid, ssid)) { index = i; break; }
        }
    }
    if (index < 0) {
        if (!ssid || !password) { wifi_unlock(); return ESP_ERR_INVALID_ARG; }
        for (int i = 0; i < WIFI_PROFILE_MAX; ++i) { if (!next.profiles[i].id) { index = i; break; } }
        if (index < 0 || next.next_id == UINT32_MAX) { wifi_unlock(); return ESP_ERR_NO_MEM; }
        next.profiles[index].id = next.next_id++;
    }
    wifi_profile_t *p = &next.profiles[index];
    if (ssid) { snprintf(p->ssid, sizeof(p->ssid), "%s", ssid); }
    if (password) { snprintf(p->password, sizeof(p->password), "%s", password); }
    p->auto_join = auto_join; p->priority = priority;
    if (*normalized) { strcpy(next.hostname, normalized); }
    if (!wifi_profiles_valid(&next)) { wifi_unlock(); return ESP_ERR_INVALID_ARG; }
    esp_err_t err = persist_profiles(&next);
    if (err == ESP_OK) {
        s_manager->store = next; *id = p->id;
        memset(&s_manager->policy.candidates[index], 0, sizeof(wifi_candidate_t));
        s_manager->policy.candidates[index].rssi = -128;
        if (*normalized) {
            strcpy(s_hostname, normalized);
            if (s_mdns_up) { (void)mdns_hostname_set(s_hostname); }
        }
        s_next_scan = 0;
    }
    wifi_unlock(); return err;
}

esp_err_t net_wifi_profile_connect(uint32_t id, unsigned delay_ms)
{
    if (!wifi_lock()) { return ESP_ERR_TIMEOUT; }
    if (s_debug_radio_off || s_debug_stop_pending || wifi_profiles_find(&s_manager->store, id) < 0) {
        wifi_unlock(); return ESP_ERR_INVALID_STATE;
    }
    s_apply_id = id; s_apply_at = wifi_now() + (delay_ms < 1500 ? 1500 : delay_ms);
    s_config_until = s_setup_until = 0; s_setup_owner[0] = 0; s_ap_suppressed = false;
    /* Allow this explicit command to supersede an attempt; never reset offline_since. */
    s_request_disconnect = false;
    wifi_unlock(); return ESP_OK;
}

esp_err_t net_wifi_profile_can_connect(uint32_t id)
{
    if (!wifi_lock()) { return ESP_ERR_TIMEOUT; }
    const bool valid = !s_debug_radio_off && !s_debug_stop_pending &&
        wifi_profiles_find(&s_manager->store, id) >= 0;
    wifi_unlock(); return valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t net_wifi_profile_forget(uint32_t id)
{
    if (!wifi_lock()) { return ESP_ERR_TIMEOUT; }
    int index = wifi_profiles_find(&s_manager->store, id);
    if (index < 0 || s_storage_fault) { wifi_unlock(); return ESP_ERR_NOT_FOUND; }
    wifi_profile_store_t next = s_manager->store;
    memset(&next.profiles[index], 0, sizeof(next.profiles[index]));
    esp_err_t err = persist_profiles(&next);
    if (err == ESP_OK) {
        s_manager->store = next;
        memset(&s_manager->policy.candidates[index], 0, sizeof(wifi_candidate_t));
        s_manager->policy.candidates[index].rssi = -128;
        if (s_apply_id == id) { s_apply_id = 0; }
        if (s_manager->policy.active == index) { s_request_disconnect = true; s_disconnect_at = wifi_now()+1500; }
    }
    wifi_unlock(); return err;
}

esp_err_t net_wifi_set_credentials(const char *ssid, const char *password, const char *host)
{
    if (!ssid || !*ssid) { return net_wifi_clear(); }
    uint32_t id = 0;
    esp_err_t err = net_wifi_profile_save(&id, ssid, password ? password : "", true, 0, host);
    return err == ESP_OK ? net_wifi_profile_connect(id, 1500) : err;
}

esp_err_t net_wifi_clear(void)
{
    if (!wifi_lock()) { return ESP_ERR_TIMEOUT; }
    if (s_storage_fault) { wifi_unlock(); return ESP_ERR_INVALID_STATE; }
    wifi_profile_store_t empty = {.version = 1, .next_id = s_manager->store.next_id};
    strcpy(empty.hostname, s_hostname);
    esp_err_t err = persist_profiles(&empty);
    if (err == ESP_OK) {
        s_manager->store = empty; s_apply_id = 0; s_request_ap = true;
        s_config_until = s_setup_until = 0; s_setup_owner[0] = 0; s_ap_suppressed = false;
        wifi_policy_scan_clear(&s_manager->policy);
        /* Remove rollback copies only on explicit forget-all, never during migration. */
        nvs_handle_t h;
        if (nvs_open(NVS_NS_NET, NVS_READWRITE, &h) == ESP_OK) {
            (void)nvs_erase_key(h, "wssid"); (void)nvs_erase_key(h, "wpass"); (void)nvs_commit(h); nvs_close(h);
        }
        if (nvs_open("nvs.net80211", NVS_READWRITE, &h) == ESP_OK) {
            (void)nvs_erase_key(h, "sta"); (void)nvs_commit(h); nvs_close(h);
        }
    }
    wifi_unlock(); return err;
}

esp_err_t net_wifi_setup_ap_start(void)
{
    if (!wifi_lock()) { return ESP_ERR_TIMEOUT; }
    if (s_debug_radio_off || s_debug_stop_pending) { wifi_unlock(); return ESP_ERR_INVALID_STATE; }
    s_request_ap = true; s_ap_suppressed = false; s_request_ap_close = false;
    s_config_until = wifi_now() + 180000; wifi_unlock(); return ESP_OK;
}

esp_err_t net_wifi_setup_session(const char *owner, bool release)
{
    if (!owner || !*owner || strlen(owner) > 64) { return ESP_ERR_INVALID_ARG; }
    for (const unsigned char *p = (const unsigned char *)owner; *p; ++p) {
        if (!isalnum(*p) && *p != '-' && *p != '_') { return ESP_ERR_INVALID_ARG; }
    }
    if (!wifi_lock()) { return ESP_ERR_TIMEOUT; }
    const int64_t now = wifi_now();
    if (!s_ap_on || s_apply_id || s_request_ap_close || s_request_disconnect || s_debug_stop_pending || s_debug_radio_off ||
        (s_setup_owner[0] && strcmp(s_setup_owner, owner) && now < s_setup_until) ||
        (release && strcmp(s_setup_owner, owner))) {
        wifi_unlock(); return ESP_ERR_INVALID_STATE;
    }
    s_config_until = 0;
    if (release) { s_setup_until = 0; s_setup_owner[0] = 0; }
    else { strcpy(s_setup_owner, owner); s_setup_until = now + 60000; }
    wifi_unlock(); return ESP_OK;
}
void net_wifi_setup_ap_stop(void)
{
    if (wifi_lock()) { s_request_ap_close = true; wifi_unlock(); }
}
bool net_wifi_setup_ap_active(void) { return s_ap_on; }
const char *net_wifi_setup_ssid(void) { return s_ap_ssid; }
const char *net_wifi_ap_ip(void) { return s_ap_on ? "192.168.4.1" : ""; }
const char *net_wifi_hostname(void) { return s_hostname; }
bool net_wifi_connected(void) { return s_connected; }
const char *net_wifi_ip(void) { return s_ip; }

esp_err_t net_wifi_debug_radio_off_schedule(void)
{
    if (!wifi_lock()) { return ESP_ERR_TIMEOUT; }
    if (!s_started || s_debug_radio_off || s_debug_stop_pending) { wifi_unlock(); return ESP_ERR_INVALID_STATE; }
    s_debug_stop_pending = true; s_debug_at = wifi_now() + 1500;
    strcpy(s_state, "debug-off-pending"); wifi_unlock(); return ESP_OK;
}

void net_wifi_disc_stats(uint32_t *count, int *reason, int64_t *age_ms)
{
    portENTER_CRITICAL(&s_event_mux);
    if (count) { *count = s_disc_count; }
    if (reason) { *reason = s_disc_reason; }
    if (age_ms) { *age_ms = s_disc_count ? wifi_now() - s_disc_ms : -1; }
    portEXIT_CRITICAL(&s_event_mux);
}

esp_err_t net_wifi_scan_json(char *out, size_t len, bool force)
{
    if (!out || len < 4) { return ESP_ERR_INVALID_ARG; }
    if (!wifi_lock()) { return ESP_ERR_TIMEOUT; }
    if (force || s_last_scan < 0) { s_scan_requested = true; }
    if (strlen(s_manager->scan_json) >= len) { wifi_unlock(); return ESP_ERR_INVALID_SIZE; }
    strcpy(out, s_manager->scan_json); wifi_unlock(); return ESP_OK;
}

esp_err_t net_wifi_profiles_json(char *out, size_t len)
{
    if (!out || len < 7800) { return ESP_ERR_INVALID_ARG; }
    if (!wifi_lock()) { return ESP_ERR_TIMEOUT; }
    const int64_t now = wifi_now();
    wifi_policy_t *p = &s_manager->policy;
    uint32_t active = p->active >= 0 ? s_manager->store.profiles[p->active].id : 0;
    if (p->active >= 0 && strcmp(s_manager->store.profiles[p->active].ssid, s_ssid)) { active = 0; }
    char current_ssid[200]; wifi_escape(s_ssid, current_ssid, sizeof(current_ssid));
    char host_esc[200]; wifi_escape(s_hostname, host_esc, sizeof(host_esc));
    size_t o = (size_t)snprintf(out, len,
        "{\"state\":\"%s\",\"ssid\":\"%s\",\"host\":\"%s\",\"connected\":%s,\"active_id\":%lu,\"pending_id\":%lu,\"ap\":%s,"
        "\"setup_hold_ms\":%lld,\"offline_ms\":%lld,\"scan_running\":%s,\"scan_age_ms\":%lld,\"storage_error\":%s,\"limit\":%d,\"profiles\":[",
        s_state, current_ssid, host_esc, s_connected ? "true" : "false", (unsigned long)active, (unsigned long)s_apply_id,
        s_ap_on ? "true" : "false", s_ap_on && s_setup_until > now ? (long long)(s_setup_until-now) : 0LL,
        p->offline_since < 0 ? 0LL : (long long)(now-p->offline_since),
        s_scan_running || s_scan_requested ? "true" : "false", s_last_scan < 0 ? -1LL : (long long)(now-s_last_scan),
        s_storage_fault ? "true" : "false", WIFI_PROFILE_MAX);
    int emitted = 0;
    for (int i = 0; i < WIFI_PROFILE_MAX; ++i) {
        const wifi_profile_t *n = &s_manager->store.profiles[i];
        if (!n->id) { continue; }
        char name[200]; wifi_escape(n->ssid, name, sizeof(name));
        const wifi_candidate_t *c = &p->candidates[i];
        o += (size_t)snprintf(out+o, len-o,
            "%s{\"id\":%lu,\"ssid\":\"%s\",\"has_password\":%s,\"auto_join\":%s,\"priority\":%d,"
            "\"rssi\":%d,\"failures\":%u,\"retry_in_ms\":%lld}", emitted++ ? "," : "", (unsigned long)n->id,
            name, n->password[0] ? "true" : "false", n->auto_join ? "true" : "false", n->priority, c->rssi,
            c->failures, c->retry_at > now ? (long long)(c->retry_at-now) : 0LL);
    }
    strcpy(out+o, "]}"); wifi_unlock(); return ESP_OK;
}

/* Preferred IPv6 address of the station interface: global first (the one a
 * remote peer can actually reach), then unique-local, then link-local.
 * Empty string when the network has no IPv6. */
static char s_ip6[48];

static bool ip6_usable(const esp_ip6_addr_t *a)
{
    if (!a) { return false; }
    if (a->addr[0] == 0 && a->addr[1] == 0 && a->addr[2] == 0 && a->addr[3] == 0) {
        return false;                 /* unspecified */
    }
    if ((a->addr[0] & 0xFF000000u) == 0xFF000000u) {
        return false;                 /* multicast */
    }
    return true;
}

/* Diagnostic dump: why does this interface have no IPv6?  Reads both the
 * esp_netif view and the raw lwIP netif, because "empty list" can mean either
 * "IPv6 not enabled on the interface" or "no address assigned yet". */
void net_wifi_ip6_debug(const char *why)
{
    if (!s_netif) {
        ESP_LOGW(TAG, "ip6[%s]: no STA netif yet", why);
        return;
    }
    esp_ip6_addr_t ll;
    const esp_err_t lerr = esp_netif_get_ip6_linklocal(s_netif, &ll);
    char llbuf[48] = "-";
    if (lerr == ESP_OK) {
        (void)inet_ntop(AF_INET6, ll.addr, llbuf, sizeof(llbuf));
    }
    ESP_LOGI(TAG, "ip6[%s]: esp_netif linklocal=%s (%s), get_all=%d",
             why, llbuf, esp_err_to_name(lerr), net_wifi_ip6_json(NULL, 0));

    struct netif *n = netif_default;
    if (!n) {
        ESP_LOGW(TAG, "ip6[%s]: no lwIP default netif", why);
        return;
    }
    const uint8_t fl = (uint8_t)n->flags;
    ESP_LOGI(TAG, "ip6[%s]: lwip netif up=%d link_up=%d ip6flags=0x%02x",
             why, (fl & NETIF_FLAG_UP) ? 1 : 0,
             (fl & NETIF_FLAG_LINK_UP) ? 1 : 0,
             (unsigned)(n->ip6_addr_state[0]));
    for (int i = 0; i < LWIP_IPV6_NUM_ADDRESSES; ++i) {
        const ip6_addr_t *a = netif_ip6_addr(n, i);
        if (ip6_addr_isinvalid(a)) {
            ESP_LOGW(TAG, "ip6[%s]: lwip addr[%d] invalid (state %u)", why, i,
                     (unsigned)n->ip6_addr_state[i]);
        } else {
            char b[48] = {0};
            (void)inet_ntop(AF_INET6, a->addr, b, sizeof(b));
            ESP_LOGI(TAG, "ip6[%s]: lwip addr[%d]=%s state=%u", why, i, b,
                     (unsigned)n->ip6_addr_state[i]);
        }
    }
}

/* All IPv6 addresses of the station interface, as a JSON array.  Exposed in
 * /api/status so "why is there no IPv6" has an answer: an empty array means
 * the stack never got an address at all (no RA), a link-local only means the
 * router does not advertise a global prefix. */
int net_wifi_ip6_json(char *out, size_t out_len)
{
    if (!out || out_len < 8) {
        return 0;
    }
    size_t o = 0;
    int n_out = 0;
    o += (size_t)snprintf(out + o, out_len - o, "[");
    if (s_netif) {
        esp_ip6_addr_t list[4];
        const int n = esp_netif_get_all_ip6(s_netif, list);
        for (int i = 0; i < n && o + 48 < out_len; ++i) {
            char a[48] = {0};
            if (!inet_ntop(AF_INET6, list[i].addr, a, sizeof(a))) {
                continue;
            }
            o += (size_t)snprintf(out + o, out_len - o, "%s\"%s\"",
                                  n_out ? "," : "", a);
            n_out++;
        }
    }
    snprintf(out + o, out_len - o, "]");
    return n_out;
}

const char *net_wifi_ip6(void)
{
    s_ip6[0] = '\0';
    if (!s_netif) {
        return s_ip6;
    }
    esp_ip6_addr_t list[4];
    const int n = esp_netif_get_all_ip6(s_netif, list);
    if (n <= 0) {
        static int64_t last_dbg;
        const int64_t now = esp_timer_get_time() / 1000;
        if (now - last_dbg > 10000) {          /* at most once per 10 s */
            last_dbg = now;
            net_wifi_ip6_debug("status");
        }
        return s_ip6;
    }
    int global = -1, ula = -1, linklocal = -1;
    for (int i = 0; i < n; ++i) {
        if (!ip6_usable(&list[i])) { continue; }
        const uint32_t f = ntohl(list[i].addr[0]);   /* bytes are network order */
        if ((f & 0xFE000000u) == 0xFC000000u) {          /* fc00::/7 */
            if (ula < 0) { ula = i; }
        } else if ((f & 0xFFC00000u) == 0xFE800000u) {   /* fe80::/10 */
            if (linklocal < 0) { linklocal = i; }
        } else if (global < 0) {
            global = i;
        }
    }
    const int pick = (global >= 0) ? global : ((ula >= 0) ? ula : linklocal);
    if (pick < 0) {
        return s_ip6;
    }
    if (!inet_ntop(AF_INET6, list[pick].addr, s_ip6, sizeof(s_ip6))) {
        s_ip6[0] = '\0';
    }
    return s_ip6;
}


const char *net_wifi_ssid(void) { return s_ssid; }

const char *net_wifi_state_name(void) { return s_state; }

int net_wifi_rssi_dbm(void)
{
    if (!s_connected) {
        return 0;
    }
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return 0;
    }
    s_rssi = ap.rssi;
    return s_rssi;
}
