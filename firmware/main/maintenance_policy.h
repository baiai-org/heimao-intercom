#pragma once
#include <stdbool.h>
#include <stdint.h>
#define MAINTENANCE_IDLE_MS 60000LL
#define MAINTENANCE_CLEANUP_MS 300000LL
typedef struct {
    uint32_t version, revision;
    uint16_t restart_hours;
    uint8_t cleanup_enabled, restart_enabled;
} maintenance_config_t;
typedef struct {
    int64_t idle_since_ms, next_cleanup_ms, next_restart_ms;
    bool restart_waiting;
} maintenance_policy_t;
enum { MAINTENANCE_NONE, MAINTENANCE_CLEANUP, MAINTENANCE_RESTART };
bool maintenance_config_valid(const maintenance_config_t *cfg);
void maintenance_policy_reset(maintenance_policy_t *p, const maintenance_config_t *cfg, int64_t now);
int maintenance_policy_tick(maintenance_policy_t *p, const maintenance_config_t *cfg,
                            int64_t now, bool busy, bool manual_cleanup);
