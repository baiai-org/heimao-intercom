"""Run the production reset journal against an emulated 16 MiB NOR flash."""
from test_device_maintenance import plain, run

STUBS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ESP_OK 0
#define ESP_FAIL 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_NOT_SUPPORTED 3
#define ESP_PARTITION_TYPE_APP 0
#define ESP_PARTITION_TYPE_DATA 1
#define ESP_PARTITION_TYPE_ANY 255
#define ESP_PARTITION_SUBTYPE_ANY 255
#define ESP_PARTITION_SUBTYPE_DATA_OTA 0
#define ESP_PARTITION_SUBTYPE_DATA_PHY 1
#define ESP_PARTITION_SUBTYPE_DATA_NVS 2
#define ESP_PARTITION_SUBTYPE_DATA_COREDUMP 3
#define ESP_PARTITION_SUBTYPE_DATA_SPIFFS 130
#define ESP_PARTITION_SUBTYPE_APP_OTA_0 16
#define ESP_PARTITION_SUBTYPE_APP_OTA_1 17
#define ESP_IMAGE_VERIFY_SILENT 1
#define MiB_16 0x1000000u
#define FLASH_JOURNAL (MiB_16 - 0x1000)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)

typedef int esp_err_t, esp_partition_type_t, esp_partition_subtype_t;
typedef unsigned esp_partition_iterator_t;
typedef struct {uint32_t offset,size;} esp_partition_pos_t;
typedef struct {uint32_t unused;} esp_image_metadata_t;
typedef struct {
    int type,subtype;
    uint32_t address,size;
    const char *label;
    const void *flash_chip;
    bool encrypted;
} esp_partition_t;

static int chip;
static const void *esp_flash_default_chip = &chip;
static uint8_t flash[MiB_16];
static const unsigned char keep[] = {0x12,0x8a,0x63,0xba};
static esp_partition_t parts[7];
static const esp_partition_t *running, *boot;
static unsigned count=7, erased;
static bool encrypted, image_bad, tear_journal_write, tear_journal_erase;
static const esp_partition_t *fail_erase, *leave_residue;

