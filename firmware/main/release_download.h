#pragma once

#include <stddef.h>
#include "esp_err.h"

/* Reads an official asset into preallocated memory, requiring the exact size
 * when expected is nonzero. The destination holds at most capacity bytes. */
esp_err_t release_download(const char *url, void *buffer, size_t capacity, size_t expected, size_t *received);
