#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define RELEASE_ROOT "https://github.com/baiai-org/heimao-intercom/releases/"
#define RELEASE_MANIFEST_URL RELEASE_ROOT "latest/download/update.json"
#define RELEASE_MANIFEST_MAX 6144
#define RELEASE_FIRMWARE_SIZE_MAX 0x400000u
#define RELEASE_NOTES_MAX 512

typedef struct {
    uint32_t code, size;
    bool force;
    char tag[96], file[96], hash[65];
    char notes[RELEASE_NOTES_MAX + 1];
} release_firmware_t;

esp_err_t release_manifest_read(const char *bytes, size_t length, uint32_t installed, release_firmware_t *out);
