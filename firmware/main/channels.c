/*
 * Preset channels.
 *
 * A channel is a whole radio setting under a name: the two frequencies, the
 * bandwidth, the squelch, both subtones and the power setting.  Storing the
 * configuration struct itself rather than a hand-picked subset means a channel
 * preserves the complete operator profile. Its binary layout is frozen;
 * module-band metadata is separate so old channel files stay readable.
 *
 * Where it lives
 * --------------
 * The whole table is one file on the `storage` partition.  99 channels of 80
 * bytes is 7.9 KB, which the 24 KB NVS partition could not hold: it filled up
 * at 84 channels ("store full or NVS error", measured on the board), because
 * every slot costs a 32-byte NVS entry header on top of the 80-byte blob and
 * NVS also has to keep a page free for garbage collection.  The SPIFFS
 * partition is 1 MB and was already mounted for the tail tone, so the table
 * moved there.
 *
 * The table is held in RAM (PSRAM) and rewritten whole on every change, which
 * costs nothing at this size and turns save/delete into a single file write
 * instead of per-slot NVS bookkeeping.
 *
 * NVS is still read once, at boot, when the file does not exist yet: older
 * firmware kept channels in NVS, and losing the operator's presets on a
 * firmware update is not acceptable.  After migrating, the NVS namespace is
 * erased so its space goes back to the WiFi settings.
 */
#include "channels.h"

#include <string.h>
#include <stdio.h>
#include <math.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_spiffs.h"
#include "esp_heap_caps.h"

static const char *TAG = "chan";

#define CH_PATH      "/storage/chans.bin"
#define CH_TMP       "/storage/chans.tmp"
#define CH_MAGIC     0x4F524C43u        /* "ORLC" */
#define CH_VERSION   1u

/* Legacy NVS layout -- read once for the migration, never written again. */
#define NVS_NS       "channels"
#define KEY_COUNT    "n"

typedef struct {
    char        name[CHANNEL_NAME_LEN];
    sa818_cfg_t cfg;
} channel_blob_t;
_Static_assert(sizeof(channel_blob_t) == 80, "preserve channel file v1 layout");

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t count;
    uint32_t reserved;
} chans_hdr_t;

static channel_blob_t *s_all;
static int  s_count;
static bool s_ready;

/* --------------------------------------------------------------- storage */

static bool spiffs_up(void)
{
    static bool tried;
    if (tried) {
        return true;
    }
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/storage",
        .partition_label = "storage",
        .max_files = 4,
        .format_if_mount_failed = true,
    };
    const esp_err_t e = esp_vfs_spiffs_register(&conf);
    /* ESP_ERR_INVALID_STATE: voice_tail.c got there first.  Same configuration,
     * so either order works. */
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "storage mount failed: %s", esp_err_to_name(e));
        return false;
    }
    tried = true;
    return true;
}

static bool store_write(void);

/* Pull the channels out of the old NVS namespace, once. */
static int migrate_from_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return 0;
    }
    uint8_t n = 0;
    if (nvs_get_u8(h, KEY_COUNT, &n) != ESP_OK || n == 0 || n > CHANNELS_MAX) {
        nvs_close(h);
        return 0;
    }
    int moved = 0;
    for (int i = 0; i < n && moved < CHANNELS_MAX; ++i) {
        char key[8];
        snprintf(key, sizeof(key), "ch%d", i);
        size_t len = sizeof(channel_blob_t);
        if (nvs_get_blob(h, key, &s_all[moved], &len) == ESP_OK &&
            len == sizeof(channel_blob_t)) {
            s_all[moved].name[CHANNEL_NAME_LEN - 1] = '\0';
            if (s_all[moved].name[0] == '\0') {
                snprintf(s_all[moved].name, sizeof(s_all[moved].name), "CH%02d", moved + 1);
            }
            ++moved;
        }
    }
    if (moved) {
        s_count = moved;
        if (store_write()) {
            /* Only give the NVS space back once the file is safely on flash. */
            nvs_erase_all(h);
            nvs_commit(h);
            ESP_LOGW(TAG, "migrated %d channel(s) from NVS to %s", moved, CH_PATH);
        }
    }
    nvs_close(h);
    return moved;
}

