#pragma once

#include "esp_http_server.h"

/* The HTTP dispatcher serializes mutations across HTTP and HTTPS. */
esp_err_t eq_presets_http(httpd_req_t *req);
