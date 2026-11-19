#include "release_manifest.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "mbedtls/base64.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"

extern const uint8_t release_pub_start[] asm("_binary_release_update_public_pem_start");
extern const uint8_t release_pub_end[] asm("_binary_release_update_public_pem_end");

static const char *string_field(const cJSON *data, const char *key)
{
    const cJSON *field = cJSON_GetObjectItemCaseSensitive(data, key);
    return cJSON_IsString(field) ? field->valuestring : NULL;
}

static bool uint_field(const cJSON *data, const char *key, uint32_t max, uint32_t *out)
{
    const cJSON *field = cJSON_GetObjectItemCaseSensitive(data, key);
    if (!cJSON_IsNumber(field) || !isfinite(field->valuedouble) ||
        field->valuedouble < 1 || field->valuedouble > max ||
        floor(field->valuedouble) != field->valuedouble) { return false; }
    *out = (uint32_t)field->valuedouble;
    return true;
}

static bool safe_name(const char *value, size_t capacity)
{
    if (!value) { return false; }
    size_t length = strlen(value);
    if (!length || length >= capacity) { return false; }
    for (size_t i = 0; i < length; ++i) {
        char c = value[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (i && (c == '.' || c == '_' || c == '-')))) { return false; }
    }
    return true;
}

static esp_err_t read_firmware(const cJSON *data, uint32_t installed, release_firmware_t *out)
{
    uint32_t schema;
    if (!uint_field(data, "schema", 1, &schema)) { return ESP_ERR_INVALID_VERSION; }
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(data, "firmware");
    if (!item) { return ESP_ERR_NOT_FOUND; }
    const char *target = string_field(item, "target");
    if (!cJSON_IsObject(item) || !target) { return ESP_ERR_INVALID_ARG; }
    if (strcmp(target, "heimao-esp32s3-n16r8")) { return ESP_ERR_NOT_FOUND; }
    uint32_t code, size;
    if (!uint_field(item, "code", INT32_MAX, &code)) { return ESP_ERR_INVALID_VERSION; }
    if (code <= installed) { return ESP_ERR_NOT_FOUND; }
    const char *tag = string_field(item, "tag");
    const char *file = string_field(item, "file");
    const char *hash = string_field(item, "sha256");
    const char *version = string_field(item, "version");
    const char *notes = string_field(data, "notes");
    const cJSON *force = cJSON_GetObjectItemCaseSensitive(item, "force");
    char expected[24];
    snprintf(expected, sizeof(expected), "r11-%lu", (unsigned long)code);
    if (!version || strcmp(version, expected) || !safe_name(tag, sizeof(out->tag)) ||
        !safe_name(file, sizeof(out->file)) || strlen(file) < 5 || strcmp(file + strlen(file) - 4, ".bin") ||
        !hash || strlen(hash) != 64 || strspn(hash, "0123456789abcdef") != 64 ||
        !uint_field(item, "size", RELEASE_FIRMWARE_SIZE_MAX, &size) || size < 0x10000u ||
        (force && !cJSON_IsBool(force)) || (notes && strlen(notes) > RELEASE_NOTES_MAX)) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = (release_firmware_t){.code=code, .size=size, .force=cJSON_IsTrue(force)};
    strcpy(out->tag, tag);
    strcpy(out->file, file);
    strcpy(out->hash, hash);
    if (notes) { strcpy(out->notes, notes); }
    return ESP_OK;
}

esp_err_t release_manifest_read(const char *bytes, size_t length, uint32_t installed, release_firmware_t *out)
{
    if (length > RELEASE_MANIFEST_MAX || !length || memchr(bytes, '\0', length)) { return ESP_ERR_INVALID_SIZE; }
    cJSON *envelope = cJSON_ParseWithLengthOpts(bytes, length + 1, NULL, true);
    const char *encoded = string_field(envelope, "payload");
    const char *sig = string_field(envelope, "signature");
    uint8_t *payload = malloc(4097);
    uint8_t signature[80], digest[32];
    size_t payload_length = 0, sig_length = 0;
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (!payload) { err = ESP_ERR_NO_MEM; }
    else if (encoded && sig && !mbedtls_base64_decode(payload, 4096, &payload_length,
                (const uint8_t *)encoded, strlen(encoded)) &&
             !mbedtls_base64_decode(signature, sizeof(signature), &sig_length,
                (const uint8_t *)sig, strlen(sig)) &&
             payload_length && sig_length >= 64 && !memchr(payload, '\0', payload_length) &&
             !mbedtls_sha256(payload, payload_length, digest, 0)) {
        mbedtls_pk_context key;
        mbedtls_pk_init(&key);
        if (!mbedtls_pk_parse_public_key(&key, release_pub_start, release_pub_end - release_pub_start) &&
            !mbedtls_pk_verify(&key, MBEDTLS_MD_SHA256, digest, sizeof(digest), signature, sig_length)) {
            payload[payload_length] = 0;
            cJSON *data = cJSON_ParseWithLengthOpts((char *)payload, payload_length + 1, NULL, true);
            err = read_firmware(data, installed, out);
            cJSON_Delete(data);
        } else {
            err = ESP_ERR_INVALID_CRC;
        }
        mbedtls_pk_free(&key);
    }
    free(payload);
    cJSON_Delete(envelope);
    return err;
}
