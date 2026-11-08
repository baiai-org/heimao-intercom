/* Deterministic saved-network policy. No driver calls, wall clock or WAN test. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WIFI_PROFILE_MAX 16
#define WIFI_RECOVERY_MS 60000
#define WIFI_ATTEMPT_MS 20000
#define WIFI_SCAN_INTERVAL_MS 10000

typedef struct {
    uint32_t id;
    char ssid[33];
    char password[65];
    int16_t priority;
    uint8_t auto_join;
    uint8_t reserved;
} wifi_profile_t;

typedef struct {
    uint32_t version;
    uint32_t next_id;
    char hostname[64];
    wifi_profile_t profiles[WIFI_PROFILE_MAX];
} wifi_profile_store_t;

typedef struct {
    int64_t retry_at;
    int64_t last_success;
    uint16_t failures;
    int16_t rssi;
} wifi_candidate_t;

typedef struct {
    wifi_candidate_t candidates[WIFI_PROFILE_MAX];
    int64_t offline_since; /* -1 while online; zero is a valid boot time */
    int64_t attempt_until;
    int active;           /* profile index; -1 when not attempting/online */
    bool recovery_ap;     /* latched until a candidate obtains an IP */
} wifi_policy_t;

void wifi_policy_init(wifi_policy_t *p, int64_t now);
void wifi_policy_offline(wifi_policy_t *p, int64_t now);
bool wifi_policy_ap_due(const wifi_policy_t *p, int64_t now);
void wifi_policy_begin(wifi_policy_t *p, int index, int64_t now);
void wifi_policy_failed(wifi_policy_t *p, int64_t now);
void wifi_policy_online(wifi_policy_t *p, int64_t now);
void wifi_policy_scan_clear(wifi_policy_t *p);
void wifi_policy_seen(wifi_policy_t *p, int index, int rssi);
int wifi_policy_pick(const wifi_policy_t *p, const wifi_profile_store_t *s, int64_t now);
bool wifi_profiles_valid(const wifi_profile_store_t *s);
int wifi_profiles_find(const wifi_profile_store_t *s, uint32_t id);
