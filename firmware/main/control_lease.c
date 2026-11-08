#include "control_lease.h"
#include <string.h>
#include <stdio.h>
bool control_lease_valid(const control_lease_t *s, const char *token, int64_t now)
{
    if (!s->held || !token || strlen(token) != CONTROL_TOKEN_LEN || now >= s->expires_at) { return false; }
    unsigned difference = 0;
    for (int i = 0; i < CONTROL_TOKEN_LEN; ++i) { difference |= (unsigned char)(s->token[i] ^ token[i]); }
    return difference == 0;
}
bool control_lease_expired(const control_lease_t *s, int64_t now) { return s->held && now >= s->expires_at; }
bool control_lease_grant(control_lease_t *s, const char *client, const char *label, const char *token, int64_t now)
{
    if (!client || !*client || strlen(client) > 64 || !label || strlen(label) > 64 ||
        !token || strlen(token) != CONTROL_TOKEN_LEN || s->generation == UINT32_MAX || s->held) { return false; }
    ++s->generation; s->held = true; s->manual = false; s->expires_at = now + CONTROL_LEASE_MS;
    snprintf(s->client, sizeof(s->client), "%s", client);
    snprintf(s->label, sizeof(s->label), "%s", label);
    memcpy(s->token, token, sizeof(s->token)); return true;
}
void control_lease_release(control_lease_t *s)
{
    s->held = false; s->manual = false; s->expires_at = 0;
    memset(s->token, 0, sizeof(s->token)); s->client[0] = s->label[0] = 0;
}
