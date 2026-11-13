/*
 * voice_tail.c -- end-of-transmission tail tones.  See voice_tail.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "voice_tail.h"
#include <stdatomic.h>
#include "freertos/idf_additions.h"
#include "audio_i2s.h"   /* RATE follows the stream rate */
#include "g711.h"
#include "minimp3.h"
#include "ptt.h"

static const char *TAG = "tail";

#define RATE          AUDIO_SAMPLE_RATE_HZ
#define CW_HZ         600.0
#define CW_AMP        9000
#define DOT_MS        30                 /* ~16.7 WPM */
#define GAP_MS        30                 /* intra-character element gap */
#define LEAD_MS       40
/*
 * Trailing pad after the last CW element.  This is DEAD CARRIER: the PTT is
 * held for as long as the tone buffer is, so a long pad is heard as "the
 * transmitter stays on after the tone finished".  10 ms is enough to let the
 * last element's 3 ms fade land on silence without a click.
 */
#define TRAIL_MS      10
#define BEEP_HZ       880.0
#define BEEP_AMP      9000
#define BEEP_MS       150
#define NORM_PEAK     12000              /* normalisation target for uploads */

#define MS2S(ms)      ((size_t)(ms) * RATE / 1000)
#define CUSTOM_PATH   "/storage/tail.bin"
#define CUSTOM_MAX_STORED (TAIL_CUSTOM_MAX_SAMPLES)

/*
 * Built-in tones, synthesized once at init into a single pool.  Sized with
 * headroom over the measured total of the seven built-in tones (~17 000).
 *
 * The pool lives in PSRAM: a static DRAM block this size would starve the
 * Wi-Fi driver's contiguous mbuf allocations (ESP_ERR_NO_MEM in
 * esp_wifi_init -- the crash loop that was found on the bench).  8 KB/s of
 * PSRAM reads at 16 ms chunk granularity is nothing for the audio task.
 */
#define POOL_SAMPLES  20000
static int16_t *s_pool = NULL;
static size_t s_burst_pos = 0;    /* running pool offset for the synth */
static size_t s_off[TAIL_COUNT];
static size_t s_len[TAIL_COUNT];

static int  s_sel = TAIL_OFF;
static uint8_t s_modes = TAIL_MODE_ALL;
/*
 * Custom (uploaded) tone.  Cached in PSRAM by custom_load() so the transmit
 * path is a plain memcpy: no file system access while the radio is keyed.
 *
 *   s_custom_pcm : decoded 8 kHz PCM (PSRAM, CUSTOM_MAX_STORED samples)
 *   s_custom_n   : samples actually in the file (what "stored" means)
 *   s_custom_off : first sample of the audible window
 *   s_custom_win : length of the audible window (what gets TRANSMITTED)
 *   s_nvs_len    : length NVS remembers, used only to report a mismatch
 */
static int16_t *s_custom_pcm = NULL;
static size_t s_custom_n = 0;
static size_t s_custom_off = 0;
static size_t s_custom_win = 0;
static size_t s_nvs_len = 0;
static uint32_t s_stored_rate;   /* rate the file was converted to, 0 = unknown */
static atomic_bool s_storing;

/* ------------------------------------------------------- built-in synth */

/*
 * One element of a parsed CW pattern: start (ms into the tone) and length.
 * Pattern language: '.' = dit, '-' = dah, ' ' = element gap.
 */
#define CW_MAX_ELEMS 32
typedef struct { uint32_t start, dur; } cw_elem_t;

static int cw_build(const char *pat, cw_elem_t *els, int max_els)
{
    uint32_t t = (uint32_t)LEAD_MS;
    int count = 0;
    for (const char *p = pat; *p; ++p) {
        if (*p == '.' || *p == '-') {
            const uint32_t dur = (*p == '-') ? 3 * DOT_MS : DOT_MS;
            if (count < max_els) {
                els[count].start = t;
                els[count].dur = dur;
                ++count;
            }
            t += dur;
        } else if (*p == ' ') {
            t += GAP_MS;
        }
    }
    return count;
}

static uint32_t cw_total_ms(const cw_elem_t *els, int count)
{
    if (!count) {
        return 0;
    }
    return els[count - 1].start + els[count - 1].dur + TRAIL_MS;
}

/*
 * Generic beep burst (brand tones): `reps` beeps of `dur_ms` at `hz`,
 * optionally with a simultaneous second tone at `hz2` (DTMF style, each
 * at half amplitude so the pair does not clip), `gap_ms` between reps,
 * 3 ms fades.  Appends to the pool at *pos and advances it.
 */
static void synth_burst(size_t *pos, int id, double hz, double hz2,
                        int dur_ms, int gap_ms, int reps, float amp)
{
    size_t total = MS2S(LEAD_MS + 20 + dur_ms * reps + gap_ms * (reps - 1));
    s_off[id] = *pos;
    s_len[id] = total;
    double phase = 0.0;
    double phase2 = 0.0;
    const double w1 = 2.0 * M_PI * hz / RATE;
    const double w2 = 2.0 * M_PI * hz2 / RATE;
    for (size_t s = 0; s < total; ++s) {
        if (*pos + s >= POOL_SAMPLES) {
            total = s;
            s_len[id] = total;
            break;
        }
        const int t_ms = (int)(s * 1000 / RATE) - LEAD_MS;
        int16_t v = 0;
        if (t_ms >= 0) {
            const int cycle = t_ms / (dur_ms + gap_ms);
            const int in = t_ms % (dur_ms + gap_ms);
            if (cycle < reps && in < dur_ms) {
                const int F = RATE * 3 / 1000;
                float fade = 1.0f;
                if (in < F) {
                    fade = (float)in / F;
                } else if (dur_ms - in < F) {
                    fade = (float)(dur_ms - in) / F;
                }
                const float a = hz2 ? 0.5f * amp : amp;
                v = (int16_t)(a * fade *
                              (sinf((float)phase) +
                               (hz2 ? sinf((float)phase2) : 0.0f)));
            }
        }
        s_pool[*pos + s] = v;
        phase += w1;
        if (hz2) {
            phase2 += w2;
        }
    }
    *pos += total;
}

