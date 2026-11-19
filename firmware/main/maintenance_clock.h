#pragma once
#include <stdint.h>

typedef struct {
    int64_t utc;
    int64_t last_sync_utc;
    const char *state;
} maintenance_clock_t;

void maintenance_clock_network_ready(void);
maintenance_clock_t maintenance_clock_get(void);