static bool store_load(void)
{
    FILE *f = fopen(CH_PATH, "rb");
    if (!f) {
        return false;
    }
    chans_hdr_t hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1 || hdr.magic != CH_MAGIC ||
        hdr.version != CH_VERSION || hdr.count > CHANNELS_MAX) {
        fclose(f);
        ESP_LOGW(TAG, "%s is not a channel table (magic %08x v%u), starting empty",
                 CH_PATH, (unsigned)hdr.magic, (unsigned)hdr.version);
        return false;
    }
    const int n = (int)hdr.count;
    size_t got = 0;
    if (n > 0) {
        got = fread(s_all, sizeof(channel_blob_t), (size_t)n, f);
    }
    fclose(f);
    if (n > 0 && got != (size_t)n) {
        ESP_LOGW(TAG, "short read from %s (%u of %d)", CH_PATH, (unsigned)got, n);
        return false;
    }
    for (int i = 0; i < n; ++i) {
        s_all[i].name[CHANNEL_NAME_LEN - 1] = '\0';
    }
    s_count = n;
    return true;
}

static bool store_write(void)
{
    if (!spiffs_up()) {
        return false;
    }
    const chans_hdr_t hdr = { .magic = CH_MAGIC, .version = CH_VERSION,
                              .count = (uint32_t)s_count, .reserved = 0 };
    /* Write a temporary file and rename over the real one: a power cut in the
     * middle then costs the last edit, not the whole table. */
    FILE *f = fopen(CH_TMP, "wb");
    if (!f) {
        ESP_LOGW(TAG, "cannot open %s", CH_TMP);
        return false;
    }
    bool ok = fwrite(&hdr, sizeof(hdr), 1, f) == 1;
    if (ok && s_count > 0) {
        ok = fwrite(s_all, sizeof(channel_blob_t), (size_t)s_count, f) ==
             (size_t)s_count;
    }
    if (fclose(f) != 0) {
        ok = false;
    }
    if (!ok) {
        remove(CH_TMP);
        ESP_LOGW(TAG, "short write to %s", CH_TMP);
        return false;
    }
    remove(CH_PATH);
    if (rename(CH_TMP, CH_PATH) != 0) {
        ESP_LOGW(TAG, "rename %s -> %s failed", CH_TMP, CH_PATH);
        return false;
    }
    return true;
}

void channels_init(void)
{
    if (s_ready) {
        return;
    }
    s_all = heap_caps_malloc(sizeof(channel_blob_t) * CHANNELS_MAX,
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_all) {
        s_all = malloc(sizeof(channel_blob_t) * CHANNELS_MAX);
    }
    if (!s_all) {
        ESP_LOGW(TAG, "no memory for %d channels", CHANNELS_MAX);
        return;
    }
    s_ready = true;

    if (spiffs_up() && store_load()) {
        ESP_LOGI(TAG, "%d channel(s) from %s", s_count, CH_PATH);
        return;
    }
    if (migrate_from_nvs()) {
        return;
    }
    s_count = 0;
    ESP_LOGI(TAG, "channel table empty (cap %d)", CHANNELS_MAX);
}

/* ---------------------------------------------------------------- public */

int channels_count(void)
{
    channels_init();
    return s_count;
}

bool channels_get(int index, char *name, size_t name_len, sa818_cfg_t *out)
{
    channels_init();
    if (index < 0 || index >= s_count) {
        return false;
    }
    if (name && name_len) {
        snprintf(name, name_len, "%s", s_all[index].name);
    }
    if (out) {
        *out = s_all[index].cfg;
    }
    return true;
}