static esp_err_t esp_flash_get_size(void *c,uint32_t *size) {(void)c;*size=MiB_16;return ESP_OK;}
static bool esp_flash_encryption_enabled(void) {return encrypted;}
static esp_partition_iterator_t esp_partition_find(int type,int subtype,const char *label) {
    assert(type==ESP_PARTITION_TYPE_ANY && subtype==ESP_PARTITION_SUBTYPE_ANY && !label);
    return count?1:0;
}
static esp_partition_iterator_t esp_partition_next(esp_partition_iterator_t it) {return it<count?it+1:0;}
static const esp_partition_t *esp_partition_find_first(int type,int subtype,const char *label) {
    for (unsigned i=0;i<7;i++) {
        if (parts[i].type==type && parts[i].subtype==subtype && !strcmp(parts[i].label,label)) return &parts[i];
    }
    return NULL;
}
static const esp_partition_t *esp_ota_get_running_partition(void) {return running;}
static const esp_partition_t *esp_ota_get_boot_partition(void) {return boot;}
static esp_err_t esp_ota_set_boot_partition(const esp_partition_t *part) {
    if (image_bad) return ESP_FAIL;
    assert(part==running);boot=part;return ESP_OK;
}
static esp_err_t esp_image_verify(int mode,const esp_partition_pos_t *pos,esp_image_metadata_t *metadata) {
    (void)metadata;assert(mode==ESP_IMAGE_VERIFY_SILENT && pos->offset==running->address && pos->size==running->size);
    return image_bad?ESP_FAIL:ESP_OK;
}
static esp_err_t esp_flash_read(void *c,void *out,uint32_t addr,size_t size) {
    (void)c;assert(addr>=FLASH_JOURNAL && addr+size<=MiB_16);memcpy(out,flash+addr,size);return ESP_OK;
}
static esp_err_t esp_flash_write(void *c,const void *data,uint32_t addr,size_t size) {
    (void)c;assert(addr==FLASH_JOURNAL && size<=0x1000);
    size_t n=tear_journal_write?size/2:size;
    for (size_t i=0;i<n;i++) flash[addr+i]&=((const uint8_t*)data)[i];
    return tear_journal_write?ESP_FAIL:ESP_OK;
}
static esp_err_t esp_flash_erase_region(void *c,uint32_t addr,size_t size) {
    (void)c;assert(addr==FLASH_JOURNAL && size==0x1000);
    memset(flash+addr,0xff,tear_journal_erase?1:size);
    return tear_journal_erase?ESP_FAIL:ESP_OK;
}
static esp_err_t esp_partition_read_raw(const esp_partition_t *p,size_t offset,void *out,size_t size) {
    assert(p!=running && offset+size<=p->size);
    memcpy(out,flash+p->address+offset,size);return ESP_OK;
}
static esp_err_t esp_partition_erase_range(const esp_partition_t *p,size_t offset,size_t size) {
    assert(p!=running && p!=boot && offset==0 && size==p->size);
    ++erased;
    memset(flash+p->address,0xff,p==fail_erase?size/2:size);
    if (p==leave_residue) flash[p->address+p->size-1]=0;
    return p==fail_erase?ESP_FAIL:ESP_OK;
}
static void vTaskDelay(int ticks) {assert(ticks==1);}
'''

TEST = r'''
static void seed(unsigned app) {
    memset(flash,0xff,sizeof(flash));
    erased=0;
    for (unsigned i=0;i<PARTITION_COUNT;i++) {
        const layout_entry_t *p=&s_expected[i];
        parts[i]=(esp_partition_t){p->type,p->subtype,p->address,p->size,p->label,&chip,false};
        memset(flash+p->address,0x87,p->size);
    }
    running=boot=&parts[app];
    memcpy(flash,keep,sizeof(keep)); /* bootloader */
    memcpy(flash+0x8000,keep,sizeof(keep)); /* table */
}
static void verify_clean(unsigned app) {
    unsigned spare=app==OTA_0?OTA_1:OTA_0;
    const unsigned targets[]={NVS,PHY,STORAGE,CORE_DUMP,spare};
    for (unsigned j=0;j<sizeof(targets)/sizeof(*targets);j++) {
        const esp_partition_t *p=&parts[targets[j]];
        for (unsigned i=0;i<p->size;i++) assert(flash[p->address+i]==0xff);
    }
    assert(flash[running->address]==0x87 && flash[parts[OTA_DATA].address]==0x87);
    assert(!memcmp(flash,keep,sizeof(keep)) && !memcmp(flash+0x8000,keep,sizeof(keep)));
    assert(boot==running && flash[JOURNAL_ADDR+sizeof(reset_journal_t)-1]==0xff);
}
int main(void) {
    for (unsigned active=OTA_0;active<=OTA_1;active++) {
        seed(active);
        assert(factory_reset_apply_pending()==ESP_OK && !erased);
        image_bad=true;assert(factory_reset_prepare()==ESP_FAIL);image_bad=false;
        count=8;assert(factory_reset_prepare()==ESP_ERR_NOT_SUPPORTED);count=7;
        encrypted=true;assert(factory_reset_prepare()==ESP_ERR_NOT_SUPPORTED);encrypted=false;
        parts[STORAGE].size-=0x1000;assert(factory_reset_prepare()==ESP_ERR_NOT_SUPPORTED);
        parts[STORAGE].size+=0x1000;
        tear_journal_write=true;assert(factory_reset_prepare()==ESP_FAIL);
        assert(factory_reset_apply_pending()==ESP_OK && !erased);tear_journal_write=false;

        /* A stale selector must be corrected before a destructive action. */
        boot=&parts[active==OTA_0?OTA_1:OTA_0];
        assert(factory_reset_prepare()==ESP_OK && boot==running);
        assert(factory_reset_prepare()==ESP_OK && !erased);
        const esp_partition_t *expected=running;
        running=&parts[active==OTA_0?OTA_1:OTA_0];
        assert(factory_reset_apply_pending()==ESP_ERR_INVALID_STATE && !erased);
        running=expected;
        const unsigned interrupted[]={STORAGE,CORE_DUMP,PHY,NVS,active==OTA_0?OTA_1:OTA_0};
        for (unsigned j=0;j<sizeof(interrupted)/sizeof(*interrupted);j++) {
            seed(active);assert(factory_reset_prepare()==ESP_OK);
            fail_erase=&parts[interrupted[j]];
            assert(factory_reset_apply_pending()==ESP_FAIL);
            reset_journal_t saved;memcpy(&saved,flash+JOURNAL_ADDR,sizeof(saved));
            assert(journal_valid(&saved)); /* a power loss keeps the intent */
            fail_erase=NULL;
            assert(factory_reset_apply_pending()==ESP_OK);
            verify_clean(active);
        }
        seed(active);assert(factory_reset_prepare()==ESP_OK);
        leave_residue=&parts[STORAGE];assert(factory_reset_apply_pending()==ESP_FAIL);leave_residue=NULL;
        assert(factory_reset_apply_pending()==ESP_OK);verify_clean(active);

        seed(active);assert(factory_reset_prepare()==ESP_OK);
        tear_journal_erase=true;assert(factory_reset_apply_pending()==ESP_FAIL);tear_journal_erase=false;
        /* A torn final erase can leave an unrecognizable marker only
           after all target partitions have been erased and verified. */
        unsigned before=erased;
        assert(factory_reset_apply_pending()==ESP_OK && erased==before);
        assert(flash[parts[NVS].address]==0xff && flash[parts[STORAGE].address]==0xff);
    }
    puts("PASS factory reset: both OTA slots, image/layout checks, interrupted journal write and removal, all five interrupted erases, read-back residue and retry");
    return 0;
}
'''

if __name__ == "__main__":
    run("factory-reset", STUBS + plain("factory_reset.c") + TEST)