static void synth_builtins(void)
{
    if (!s_pool) {
        return;
    }
    static const struct { int id; const char *pat; } cw_tones[] = {
        { TAIL_KCHOO, "-.-." },
    };
    size_t pos = 0;

    /* Prosigns: one continuous shape, 600 Hz, 3 ms fades per element. */
    for (size_t i = 0; i < sizeof(cw_tones) / sizeof(cw_tones[0]); ++i) {
        const int id = cw_tones[i].id;
        cw_elem_t els[CW_MAX_ELEMS];
        const int count = cw_build(cw_tones[i].pat, els, CW_MAX_ELEMS);
        size_t total = MS2S((int)cw_total_ms(els, count));
        s_off[id] = pos;
        s_len[id] = total;
        double phase = 0.0;
        const double w = 2.0 * M_PI * CW_HZ / RATE;
        for (size_t s = 0; s < total; ++s) {
            if (pos + s >= POOL_SAMPLES) {
                total = s;
                s_len[id] = total;
                break;
            }
            const int t_ms = (int)(s * 1000 / RATE);
            int16_t v = 0;
            for (int e = 0; e < count; ++e) {
                if ((uint32_t)t_ms >= els[e].start &&
                    (uint32_t)t_ms < els[e].start + els[e].dur) {
                    const int rel = (int)((uint32_t)t_ms - els[e].start) * RATE / 1000;
                    const int end = (int)els[e].dur * RATE / 1000;
                    /* 3 ms fade in/out: a hard key click at 600 Hz is ugly. */
                    const int F = RATE * 3 / 1000;
                    float fade = 1.0f;
                    if (rel < F) {
                        fade = (float)rel / F;
                    } else if (end - rel < F) {
                        fade = (float)(end - rel) / F;
                    }
                    v = (int16_t)(CW_AMP * fade * sinf((float)phase));
                    break;
                }
            }
            s_pool[pos + s] = v;
            phase += w;
        }
        pos += total;
    }

    /* Beep: single 880 Hz, 150 ms. */
    {
        const int id = TAIL_BEEP1;
        size_t total = MS2S(LEAD_MS + 20 + BEEP_MS);
        s_off[id] = pos;
        s_len[id] = total;
        double phase = 0.0;
        const double w = 2.0 * M_PI * BEEP_HZ / RATE;
        for (size_t s = 0; s < total; ++s) {
            if (pos + s >= POOL_SAMPLES) {
                total = s;
                s_len[id] = total;
                break;
            }
            const int t_ms = (int)(s * 1000 / RATE) - LEAD_MS;
            int16_t v = 0;
            if (t_ms >= 0 && t_ms < BEEP_MS) {
                const int F = RATE * 20 / 1000;
                float fade = 1.0f;
                if (t_ms < F) {
                    fade = (float)t_ms / F;
                } else if (BEEP_MS - t_ms < F) {
                    fade = (float)(BEEP_MS - t_ms) / F;
                }
                v = (int16_t)(BEEP_AMP * fade * sinf((float)phase));
            }
            s_pool[pos + s] = v;
            phase += w;
        }
        pos += total;
    }

    /* Brand classics (researched from manuals / firmware sources). */
    synth_burst(&pos, TAIL_ROGER, 880.0, 0.0, 100, 0, 1, BEEP_AMP);

    /*
     * MDC-1200 (Motorola): 1200-baud FFSK, CCIR tones -- mark (1) = 1200 Hz,
     * space (0) = 1800 Hz.  A real burst starts with the 40-bit sync word
     * 0x07092A446F followed by 112 data bits; this tone is the sync word
     * plus a 32-bit 0xA5A5A5A5 tail (72 bits ~ 60 ms) -- recognisable as the
     * classic Motorola "digital chirp" without claiming to be a decodable
     * frame.
     */
    {
        static const uint64_t sync = UINT64_C(0x07092A446F);
        static const uint32_t data_bits = 0xA5A5A5A5u;
        const int bits = 40 + 32;
        const int burst_ms = (bits * 1000 + 1199) / 1200;   /* 1200 baud */
        size_t total = MS2S(LEAD_MS + 20 + burst_ms);
        s_off[TAIL_MDC] = pos;
        s_len[TAIL_MDC] = total;
        double phase = 0.0;
        for (size_t s = 0; s < total; ++s) {
            if (pos + s >= POOL_SAMPLES) {
                total = s;
                s_len[TAIL_MDC] = total;
                break;
            }
            const int t_ms = (int)(s * 1000 / RATE) - LEAD_MS;
            int16_t v = 0;
            if (t_ms >= 0 && t_ms < burst_ms) {
                const int bit = (t_ms * 1200) / 1000;
                const int b = (bit < 40)
                              ? ((sync >> (39 - bit)) & 1u)
                              : ((data_bits >> (31 - (bit - 40))) & 1u);
                const double hz = b ? 1200.0 : 1800.0;
                /* 2 ms fades at the burst edges */
                const int F = RATE * 2 / 1000;
                float fade = 1.0f;
                if (s < F) {
                    fade = (float)s / F;
                } else if (total - s < F) {
                    fade = (float)(total - s) / F;
                }
                v = (int16_t)(CW_AMP * fade * sinf((float)phase));
                phase += 2.0 * M_PI * hz / RATE;
            }
            s_pool[pos + s] = v;
        }
        pos += total;
    }

    /* Icom IDAS analog "go ahead": DTMF * = 941 Hz. */
    synth_burst(&pos, TAIL_ICOM_STAR, 941.0, 0.0, 200, 0, 1, BEEP_AMP);
    /* ...or DTMF # = 941 + 1336 Hz. */
    synth_burst(&pos, TAIL_ICOM_HASH, 941.0, 1336.0, 200, 0, 1, BEEP_AMP);
    /* Quansheng UV-K5 tail-style double (firmware: 880 Hz, 60 ms beeps). */
    synth_burst(&pos, TAIL_K5X2, 880.0, 0.0, 60, 80, 2, BEEP_AMP);

    if (pos > POOL_SAMPLES) {
        ESP_LOGE(TAG, "tone pool overflow: %u > %d", (unsigned)pos, POOL_SAMPLES);
        /* truncate the last tone to the remaining space */
        for (int i = TAIL_COUNT - 1; i > 0; --i) {
            if (s_off[i] + s_len[i] > POOL_SAMPLES) {
                s_len[i] = (s_off[i] < POOL_SAMPLES) ? POOL_SAMPLES - s_off[i] : 0;
            }
        }
    }
    ESP_LOGI(TAG, "tones synthesized: %u / %d samples", (unsigned)pos, POOL_SAMPLES);
    for (int i = 0; i < TAIL_COUNT; ++i) {
        if (i != TAIL_OFF && i != TAIL_CUSTOM && s_len[i]) {
            ESP_LOGI(TAG, "  %-8s %4.1f s", tail_name(i), (double)s_len[i] / RATE);
        }
    }
}

