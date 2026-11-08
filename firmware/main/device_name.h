#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define DEVICE_NAME_BYTES 96
typedef struct {
    uint32_t format;
    uint32_t revision;
    char name[DEVICE_NAME_BYTES + 1];
} device_name_t;

bool device_name_validate(const char *name);
esp_err_t device_name_init(void);
void device_name_get(device_name_t *out);
/* Compare-and-set; no network or radio reconfiguration. */
esp_err_t device_name_set(const char *name, uint32_t expected, device_name_t *out);
