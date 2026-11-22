#pragma once
#include "esp_http_server.h"
esp_err_t maintenance_http(httpd_req_t *req);
esp_err_t peer_labels_http(httpd_req_t *req);
esp_err_t broker_http(httpd_req_t *req);