/* ------------------------------------------------------------- naming */

static const char *const s_names[TAIL_COUNT] = {
    "off", "beep", "K-CHOO", "roger", "mdc1200",
    "icom *", "icom #", "k5 x2", "custom",
};

const char *tail_name(int id)
{
    if (id < 0 || id >= TAIL_COUNT) {
        return "?";
    }
    return s_names[id];
}

/* ------------------------------------------------------------- storage */

static void spiffs_up(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/storage",
        .partition_label = "storage",
        .max_files = 4,
        .format_if_mount_failed = true,
    };
    esp_err_t e = esp_vfs_spiffs_register(&conf);
    if (e == ESP_ERR_INVALID_STATE) {
        return;                          /* already mounted */
    }
    ESP_LOGI(TAG, "storage partition: %s", esp_err_to_name(e));
}

static void nvs_load(void)
{
    nvs_handle_t h;
    if (nvs_open("tail", NVS_READONLY, &h) != ESP_OK) {
        return;                           /* first boot: defaults */
    }
    uint8_t sel = TAIL_OFF;
    (void)nvs_get_u8(h, "sel", &sel);
    uint8_t modes = TAIL_MODE_ALL;
    (void)nvs_get_u8(h, "modes", &modes);
    uint32_t len = 0;
    (void)nvs_get_u32(h, "len", &len);
    uint32_t rate = 0;
    (void)nvs_get_u32(h, "rate", &rate);
    nvs_close(h);
    /*
     * A custom tone is stored as u-law, which says nothing about how fast it
     * was recorded.  When the stream rate changes (8 kHz -> 12 kHz) the stored
     * clip plays 1.5x fast and sharp, so drop it once and say why instead of
     * shipping a wrong-sounding tail.  The operator re-uploads and it is
     * converted at the current rate.
     */
    /*
     * A file converted for a different stream rate used to be DELETED here, so
     * every rate change (8k -> 12k, 12k -> 16k) silently threw away the tone the
     * operator uploaded.  Keep it and resample on load instead: one linear
     * interpolation pass at boot, and the tone keeps working across rate changes.
     */
    s_stored_rate = rate;
    if (rate && (uint32_t)RATE != rate) {
        ESP_LOGW(TAG, "custom tail was converted for %u Hz, stream is %d Hz -- "
                      "resampling", (unsigned)rate, RATE);
    }
    if (sel >= TAIL_COUNT) {
        sel = TAIL_OFF;
    }
    if (!modes || (modes & (uint8_t)~TAIL_MODE_ALL)) {
        modes = TAIL_MODE_ALL;
    }
    s_sel = (int)sel;
    s_modes = modes;
    /* Only a sanity bound here: the real length comes from the file itself when
     * the tone is cached (see custom_load).  NVS and SPIFFS can disagree after
     * a partition format, and trusting NVS is what made the panel promise a
     * tone that no longer existed. */
    s_nvs_len = (len > CUSTOM_MAX_STORED) ? CUSTOM_MAX_STORED : (size_t)len;
}

