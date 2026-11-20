#pragma once
#include <stddef.h>
#include "esp_err.h"
#include "maintenance_policy.h"
esp_err_t device_maintenance_init(void);
void device_maintenance_tick(void);
void device_maintenance_config(maintenance_config_t *out);
esp_err_t device_maintenance_set(const maintenance_config_t *cfg, uint32_t expected);
esp_err_t device_maintenance_cleanup(void);
esp_err_t device_maintenance_reboot(void);
esp_err_t device_maintenance_factory_reset(void);
bool device_maintenance_reboot_pending(void);
bool device_maintenance_writing(void);
/* Track non-HTTP file writers too; new writes cannot start after reboot arm. */
bool device_maintenance_write_begin(void);
bool device_maintenance_update_idle(void);
bool device_maintenance_update_begin(void);
void device_maintenance_update_end(bool reboot);
void device_maintenance_write_end(void);
void device_maintenance_json(char *out, size_t capacity);