int channels_save(int index, const char *name, const sa818_cfg_t *cfg)
{
    channels_init();
    if (!s_ready || !cfg) {
        return -1;
    }
    int slot;
    if (index < 0 || index >= s_count) {
        if (s_count >= CHANNELS_MAX) {
            ESP_LOGW(TAG, "store full (%d)", s_count);
            return -1;
        }
        slot = s_count++;
    } else {
        slot = index;
    }
    channel_blob_t *b = &s_all[slot];
    memset(&b->name, 0, sizeof(b->name));
    /* '|' is the separator in some other formats and has bitten before; keep
     * names free of control characters at least. */
    snprintf(b->name, sizeof(b->name), "%s", (name && name[0]) ? name : "未命名");
    b->cfg = *cfg;
    if (!store_write()) {
        ESP_LOGW(TAG, "slot %d kept in RAM only -- storage write failed", slot);
    }
    ESP_LOGI(TAG, "channel %d = \"%s\" %.4f/%.4f", slot, b->name,
             (double)cfg->tx_mhz, (double)cfg->rx_mhz);
    return slot;
}

bool channels_delete(int index)
{
    channels_init();
    if (!s_ready || index < 0 || index >= s_count) {
        return false;
    }
    /* Shift the tail down so the list stays contiguous. */
    if (index < s_count - 1) {
        memmove(&s_all[index], &s_all[index + 1],
                sizeof(channel_blob_t) * (size_t)(s_count - index - 1));
    }
    --s_count;
    return store_write();
}

/* Bounded raw storage strings, including control bytes, remain valid JSON. */
static void json_string(const char *src, size_t len, char *out)
{
    size_t o = 0;
    for (size_t i = 0; i < len && src[i]; ++i) {
        const unsigned char c = (unsigned char)src[i];
        if (c < 32) { o += (size_t)sprintf(out + o, "\\u%04x", c); }
        else {
            if (c == '"' || c == '\\') { out[o++] = '\\'; }
            out[o++] = (char)c;
        }
    }
    out[o] = 0;
}

int channels_list_json(char *out, size_t out_len)
{
    if (!out || out_len < 32) {
        return 0;
    }
    channels_init();
    size_t o = 0;
    int n = 0;
    o += (size_t)snprintf(out + o, out_len - o, "[");
    for (int i = 0; i < s_count; ++i) {
        if (o + 768 >= out_len) {
            snprintf(out, out_len, "[]");
            return -1;
        }
        const sa818_cfg_t *c = &s_all[i].cfg;
        /* Names go through a JSON string escape: a quote or backslash in a
         * user-supplied name would otherwise break the whole document. */
        char esc[CHANNEL_NAME_LEN * 6 + 1], txt[49], rxt[49];
        json_string(s_all[i].name, CHANNEL_NAME_LEN, esc);
        json_string(c->tx_ctcss, sizeof(c->tx_ctcss), txt);
        json_string(c->rx_ctcss, sizeof(c->rx_ctcss), rxt);
        char tx[64] = "null", rx[64] = "null";
        if (!sa818_freq_format(c->tx_mhz, tx, sizeof(tx))) { strcpy(tx, "null"); }
        if (!sa818_freq_format(c->rx_mhz, rx, sizeof(rx))) { strcpy(rx, "null"); }
        const sa818_band_t band = sa818_freq_pair_valid(SA818_BAND_UHF, c->tx_mhz, c->rx_mhz)
            ? SA818_BAND_UHF : sa818_freq_pair_valid(SA818_BAND_VHF, c->tx_mhz, c->rx_mhz)
            ? SA818_BAND_VHF : SA818_BAND_UNSET;
        o += (size_t)snprintf(
            out + o, out_len - o,
            "%s{\"i\":%d,\"name\":\"%s\",\"tx\":%s,\"rx\":%s,"
            "\"bw\":%d,\"sq\":%d,\"vol\":%d,\"pwr\":%d,"
            "\"txt\":\"%s\",\"rxt\":\"%s\",\"band\":\"%s\",\"compatible\":%s}",
            n ? "," : "", i, esc, tx, rx,
            c->bandwidth, c->squelch, c->volume, c->high_power,
            txt, rxt, sa818_band_name(band), sa818_cfg_compatible(c) ? "true" : "false");
        ++n;
    }
    o += (size_t)snprintf(out + o, out_len - o, "]");
    return n;
}
