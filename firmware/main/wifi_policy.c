#include "wifi_policy.h"
#include <string.h>
#include <limits.h>

void wifi_policy_init(wifi_policy_t *p, int64_t now)
{
    memset(p, 0, sizeof(*p));
    p->offline_since = now;
    p->active = -1;
    wifi_policy_scan_clear(p);
}

void wifi_policy_offline(wifi_policy_t *p, int64_t now)
{
    if (p->offline_since < 0) { p->offline_since = now; }
}

bool wifi_policy_ap_due(const wifi_policy_t *p, int64_t now)
{
    return p->offline_since >= 0 && now - p->offline_since >= WIFI_RECOVERY_MS;
}

void wifi_policy_begin(wifi_policy_t *p, int index, int64_t now)
{
    wifi_policy_offline(p, now);
    p->active = index;
    p->attempt_until = now + WIFI_ATTEMPT_MS;
    /* Once AP recovery has been reached, later attempts have their own limit.
     * Before then no last-minute attempt can move the original AP deadline. */
    if (!p->recovery_ap && p->attempt_until > p->offline_since + WIFI_RECOVERY_MS) {
        p->attempt_until = p->offline_since + WIFI_RECOVERY_MS;
    }
}

void wifi_policy_failed(wifi_policy_t *p, int64_t now)
{
    if (p->active >= 0 && p->active < WIFI_PROFILE_MAX) {
        wifi_candidate_t *c = &p->candidates[p->active];
        if (c->failures < UINT16_MAX) { ++c->failures; }
        const int shift = c->failures > 3 ? 2 : c->failures - 1;
        c->retry_at = now + (30000 << shift);
    }
    p->active = -1;
    p->attempt_until = 0;
    wifi_policy_offline(p, now);
}

void wifi_policy_online(wifi_policy_t *p, int64_t now)
{
    if (p->active >= 0 && p->active < WIFI_PROFILE_MAX) {
        wifi_candidate_t *c = &p->candidates[p->active];
        c->last_success = now + 1;
        c->failures = 0;
        c->retry_at = 0;
    }
    p->offline_since = -1;
    p->attempt_until = 0;
    p->recovery_ap = false;
}

void wifi_policy_scan_clear(wifi_policy_t *p)
{
    for (int i = 0; i < WIFI_PROFILE_MAX; ++i) { p->candidates[i].rssi = -128; }
}

void wifi_policy_seen(wifi_policy_t *p, int index, int rssi)
{
    if (index >= 0 && index < WIFI_PROFILE_MAX && rssi > p->candidates[index].rssi) {
        p->candidates[index].rssi = rssi;
    }
}

int wifi_policy_pick(const wifi_policy_t *p, const wifi_profile_store_t *s, int64_t now)
{
    int best = -1, best_score = INT_MIN;
    for (int i = 0; i < WIFI_PROFILE_MAX; ++i) {
        const wifi_profile_t *n = &s->profiles[i];
        const wifi_candidate_t *c = &p->candidates[i];
        if (!n->id || !n->auto_join || c->rssi <= -128 || c->retry_at > now) { continue; }
        /* User priority dominates; history breaks close signals, it does not
         * permanently prefer the most recently disconnected SSID. */
        const int score = (int)n->priority * 256 + c->rssi +
                         (c->last_success > 0 ? 5 : 0) - (c->failures > 4 ? 20 : c->failures * 5);
        if (best < 0 || score > best_score || (score == best_score && n->id < s->profiles[best].id)) {
            best = i;
            best_score = score;
        }
    }
    return best;
}

int wifi_profiles_find(const wifi_profile_store_t *s, uint32_t id)
{
    if (id) {
        for (int i = 0; i < WIFI_PROFILE_MAX; ++i) {
            if (s->profiles[i].id == id) { return i; }
        }
    }
    return -1;
}

bool wifi_profiles_valid(const wifi_profile_store_t *s)
{
    if (s->version != 1 || !s->next_id || !memchr(s->hostname, 0, sizeof(s->hostname))) { return false; }
    for (int i = 0; i < WIFI_PROFILE_MAX; ++i) {
        const wifi_profile_t *n = &s->profiles[i];
        if (!n->id) { continue; }
        if (!n->ssid[0] || !memchr(n->ssid, 0, sizeof(n->ssid)) ||
            !memchr(n->password, 0, sizeof(n->password)) || n->auto_join > 1 || n->id >= s->next_id) {
            return false;
        }
        for (int j = 0; j < i; ++j) {
            if (s->profiles[j].id == n->id || (s->profiles[j].id && !strcmp(s->profiles[j].ssid, n->ssid))) {
                return false;
            }
        }
    }
    return true;
}
