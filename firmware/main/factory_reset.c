#include "factory_reset.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "esp_flash.h"
#include "esp_flash_encrypt.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* This sector is outside every partition on the supported r11 layout. A
 * journal here lets OTA-installed devices resume after power loss without
 * requiring a new partition table or keeping a flag in the data being erased. */
#define FLASH_SIZE 0x1000000u
#define JOURNAL_SIZE 0x1000u
#define JOURNAL_ADDR (FLASH_SIZE - JOURNAL_SIZE)
#define JOURNAL_MAGIC 0x484d5232u
#define JOURNAL_SEAL 0x7a6bd491u

typedef struct {
    uint32_t magic, boot_address, inverse_address, seal;
} reset_journal_t;

typedef struct {
    const char *label;
    esp_partition_type_t type;
    esp_partition_subtype_t subtype;
    uint32_t address, size;
} layout_entry_t;

enum { NVS, OTA_DATA, PHY, OTA_0, OTA_1, STORAGE, CORE_DUMP, PARTITION_COUNT };

static const layout_entry_t s_expected[PARTITION_COUNT] = {
    [NVS]       = {"nvs", ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, 0x9000, 0x6000},
    [OTA_DATA]  = {"otadata", ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, 0xf000, 0x2000},
    [PHY]       = {"phy_init", ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_PHY, 0x11000, 0x1000},
    [OTA_0]     = {"ota_0", ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, 0x20000, 0x400000},
    [OTA_1]     = {"ota_1", ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, 0x420000, 0x400000},
    [STORAGE]   = {"storage", ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, 0x820000, 0x100000},
    [CORE_DUMP] = {"coredump", ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, 0x920000, 0x40000},
};

static const char *TAG = "factory_reset";

static bool same_position(const esp_partition_t *a, const esp_partition_t *b)
{
    return a && b && a->address == b->address && a->flash_chip == b->flash_chip;
}

static esp_err_t find_layout(const esp_partition_t *parts[PARTITION_COUNT])
{
    uint32_t size = 0;
    esp_err_t err = esp_flash_get_size(NULL, &size);
    if (err != ESP_OK) { return err; }
    if (size != FLASH_SIZE || esp_flash_encryption_enabled()) { return ESP_ERR_NOT_SUPPORTED; }

    unsigned count = 0;
    for (esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
         it; it = esp_partition_next(it)) {
        ++count;
    }
    if (count != PARTITION_COUNT) { return ESP_ERR_NOT_SUPPORTED; }
    for (unsigned i = 0; i < PARTITION_COUNT; ++i) {
        const layout_entry_t *expected = &s_expected[i];
        const esp_partition_t *part = esp_partition_find_first(expected->type, expected->subtype, expected->label);
        if (!part || part->address != expected->address || part->size != expected->size ||
            part->address + part->size > JOURNAL_ADDR || part->flash_chip != esp_flash_default_chip ||
            part->encrypted) {
            return ESP_ERR_NOT_SUPPORTED;
        }
        parts[i] = part;
    }
    return ESP_OK;
}

static bool journal_valid(const reset_journal_t *journal)
{
    return journal->magic == JOURNAL_MAGIC && journal->seal == JOURNAL_SEAL &&
        journal->inverse_address == ~journal->boot_address &&
        (journal->boot_address == s_expected[OTA_0].address || journal->boot_address == s_expected[OTA_1].address);
}

static esp_err_t verify_boot_target(const esp_partition_t *running)
{
    if (!same_position(running, esp_ota_get_boot_partition())) {
        /* The OTA API validates the image before changing the selector. */
        return esp_ota_set_boot_partition(running);
    }
    const esp_partition_pos_t pos = {.offset = running->address, .size = running->size};
    esp_image_metadata_t image = {0};
    return esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &image);
}