static void nvs_save(void)
{
    nvs_handle_t h;
    if (nvs_open("tail", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    (void)nvs_set_u8(h, "sel", (uint8_t)s_sel);
    (void)nvs_set_u8(h, "modes", s_modes);
    (void)nvs_set_u32(h, "len", (uint32_t)s_custom_n);
    (void)nvs_set_u32(h, "rate", (uint32_t)RATE);
    (void)nvs_commit(h);
    nvs_close(h);
}

/*
 * Audible window inside a stored tone.
 *
 * Everything in the buffer is transmitted, so leading/trailing near-silence is
 * dead carrier: the PTT is held for it and the far end hears nothing.  This
 * finds the first and last sample above a conservative threshold (-30 dBFS
 * relative to the clip's own peak) plus a 20 ms guard so the clip does not
 * start or end abruptly.
 *
 * It deliberately does NOT modify the audio: the window is recomputed from the
 * stored file on every boot, so a genuine quiet or decaying tone is never
 * destroyed by a threshold guess, and a re-upload always starts from the
 * original data.  If the audible part would be less than half of the clip, the
 * whole clip is kept -- at that point the "silence" is the content.
 */
static void audio_window(const int16_t *pcm, size_t n, size_t *off, size_t *len)
{
    *off = 0;
    *len = n;
    if (!pcm || n == 0) {
        return;
    }
    int peak = 0;
    for (size_t i = 0; i < n; ++i) {
        const int a = pcm[i] < 0 ? -pcm[i] : pcm[i];
        if (a > peak) {
            peak = a;
        }
    }
    if (peak <= 200) {
        return;                    /* too quiet to judge: play it all */
    }
    const int thr = peak / 30;     /* about -30 dBFS */
    size_t s = 0, e = n;
    while (s < n && (pcm[s] < 0 ? -pcm[s] : pcm[s]) < thr) {
        ++s;
    }
    while (e > s && (pcm[e - 1] < 0 ? -pcm[e - 1] : pcm[e - 1]) < thr) {
        --e;
    }
    const size_t guard = RATE / 50;            /* 20 ms */
    s = (s > guard) ? (s - guard) : 0;
    e = (n - e > guard) ? (e + guard) : n;
    if (e <= s || (e - s) < RATE / 5 || (e - s) < n / 2) {
        return;                    /* nothing sane, or most of it is content */
    }
    *off = s;
    *len = e - s;
    ESP_LOGI(TAG, "custom tail audible window: %u..%u of %u samples "
             "(%u ms of dead carrier skipped)",
             (unsigned)s, (unsigned)e, (unsigned)n,
             (unsigned)((n - (e - s)) * 1000 / RATE));
}

/*
 * Load the uploaded tone from SPIFFS into PSRAM, ONCE.
 *
 * The transmit path must never touch the file system.  It used to fopen/seek/
 * read per 16 ms block, and the first block after an idle period paid for a
 * cold SPIFFS lookup: the stream stalled, the beginning of the tone was
 * swallowed, and -- because the PTT window is only refreshed after a
 * successful fetch -- a short transmission could lose the tone completely when
 * the stall ran past the window.  Decoding it here also removes the shared
 * file handle that the preview and the talk tail used to fight over, and lets
 * tail_samples() report what is really stored instead of what NVS remembers.
 */
static void custom_load(void)
{
    if (!s_custom_pcm) {
        s_custom_pcm = (int16_t *)heap_caps_malloc(
            CUSTOM_MAX_STORED * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (!s_custom_pcm) {
            ESP_LOGE(TAG, "no PSRAM for the custom tone cache -- "
                          "uploaded tones unavailable");
            s_custom_n = 0;
            s_custom_off = 0;
            s_custom_win = 0;
            return;
        }
    }
    s_custom_n = 0;
    s_custom_off = 0;
    s_custom_win = 0;

    FILE *f = fopen(CUSTOM_PATH, "rb");
    if (!f) {
        if (s_nvs_len) {
            ESP_LOGW(TAG, "custom tail: NVS remembers %u samples but %s is "
                          "not readable -- the tone is GONE (SPIFFS formatted?)",
                     (unsigned)s_nvs_len, CUSTOM_PATH);
        }
        return;
    }
    uint8_t mu[512];
    size_t total = 0;
    while (total < CUSTOM_MAX_STORED) {
        size_t want = CUSTOM_MAX_STORED - total;
        if (want > sizeof(mu)) {
            want = sizeof(mu);
        }
        const size_t r = fread(mu, 1, want, f);
        if (!r) {
            break;
        }
        g711_ulaw_decode_buf(mu, s_custom_pcm + total, r);
        total += r;
    }
    fclose(f);
    s_custom_n = total;

    /*
     * Resample once, at load: the playback path is a memcpy (tail_fetch) and
     * must stay one, so the cache has to be at the stream rate by the time the
     * radio keys up.
     */
    if (total && s_stored_rate && s_stored_rate != (uint32_t)RATE) {
        const size_t out_n = (size_t)((double)total * (double)RATE / (double)s_stored_rate);
        int16_t *rs = (int16_t *)heap_caps_malloc(out_n * sizeof(int16_t),
                                                  MALLOC_CAP_SPIRAM);
        if (rs) {
            const double step = (double)s_stored_rate / (double)RATE;
            for (size_t i = 0; i < out_n; ++i) {
                const double x = (double)i * step;
                const size_t i0 = (size_t)x;
                const double fr = x - (double)i0;
                const int32_t a = (i0 < total) ? s_custom_pcm[i0] : 0;
                const int32_t b = (i0 + 1 < total) ? s_custom_pcm[i0 + 1] : a;
                rs[i] = (int16_t)((double)a + ((double)b - (double)a) * fr);
            }
            heap_caps_free(s_custom_pcm);
            s_custom_pcm = rs;
            total = out_n;
            s_custom_n = total;
            ESP_LOGW(TAG, "custom tail resampled %u -> %u Hz (%u samples)",
                     (unsigned)s_stored_rate, (unsigned)RATE, (unsigned)total);
            s_stored_rate = RATE;
            nvs_save();
        } else {
            ESP_LOGW(TAG, "no PSRAM to resample the custom tail -- playing it at "
                          "the wrong speed");
        }
    }
    audio_window(s_custom_pcm, total, &s_custom_off, &s_custom_win);
    ESP_LOGI(TAG, "custom tail cached in PSRAM: %u samples (%.2f s stored, "
             "%.2f s transmitted)",
             (unsigned)total, (double)total / RATE, (double)s_custom_win / RATE);
    if (total != s_nvs_len) {
        ESP_LOGW(TAG, "custom tail length mismatch: NVS %u vs file %u; "
                      "the file wins", (unsigned)s_nvs_len, (unsigned)total);
    }
}

void voice_tail_init(void)
{
    s_pool = (int16_t *)heap_caps_malloc(POOL_SAMPLES * sizeof(int16_t),
                                         MALLOC_CAP_SPIRAM);
    if (!s_pool) {
        ESP_LOGE(TAG, "no PSRAM for the tone pool -- built-in tails disabled");
    }
    synth_builtins();
    spiffs_up();
    nvs_load();
    custom_load();                    /* cache the uploaded tone in PSRAM */
    if (s_sel == TAIL_CUSTOM && s_custom_win == 0) {
        /* Selected a custom tone that is no longer on flash (wiped?). */
        ESP_LOGW(TAG, "custom tail selected but not available -- falling back");
        s_sel = TAIL_OFF;
    }
    ESP_LOGI(TAG, "selected: %s (%u samples)", tail_name(s_sel),
             (unsigned)tail_samples(s_sel));
}

size_t tail_samples(int id)
{
    if (id < 0 || id >= TAIL_COUNT) {
        return 0;
    }
    if (id == TAIL_OFF) {
        return 0;
    }
    if (id == TAIL_CUSTOM) {
        return s_custom_win;          /* what actually goes on the air */
    }
    if (!s_pool) {
        return 0;
    }
    return s_len[id];
}

int tail_get(void)
{
    return s_sel;
}

uint8_t tail_modes_get(void)
{
    return s_modes;
}

esp_err_t tail_modes_set(int modes)
{
    if (modes < 1 || modes > (int)TAIL_MODE_ALL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_modes = (uint8_t)modes;
    ESP_LOGI(TAG, "tail modes -> 0x%02x", (unsigned)s_modes);
    nvs_save();
    return ESP_OK;
}

bool tail_should_play(int mode)
{
    if (mode < 0 || mode > 2 || s_sel == TAIL_OFF) {
        return false;
    }
    return (s_modes & (uint8_t)(1u << mode)) != 0;
}

esp_err_t tail_set(int id)
{
    if (id < 0 || id >= TAIL_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (id == TAIL_CUSTOM && s_custom_win == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    s_sel = id;
    ESP_LOGI(TAG, "tail -> %s", tail_name(id));
    nvs_save();
    return ESP_OK;
}

/* ------------------------------------------------------ custom (SPIFFS) */

size_t tail_custom_samples(void)
{
    return s_custom_n;
}

esp_err_t tail_custom_delete(void)
{
    (void)remove(CUSTOM_PATH);
    s_custom_n = 0;
    s_custom_off = 0;
    s_custom_win = 0;
    s_nvs_len = 0;
    if (s_sel == TAIL_CUSTOM) {
        s_sel = TAIL_OFF;
    }
    nvs_save();
    ESP_LOGI(TAG, "custom tail deleted");
    return ESP_OK;
}

/*
 * Shared finish for uploads: `pcm` holds 8 kHz mono int16 (ownership is
 * taken).  Peak-normalises to -2.5 dBFS, mu-law encodes and writes the
 * storage partition, then records the length.
 */
static esp_err_t custom_store_finish(int16_t *pcm, size_t out_n)
{
    if (out_n < RATE / 5) {
        free(pcm);
        return ESP_ERR_INVALID_SIZE;    /* shorter than 0.2 s */
    }

    /*
     * Note: leading/trailing silence is NOT cut out of the stored audio.  The
     * audible window is computed at load time instead (audio_window), so the
     * uploaded data is kept intact and a re-upload always starts from the
     * original clip -- an earlier version rewrote the file with a -34 dBFS
     * threshold and could eat most of a quiet or decaying tone.
     */

    /* Peak-normalise: a quiet recording must not come out inaudible, a hot
     * one must not clip. */
    int peak = 0;
    for (size_t i = 0; i < out_n; ++i) {
        const int a = pcm[i] < 0 ? -pcm[i] : pcm[i];
        if (a > peak) {
            peak = a;
        }
    }
    if (peak > 200) {
        const float sc = (float)NORM_PEAK / (float)peak;
        for (size_t i = 0; i < out_n; ++i) {
            pcm[i] = (int16_t)((int)pcm[i] * sc);
        }
    }

    uint8_t *mu = malloc(out_n);
    if (!mu) {
        free(pcm);
        return ESP_ERR_NO_MEM;
    }
    g711_ulaw_encode_buf(pcm, mu, out_n);
    free(pcm);

    FILE *f = fopen(CUSTOM_PATH, "wb");
    if (!f) {
        free(mu);
        ESP_LOGE(TAG, "cannot open %s for write", CUSTOM_PATH);
        return ESP_FAIL;
    }
    const size_t wr = fwrite(mu, 1, out_n, f);
    fclose(f);
    free(mu);
    if (wr != out_n) {
        ESP_LOGE(TAG, "short write to %s (%u/%u)", CUSTOM_PATH,
                 (unsigned)wr, (unsigned)out_n);
        return ESP_FAIL;
    }

    s_custom_n = out_n;
    nvs_save();
    custom_load();                    /* refresh the PSRAM cache + window */
    ESP_LOGI(TAG, "custom tail stored: %.2f s (%u mu-law bytes), "
             "%.2f s audible will be transmitted",
             (double)out_n / RATE, (unsigned)out_n,
             (double)s_custom_win / RATE);
    return ESP_OK;
}

/*
 * WAV: RIFF....WAVE with a PCM fmt chunk (16-bit, mono/stereo, 4.8..96 kHz)
 * and a data chunk.  Converted: resampled to 8 kHz mono, peak-normalised to
 * -2.5 dBFS, capped at 5 s, mu-law encoded (<= 40 KB) and stored in the
 * storage partition.
 */
static esp_err_t wav_store(const uint8_t *w, size_t len)
{
    if (len < 44 || memcmp(w, "RIFF", 4) != 0 || memcmp(w + 8, "WAVE", 4) != 0) {
        return ESP_ERR_INVALID_ARG;      /* not a RIFF/WAVE file */
    }
    uint16_t fmt = 0, ch = 0, bits = 0;
    uint32_t rate = 0;
    const uint8_t *data = NULL;
    size_t dlen = 0;
    size_t p = 12;
    while (p + 8 <= len) {
        const uint8_t *cp = w + p;
        uint32_t csz;
        memcpy(&csz, cp + 4, 4);
        const size_t avail = len - (p + 8);
        if (csz > avail) {
            csz = (uint32_t)avail;
        }
        if (memcmp(cp, "fmt ", 4) == 0 && csz >= 16) {
            memcpy(&fmt, cp + 8, 2);
            memcpy(&ch, cp + 10, 2);
            memcpy(&rate, cp + 12, 4);
            memcpy(&bits, cp + 22, 2);
        } else if (memcmp(cp, "data", 4) == 0) {
            data = cp + 8;
            dlen = csz;
            break;
        }
        p += 8 + csz + (csz & 1);
    }
    if (!data || dlen < 64 || fmt != 1 || bits != 16 ||
        (ch != 1 && ch != 2) || rate < 4800 || rate > 96000) {
        ESP_LOGW(TAG, "wav: fmt=%u ch=%u bits=%u rate=%u -- not 16-bit PCM",
                 fmt, ch, bits, (unsigned)rate);
        return ESP_ERR_INVALID_ARG;
    }

    const size_t nsamp = dlen / 2 / ch;
    if (nsamp < RATE / 5) {
        return ESP_ERR_INVALID_SIZE;     /* shorter than 0.2 s */
    }

    int16_t *pcm = heap_caps_malloc(CUSTOM_MAX_STORED * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!pcm) {
        return ESP_ERR_NO_MEM;
    }

    /* Linear resample to 8 kHz; take at most 5 s worth of source. */
    size_t out_n = 0;
    for (size_t i = 0; i < TAIL_CUSTOM_MAX_SAMPLES; ++i) {
        const double sp = (double)i * (double)rate / RATE;
        const size_t s0 = (size_t)sp;
        if (s0 >= nsamp) {
            break;
        }
        int v0, v1;
        if (ch == 1) {
            v0 = (int16_t)(data[s0 * 2] | (data[s0 * 2 + 1] << 8));
            v1 = (s0 + 1 < nsamp)
                 ? (int16_t)(data[(s0 + 1) * 2] | (data[(s0 + 1) * 2 + 1] << 8))
                 : v0;
        } else {
            const int16_t l0 = (int16_t)(data[s0 * 4] | (data[s0 * 4 + 1] << 8));
            const int16_t r0 = (int16_t)(data[s0 * 4 + 2] | (data[s0 * 4 + 3] << 8));
            v0 = (l0 + r0) / 2;
            if (s0 + 1 < nsamp) {
                const int16_t l1 = (int16_t)(data[(s0 + 1) * 4] | (data[(s0 + 1) * 4 + 1] << 8));
                const int16_t r1 = (int16_t)(data[(s0 + 1) * 4 + 2] | (data[(s0 + 1) * 4 + 3] << 8));
                v1 = (l1 + r1) / 2;
            } else {
                v1 = v0;
            }
        }
        const double fr = sp - (double)s0;
        pcm[i] = (int16_t)(v0 + (v1 - v0) * fr);
        ++out_n;
    }
    return custom_store_finish(pcm, out_n);
}

/*
 * MP3: MPEG-1/2 Layer III (mono/stereo, 8..48 kHz; CBR/VBR; a leading
 * ID3v2 tag is skipped by the frame scanner, a trailing ID3v1 simply ends
 * the stream).  Decoded with minimp3 (public domain) on a short-lived task
 * -- its scratch is ~16 KB of stack, far more than the httpd task has --
 * downmixed to mono, then the same resample / normalise / mu-law path as
 * WAV.  The handler waits for the task (30 s is a generous ceiling for a
 * 5 s clip on the S3).
 */
static int mp3_frame_samples(const mp3dec_frame_info_t *info, int got)
{
    if (!info->bitrate_kbps) {
        return 0;                       /* free format: not supported */
    }
    /* Samples per channel implied by the consumed frame, snapped to the
     * ISO values (384 / 576 / 1152). */
    const long f = (long)got * (long)info->hz /
                   (long)(info->bitrate_kbps * 125);
    static const int cand[3] = { 384, 576, 1152 };
    int best = 576;
    long bd = 1L << 30;
    for (int i = 0; i < 3; ++i) {
        const long d = f > cand[i] ? f - cand[i] : cand[i] - f;
        if (d < bd) {
            bd = d;
            best = cand[i];
        }
    }
    return best;
}

static size_t resample_mono(const int16_t *src, size_t nsamp, uint32_t rate,
                            int16_t *dst)
{
    size_t out_n = 0;
    for (size_t i = 0; i < TAIL_CUSTOM_MAX_SAMPLES; ++i) {
        const double sp = (double)i * (double)rate / RATE;
        const size_t s0 = (size_t)sp;
        if (s0 >= nsamp) {
            break;
        }
        const int v0 = src[s0];
        const int v1 = (s0 + 1 < nsamp) ? src[s0 + 1] : v0;
        const double fr = sp - (double)s0;
        dst[out_n] = (int16_t)(v0 + (v1 - v0) * fr);
        ++out_n;
    }
    return out_n;
}

typedef struct {
    uint8_t *data;      /* owned by the task */
    size_t len;
    esp_err_t err;
    int16_t *pcm;
    size_t samples;
    SemaphoreHandle_t done;
    atomic_uint refs;
    atomic_bool cancelled;
} mp3_job_t;

/* The worker never writes Flash. The last owner reclaims timed-out jobs. */
static void mp3_job_release(mp3_job_t *job)
{
    if (atomic_fetch_sub(&job->refs, 1) != 1) { return; }
    free(job->data);
    free(job->pcm);
    vSemaphoreDelete(job->done);
    free(job);
    atomic_store(&s_storing, false);
}

static void mp3_conv_task(void *arg)
{
    mp3_job_t *job = (mp3_job_t *)arg;
    esp_err_t err = ESP_ERR_INVALID_ARG;

    int16_t *pcm = heap_caps_malloc(CUSTOM_MAX_STORED * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    size_t out_n = 0;
    if (pcm) {
        /*
         * The decoder's scratch is ~16 KB of stack (mp3dec_decode_frame),
         * so keep everything else off the task stack: the decoder state
         * and the frame buffer are heap objects.
         */
        mp3dec_t *dec = heap_caps_malloc(sizeof(mp3dec_t), MALLOC_CAP_SPIRAM);
        int16_t *frame = (int16_t *)heap_caps_malloc(
                             2 * MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t),
                             MALLOC_CAP_SPIRAM);
        mp3dec_frame_info_t info;
        uint32_t rate = 0;
        size_t mono_n = 0, mono_cap = 0;
        int16_t *mono = NULL;
        size_t pos = 0;
        int frames = 0;
        bool ok = (dec != NULL && frame != NULL);
        if (!ok) { err = ESP_ERR_NO_MEM; }
        if (ok) {
            mp3dec_init(dec);
        }

        while (pos < job->len && ok && !atomic_load(&job->cancelled)) {
            const int got = mp3dec_decode_frame(dec, job->data + pos,
                                                (int)(job->len - pos),
                                                frame, &info);
            /* `got` is the per-channel sample count; the consumed byte
             * count (including any leading junk such as an ID3v2 tag on
             * the first call) is info->frame_bytes. */
            if (got <= 0 || !info.frame_bytes) {
                break;                  /* no (more) frame */
            }
            pos += (size_t)info.frame_bytes;
            if (!rate) {
                rate = info.hz;
                if (rate < 8000 || rate > 48000 || !info.bitrate_kbps) {
                    ESP_LOGW(TAG, "mp3: unsupported (hz=%u br=%u)",
                             (unsigned)rate,
                             (unsigned)info.bitrate_kbps);
                    ok = false;
                    break;
                }
                /* Decode no more source than 5 s of 8 kHz output. */
                mono_cap = (size_t)TAIL_CUSTOM_MAX_SAMPLES * rate / RATE + 2;
                mono = (int16_t *)heap_caps_malloc(mono_cap * sizeof(int16_t),
                                                  MALLOC_CAP_SPIRAM);
                if (!mono) {
                    err = ESP_ERR_NO_MEM;
                    ok = false;
                    break;
                }
            }
            const int spc = mp3_frame_samples(&info, got);
            const int nch = (info.channels == 1) ? 1 : 2;
            if (!spc || nch * spc > 2 * MINIMP3_MAX_SAMPLES_PER_FRAME) {
                ok = false;
                break;
            }
            for (int i = 0; i < spc && mono_n < mono_cap; ++i) {
                const int16_t l = frame[i * nch];
                const int16_t r = (nch == 2) ? frame[i * nch + 1] : l;
                mono[mono_n++] = (int16_t)(((int)l + (int)r) >> 1);
            }
            if (++frames > 20000) {     /* ~2.5 min of audio: be generous */
                ok = false;
                break;
            }
        }
        ESP_LOGI(TAG, "mp3: %d frames, %u samples @ %u Hz (%u bytes read)",
                 frames, (unsigned)mono_n, (unsigned)rate, (unsigned)pos);

        if (ok && mono && mono_n >= RATE / 5) {
            out_n = resample_mono(mono, mono_n, rate, pcm);
        }
        free(mono);
        free(frame);
        free(dec);
        if (ok && !atomic_load(&job->cancelled) && out_n >= RATE / 5) {
            job->pcm = pcm; job->samples = out_n;
            err = ESP_OK; pcm = NULL;
        }
    } else {
        err = ESP_ERR_NO_MEM;
    }
    if (pcm) {
        free(pcm);
    }
    if (atomic_load(&job->cancelled)) { err = ESP_ERR_TIMEOUT; }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mp3: store failed: %s", esp_err_to_name(err));
    }
    job->err = err;
    free(job->data);
    job->data = NULL;
    xSemaphoreGive(job->done);
    mp3_job_release(job);
    vTaskDeleteWithCaps(NULL);
}

#define MP3_TASK_STACK 24576
static esp_err_t mp3_store(const uint8_t *w, size_t len)
{
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_storing, &expected, true)) { return ESP_ERR_INVALID_STATE; }
    mp3_job_t *job = calloc(1, sizeof(*job));
    if (!job) { atomic_store(&s_storing, false); return ESP_ERR_NO_MEM; }
    job->done = xSemaphoreCreateBinary();
    job->data = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (!job->done || !job->data) {
        if (job->done) { vSemaphoreDelete(job->done); }
        free(job->data); free(job); atomic_store(&s_storing, false);
        return ESP_ERR_NO_MEM;
    }
    memcpy(job->data, w, len); job->len = len; job->err = ESP_ERR_INVALID_ARG;
    atomic_init(&job->refs, 2); atomic_init(&job->cancelled, false);
    TaskHandle_t task = NULL;
    if (xTaskCreateWithCaps(mp3_conv_task, "mp3conv", MP3_TASK_STACK, job, 10, &task,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        atomic_store(&job->refs, 1); mp3_job_release(job); return ESP_ERR_NO_MEM;
    }
    esp_err_t err = ESP_ERR_TIMEOUT;
    if (xSemaphoreTake(job->done, pdMS_TO_TICKS(30000)) == pdTRUE) {
        err = job->err;
        if (err == ESP_OK) {
            /* HTTP/console callers have INTERNAL stacks: persistence stays here. */
            int16_t *pcm = job->pcm; job->pcm = NULL;
            err = custom_store_finish(pcm, job->samples);
        }
    } else {
        atomic_store(&job->cancelled, true);
        ESP_LOGW(TAG, "mp3 conversion timed out; no background persistence");
    }
    mp3_job_release(job);
    return err;
}

esp_err_t tail_custom_store(const uint8_t *w, size_t len)
{
    if (len < 2) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len >= 44 && memcmp(w, "RIFF", 4) == 0 && memcmp(w + 8, "WAVE", 4) == 0) {
        bool expected = false;
        if (!atomic_compare_exchange_strong(&s_storing, &expected, true)) { return ESP_ERR_INVALID_STATE; }
        const esp_err_t err = wav_store(w, len);
        atomic_store(&s_storing, false);
        return err;
    }
    if ((w[0] == 0xFF && (w[1] & 0xE0) == 0xE0) ||
        (len >= 3 && w[0] == 'I' && w[1] == 'D' && w[2] == '3')) {
        return mp3_store(w, len);
    }
    return ESP_ERR_INVALID_ARG;          /* neither WAV nor MP3 */
}

/* ----------------------------------------------------------- fetching */

/*
 * Pure RAM reads: the built-in tones live in the PSRAM pool and the uploaded
 * tone lives in the PSRAM cache (custom_load), so this never blocks on flash
 * while the transmitter is keyed -- which is exactly what used to swallow the
 * beginning of the tone, and the whole tone on a short transmission.
 */
size_t tail_fetch(int id, size_t pos, int16_t *buf, size_t n)
{
    if (!buf || n == 0 || id == TAIL_OFF || id >= TAIL_COUNT) {
        return 0;
    }
    if (id == TAIL_CUSTOM) {
        if (!s_custom_pcm || s_custom_win == 0 || pos >= s_custom_win) {
            return 0;
        }
        if (n > s_custom_win - pos) {
            n = s_custom_win - pos;
        }
        memcpy(buf, s_custom_pcm + s_custom_off + pos, n * sizeof(int16_t));
        return n;
    }
    const size_t total = s_len[id];
    if (pos >= total) {
        return 0;
    }
    if (n > total - pos) {
        n = total - pos;
    }
    if (!s_pool) {
        return 0;
    }
    memcpy(buf, &s_pool[s_off[id] + pos], n * sizeof(int16_t));
    return n;
}

/* ------------------------------------------------------------- preview */

static volatile int s_prev_id = 0;
static volatile size_t s_prev_pos = 0;

void tail_preview_arm(int id)
{
    ptt_control_lock();
    if (ptt_local_audio_active()) { ptt_control_unlock(); return; }
    if (id < 0 || id >= TAIL_COUNT || id == TAIL_OFF) {
        id = TAIL_OFF;
    }
    if (id == TAIL_CUSTOM && s_custom_win == 0) {
        id = TAIL_OFF;
    }
    s_prev_pos = 0;
    s_prev_id = id;
    ptt_control_unlock();
    ESP_LOGI(TAG, "preview: %s", tail_name(id));
}

void tail_preview_cancel(void)
{
    ptt_control_lock();
    s_prev_id = 0;
    s_prev_pos = 0;
    ptt_control_unlock();
}

bool tail_preview_active(void)
{
    ptt_control_lock();
    const bool active = s_prev_id != 0;
    ptt_control_unlock();
    return active;
}

size_t tail_preview_fetch(int16_t *buf, size_t n)
{
    ptt_control_lock();
    const int id = s_prev_id;
    if (!id) {
        ptt_control_unlock();
        return 0;
    }
    const size_t got = tail_fetch(id, s_prev_pos, buf, n);
    if (!got) {
        s_prev_id = 0;
        s_prev_pos = 0;
        ptt_control_unlock();
        return 0;
    }
    s_prev_pos += got;
    /* The custom tone may be shorter than the previous fetch position. */
    if (s_prev_pos >= tail_samples(id)) {
        s_prev_id = 0;
        s_prev_pos = 0;
    }
    ptt_control_unlock();
    return got;
}
