#pragma once
#include "esp_err.h"

/* Prepare under the common reboot/write gate; apply before any data mounts. */
esp_err_t factory_reset_prepare(void);
esp_err_t factory_reset_apply_pending(void);
