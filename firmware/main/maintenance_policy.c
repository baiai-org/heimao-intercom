#include "maintenance_policy.h"

#define CHINA_UTC_OFFSET_MINUTES 480
#define SECONDS_PER_MINUTE 60
#define DAYS_PER_WEEK 7

bool maintenance_config_valid(const maintenance_config_t *cfg)
{
    return cfg && cfg->version == 2 && cfg->cleanup_enabled <= 1 && cfg->restart_enabled <= 1 &&
        cfg->restart_minute < MAINTENANCE_MINUTES_PER_DAY && !(cfg->restart_days & ~MAINTENANCE_WEEKDAYS) &&
        (!cfg->restart_enabled || cfg->restart_days);
}

void maintenance_policy_reset(maintenance_policy_t *policy, int64_t now)
{
    policy->idle_since_ms = -1;
    policy->next_cleanup_ms = now + MAINTENANCE_CLEANUP_MS;
}

bool maintenance_policy_cleanup_due(maintenance_policy_t *policy, const maintenance_config_t *cfg,
                                    int64_t now, bool busy)
{
    if (busy) { policy->idle_since_ms = -1; }
    else if (policy->idle_since_ms < 0) { policy->idle_since_ms = now; }
    if (!cfg->cleanup_enabled || now < policy->next_cleanup_ms ||
        policy->idle_since_ms < 0 || now - policy->idle_since_ms < MAINTENANCE_IDLE_MS) {
        return false;
    }
    policy->next_cleanup_ms = now + MAINTENANCE_CLEANUP_MS;
    return true;
}

static bool scheduled_on(const maintenance_config_t *cfg, int64_t local_day)
{
    /* 1970-01-01 was Thursday. Bit zero is Monday. */
    const unsigned weekday = (unsigned)((local_day + 3) % DAYS_PER_WEEK);
    return cfg->restart_days & (1u << weekday);
}

bool maintenance_restart_due(const maintenance_config_t *cfg, int64_t previous_utc,
                             int64_t utc, int64_t elapsed_ms, int64_t last_slot)
{
    if (!cfg->restart_enabled || previous_utc <= 0 || utc <= previous_utc || elapsed_ms < 0) {
        return false;
    }
    const int64_t slot = utc / SECONDS_PER_MINUTE;
    const int64_t local = slot + CHINA_UTC_OFFSET_MINUTES;
    const int64_t clock_step = utc - previous_utc - elapsed_ms / 1000;
    /* Only a live crossing counts. A newly acquired clock, a large SNTP
     * correction or an already handled slot must never trigger a catch-up. */
    return slot > last_slot && slot > previous_utc / SECONDS_PER_MINUTE &&
        clock_step >= -3 && clock_step <= 3 &&
        local % MAINTENANCE_MINUTES_PER_DAY == cfg->restart_minute &&
        scheduled_on(cfg, local / MAINTENANCE_MINUTES_PER_DAY);
}

int64_t maintenance_next_restart(const maintenance_config_t *cfg, int64_t utc, int64_t last_slot)
{
    if (!cfg->restart_enabled || !cfg->restart_days || utc <= 0) { return 0; }
    int64_t minute = utc / SECONDS_PER_MINUTE;
    if (minute < last_slot) { minute = last_slot; }
    minute += CHINA_UTC_OFFSET_MINUTES;
    const int64_t day = minute / MAINTENANCE_MINUTES_PER_DAY;
    for (int offset = 0; offset <= DAYS_PER_WEEK; ++offset) {
        const int64_t candidate = (day + offset) * MAINTENANCE_MINUTES_PER_DAY + cfg->restart_minute;
        if (candidate > minute && scheduled_on(cfg, day + offset)) {
            return (candidate - CHINA_UTC_OFFSET_MINUTES) * SECONDS_PER_MINUTE;
        }
    }
    return 0;
}
