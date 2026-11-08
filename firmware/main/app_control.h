#pragma once
#include "esp_http_server.h"
#include <stdbool.h>
#include <stdint.h>
esp_err_t app_control_init(void (*revoke)(void));
esp_err_t app_control_http(httpd_req_t *req);
/* Hold through the entire mutation, including streaming uploads. This prevents
 * a request accepted under an old lease committing after a takeover. */
bool app_control_begin(httpd_req_t *req, bool websocket);
bool app_control_generation_begin(uint32_t generation);
uint32_t app_control_generation(void); /* caller holds the lock */
void app_control_end(void);
/* Strict bounded JSON body, owned by caller. */
struct cJSON;
struct cJSON *app_read_json(httpd_req_t *req);
