#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* Initialise before the front-panel/network tasks. Indices are 1-based. */
void channel_control_init(void);
/* Match the acknowledged profile, never a separate UI cursor; 0 = custom/not ready. */
int channel_current(void);
/* One application path for keys, presets and bridge selection. Zero only clears
 * a bridge selection. A repeated selection deliberately re-applies the profile. */
esp_err_t channel_select(int index, bool bind_bridge, char *log, size_t log_len);
