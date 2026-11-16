#include "maintenance_policy.h"
#include <string.h>
bool maintenance_config_valid(const maintenance_config_t *cfg)
{
    return cfg && cfg->version == 1 && cfg->restart_hours >= 1 && cfg->restart_hours <= 168 &&
        cfg->cleanup_enabled <= 1 && cfg->restart_enabled <= 1;
}
void maintenance_policy_reset(maintenance_policy_t *p, const maintenance_config_t *cfg, int64_t now)
{
    memset(p, 0, sizeof(*p));
    p->idle_since_ms = -1;
    p->next_cleanup_ms = now + MAINTENANCE_CLEANUP_MS;
    p->next_restart_ms = cfg->restart_enabled ? now + (int64_t)cfg->restart_hours * 3600000LL : 0;
}
int maintenance_policy_tick(maintenance_policy_t *p, const maintenance_config_t *cfg,
                            int64_t now, bool busy, bool manual_cleanup)
{
    if (busy) { p->idle_since_ms = -1; }
    else if (p->idle_since_ms < 0) { p->idle_since_ms = now; }
    p->restart_waiting = cfg->restart_enabled && p->next_restart_ms && now >= p->next_restart_ms;
    const bool idle = !busy && p->idle_since_ms >= 0 && now - p->idle_since_ms >= MAINTENANCE_IDLE_MS;
    if (p->restart_waiting && idle) { return MAINTENANCE_RESTART; }
    if ((manual_cleanup || (cfg->cleanup_enabled && now >= p->next_cleanup_ms)) && idle) {
        p->next_cleanup_ms = now + MAINTENANCE_CLEANUP_MS;
        return MAINTENANCE_CLEANUP;
    }
    return MAINTENANCE_NONE;
}
