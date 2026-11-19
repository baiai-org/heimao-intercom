#pragma once
#include <stdbool.h>
#include <stdint.h>

#define MAINTENANCE_IDLE_MS 60000LL
#define MAINTENANCE_CLEANUP_MS 300000LL
#define MAINTENANCE_WEEKDAYS 0x7fu
#define MAINTENANCE_MINUTES_PER_DAY 1440

typedef struct {
    uint32_t version, revision;
    uint16_t restart_minute;
    uint8_t cleanup_enabled, restart_enabled, restart_days;
    uint8_t reserved[3];
} maintenance_config_t;

typedef struct {
    int64_t idle_since_ms, next_cleanup_ms;
} maintenance_policy_t;

bool maintenance_config_valid(const maintenance_config_t *cfg);
void maintenance_policy_reset(maintenance_policy_t *policy, int64_t now);
bool maintenance_policy_cleanup_due(maintenance_policy_t *policy, const maintenance_config_t *cfg,
                                    int64_t now, bool busy);
bool maintenance_restart_due(const maintenance_config_t *cfg, int64_t previous_utc,
                             int64_t utc, int64_t elapsed_ms, int64_t last_slot);
int64_t maintenance_next_restart(const maintenance_config_t *cfg, int64_t utc, int64_t last_slot);
