#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"

bool release_update_watch_boot(void);
void release_update_start(bool boot_healthy);
esp_err_t release_update_http(httpd_req_t *req);
