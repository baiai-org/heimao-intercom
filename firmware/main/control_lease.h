#pragma once
#include <stdbool.h>
#include <stdint.h>
#define CONTROL_LEASE_MS 15000
/* A hold taken by an explicit user action lasts longer, so a phone that dies
 * mid-session does not hand the board to an automatic client for a few seconds
 * and then take it back (the ping-pong this replaces). */
#define CONTROL_MANUAL_MS 60000
#define CONTROL_TOKEN_LEN 64
typedef struct {
    char token[CONTROL_TOKEN_LEN + 1];
    char client[65];
    char label[65];
    uint32_t generation;
    int64_t expires_at;
    bool held;
    bool manual;   /* taken by a user action; automatic takeover is refused */
} control_lease_t;
bool control_lease_valid(const control_lease_t *s, const char *token, int64_t now);
bool control_lease_expired(const control_lease_t *s, int64_t now);
bool control_lease_grant(control_lease_t *s, const char *client, const char *label,
                         const char *token, int64_t now);
void control_lease_release(control_lease_t *s);