esp_err_t factory_reset_prepare(void)
{
    const esp_partition_t *parts[PARTITION_COUNT];
    esp_err_t err = find_layout(parts);
    if (err != ESP_OK) { return err; }
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!same_position(running, parts[OTA_0]) && !same_position(running, parts[OTA_1])) {
        return ESP_ERR_INVALID_STATE;
    }
    err = verify_boot_target(running);
    if (err != ESP_OK) { return err; }

    const reset_journal_t journal = {
        .magic = JOURNAL_MAGIC, .boot_address = running->address,
        .inverse_address = ~running->address, .seal = JOURNAL_SEAL,
    };
    reset_journal_t persisted;
    err = esp_flash_read(NULL, &persisted, JOURNAL_ADDR, sizeof(persisted));
    if (err != ESP_OK) { return err; }
    if (journal_valid(&persisted)) {
        return memcmp(&journal, &persisted, sizeof(journal)) ? ESP_ERR_INVALID_STATE : ESP_OK;
    }
    err = esp_flash_erase_region(NULL, JOURNAL_ADDR, JOURNAL_SIZE);
    if (err == ESP_OK) { err = esp_flash_write(NULL, &journal, JOURNAL_ADDR, sizeof(journal)); }
    if (err == ESP_OK) { err = esp_flash_read(NULL, &persisted, JOURNAL_ADDR, sizeof(persisted)); }
    if (err != ESP_OK) { return err; }
    return memcmp(&journal, &persisted, sizeof(journal)) ? ESP_FAIL : ESP_OK;
}

static esp_err_t erase_and_verify(const esp_partition_t *part)
{
    ESP_LOGI(TAG, "erasing %s", part->label);
    esp_err_t err = esp_partition_erase_range(part, 0, part->size);
    if (err != ESP_OK) { return err; }
    uint32_t words[256];
    for (size_t offset = 0; offset < part->size; offset += sizeof(words)) {
        err = esp_partition_read_raw(part, offset, words, sizeof(words));
        if (err != ESP_OK) { return err; }
        for (unsigned i = 0; i < sizeof(words) / sizeof(words[0]); ++i) {
            if (words[i] != UINT32_MAX) { return ESP_FAIL; }
        }
        if ((offset & 0xffffu) == 0) { vTaskDelay(1); }
    }
    return ESP_OK;
}

esp_err_t factory_reset_apply_pending(void)
{
    /* On other flash capacities the reserved address has no meaning. */
    uint32_t size = 0;
    esp_err_t err = esp_flash_get_size(NULL, &size);
    if (err != ESP_OK) { return err; }
    if (size != FLASH_SIZE || esp_flash_encryption_enabled()) { return ESP_OK; }

    reset_journal_t journal;
    err = esp_flash_read(NULL, &journal, JOURNAL_ADDR, sizeof(journal));
    if (err != ESP_OK) { return err; }
    /* A torn journal write cannot follow any data erases. A torn journal
     * removal is possible only after all data erases have been verified. */
    if (!journal_valid(&journal)) { return ESP_OK; }

    const esp_partition_t *parts[PARTITION_COUNT];
    err = find_layout(parts);
    if (err != ESP_OK) { return err; }
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running || running->address != journal.boot_address ||
        !same_position(running, esp_ota_get_boot_partition())) {
        return ESP_ERR_INVALID_STATE;
    }
    const unsigned spare = same_position(running, parts[OTA_0]) ? OTA_1 : OTA_0;
    const unsigned targets[] = {STORAGE, CORE_DUMP, PHY, NVS, spare};
    for (unsigned i = 0; i < sizeof(targets) / sizeof(targets[0]); ++i) {
        err = erase_and_verify(parts[targets[i]]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "%s erase/verify failed: %s", parts[targets[i]]->label, esp_err_to_name(err));
            return err;
        }
    }
    err = esp_flash_erase_region(NULL, JOURNAL_ADDR, JOURNAL_SIZE);
    if (err == ESP_OK) { err = esp_flash_read(NULL, &journal, JOURNAL_ADDR, sizeof(journal)); }
    if (err != ESP_OK) { return err; }
    if (journal.magic != UINT32_MAX || journal.seal != UINT32_MAX ||
        journal.boot_address != UINT32_MAX || journal.inverse_address != UINT32_MAX) {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "factory reset verified, starting initial setup");
    return ESP_OK;
}
