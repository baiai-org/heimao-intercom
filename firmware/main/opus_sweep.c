/*
 * opus_sweep -- what does more bandwidth actually buy on this link?
 *
 * The relay runs Opus at 12 kHz / 40 ms / CBR 24 kbit/s / complexity 0.  Before
 * spending bandwidth (or CPU) on "better quality" it is worth measuring what
 * each knob does to a speech-like signal, because the source is a narrowband FM
 * radio: above a certain point the codec is no longer what limits the audio.
 *
 * The test signal is synthesised here: a 120 Hz glottal pulse train through two
 * formant resonators (F1 600 Hz, F2 1500 Hz), amplitude-modulated into 250 ms
 * syllables with pauses, plus a little noise.  That is close enough to voiced
 * speech for the encoder to make the same decisions it makes on the real thing.
 *
 * For each configuration the signal is encoded and decoded frame by frame and
 * the segmental SNR is reported, with a plus/minus one frame lag search because
 * Opus has an algorithmic delay of a few milliseconds.  Higher is better; below
 * ~15 dB the artefacts are audible, and past ~25 dB the source is the limit.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "opus.h"
#include "opus_link.h"   /* OPUS_LINK_RATE_HZ / FRAME_MS / BITRATE */

static const char *TAG = "opus_sweep";

#define SW_RATE   OPUS_LINK_RATE_HZ   /* follow the real stream, not a stale 12 kHz */
#define SW_MAXN   720                  /* 60 ms, the longest frame we test */
#define SW_HIST   4                    /* frames kept for the lag search */

typedef struct {
    uint16_t frame_ms;
    uint16_t kbps;
    uint8_t  complexity;
} sw_cfg_t;

/* Two biquad resonators, state carried across frames. */
typedef struct {
    float y1a, y2a, y1b, y2b;
} sw_res_t;

static void sw_gen(int16_t *out, int n, int *t, sw_res_t *r, unsigned *seed)
{
    const float r1 = 0.990f, th1 = 2.0f * (float)M_PI * 600.0f / SW_RATE;
    const float r2 = 0.985f, th2 = 2.0f * (float)M_PI * 1500.0f / SW_RATE;
    const float c1 = 2.0f * r1 * cosf(th1), d1 = r1 * r1;
    const float c2 = 2.0f * r2 * cosf(th2), d2 = r2 * r2;

    for (int i = 0; i < n; ++i) {
        const int tt = (*t)++;
        /* glottal excitation: one pulse per 100 samples = 120 Hz */
        float ex = ((tt % 100) == 0) ? 1.0f : 0.0f;
        *seed = (*seed) * 1103515245u + 12345u;
        ex += (((float)((*seed >> 16) & 0xff)) - 128.0f) * (1.0f / 128.0f) * 0.01f;

        /* syllable envelope: 250 ms on, 125 ms off, 15 ms edges */
        const int period = SW_RATE / 3;              /* 333 ms */
        const int ph = tt % period;
        float env = 0.0f;
        const int on = SW_RATE / 4;                  /* 250 ms voiced */
        if (ph < on) {
            env = 1.0f;
            if (ph < SW_RATE * 15 / 1000) {
                env = (float)ph / (float)(SW_RATE * 15 / 1000);
            } else if (ph > on - SW_RATE * 15 / 1000) {
                env = (float)(on - ph) / (float)(SW_RATE * 15 / 1000);
            }
        }
        ex *= env;

        const float ya = ex + c1 * r->y1a - d1 * r->y2a;
        r->y2a = r->y1a;
        r->y1a = ya;
        const float yb = ya + c2 * r->y1b - d2 * r->y2b;
        r->y2b = r->y1b;
        r->y1b = yb;

        float v = yb * 7000.0f;
        if (v > 32000.0f) { v = 32000.0f; }
        if (v < -32000.0f) { v = -32000.0f; }
        out[i] = (int16_t)v;
    }
}

/*
 * One configuration: returns segmental SNR in dB, sets *enc_us to the mean
 * encode time per frame and *pkt to the largest packet produced.
 */
static double sw_run(const sw_cfg_t *c, double *enc_us, int *pkt)
{
    const int n = SW_RATE * c->frame_ms / 1000;
    int err = OPUS_OK;
    OpusEncoder *enc = opus_encoder_create(SW_RATE, 1, OPUS_APPLICATION_VOIP, &err);
    OpusDecoder *dec = opus_decoder_create(SW_RATE, 1, &err);
    if (!enc || !dec) {
        opus_encoder_destroy(enc);
        opus_decoder_destroy(dec);
        return -1.0;
    }
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(c->kbps * 1000));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(c->complexity));
    opus_encoder_ctl(enc, OPUS_SET_VBR(0));
    opus_encoder_ctl(enc, OPUS_SET_DTX(0));
    opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC(0));
    opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));

    /*
     * Input history, long enough to try every candidate delay.  Opus delays its
     * output by a few milliseconds (6.5 ms of lookahead plus the decoder's own
     * pipeline); at 12 kHz that is around 80 samples, so comparing a decoded
     * frame against the input frame at the same index measures alignment, not
     * quality -- which is exactly why the first version of this sweep reported
     * ~2 dB for every bitrate.
     */
    static int16_t hist[SW_MAXN * 2];
    static int16_t in[SW_MAXN];
    static int16_t out[SW_MAXN];
    static uint8_t pktbuf[1024];
    const int hist_n = (int)(sizeof(hist) / sizeof(hist[0]));
    memset(hist, 0, sizeof(hist));

    const int frames = (SW_RATE * 3) / n;          /* three seconds of audio */
    long long sig = 0, noise = 0;
    long long t_enc = 0;
    int delay = -1;                                 /* samples, found once */
    sw_res_t res = {0};
    unsigned seed = 999;
    int t = 0;
    int measured = 0;

    for (int f = 0; f < frames; ++f) {
        sw_gen(in, n, &t, &res, &seed);

        const int64_t a = esp_timer_get_time();
        const int len = opus_encode(enc, in, n, pktbuf, (opus_int32)sizeof(pktbuf));
        t_enc += esp_timer_get_time() - a;
        if (len <= 0) {
            break;
        }
        if (len > *pkt) {
            *pkt = len;
        }
        if (opus_decode(dec, pktbuf, len, out, n, 0) != n) {
            break;
        }

        /* Slide the history and append this frame. */
        memmove(hist, hist + n, (size_t)(hist_n - n) * sizeof(hist[0]));
        memcpy(hist + hist_n - n, in, (size_t)n * sizeof(hist[0]));

        if (f < 3) {                     /* let the codec fill its state */
            continue;
        }
        /* Find the delay once, then keep it: it is a property of the codec. */
        if (delay < 0) {
            double best = 1e30;
            for (int d = 0; d <= 200 && d + n <= hist_n; ++d) {
                const int16_t *ref = hist + hist_n - n - d;
                long long s2 = 0, e2 = 0;
                for (int i = 0; i < n; ++i) {
                    const long long x = ref[i];
                    const long long df = (long long)out[i] - x;
                    s2 += x * x;
                    e2 += df * df;
                }
                if (s2 > 0) {
                    const double r = (double)e2 / (double)s2;
                    if (r < best) { best = r; delay = d; }
                }
            }
            if (delay < 0) {
                delay = 0;
            }
        }
        const int16_t *ref = hist + hist_n - n - delay;
        for (int i = 0; i < n; ++i) {
            const long long x = ref[i];
            const long long df = (long long)out[i] - x;
            sig += x * x;
            noise += df * df;
        }
        ++measured;
    }

    *enc_us = (double)t_enc / (double)frames;

    double snr = -1.0;
    if (sig > 0 && noise > 0) {
        snr = 10.0 * log10((double)sig / (double)noise);
    } else if (sig > 0) {
        snr = 99.0;
    }
    opus_encoder_destroy(enc);
    opus_decoder_destroy(dec);
    return snr;
}

const char *opus_sweep(char *out, size_t n)
{
    /*
     * The matrix now centres on what the board actually runs (16 kHz, 20 ms,
     * 32 kbit/s) and varies one axis at a time, so the answer to "is the hiss
     * under speech the codec?" is a number rather than an opinion.
     */
    static const sw_cfg_t cfgs[] = {
        { 20, 32, 0 },   /* current */  { 20, 32, 2 },  { 20, 24, 0 },
        { 20, 48, 0 },   { 40, 32, 0 },  { 40, 32, 2 },
        { 40, 24, 0 },   { 40, 48, 0 },  { 60, 32, 0 },
    };
    size_t off = 0;
    off += (size_t)snprintf(out + off, n - off, "segmental SNR, %d Hz mono, 3 s of synthetic voiced speech; "
                                               "current = %u ms / %u kbit/s / c0\n",
                               OPUS_LINK_RATE_HZ, OPUS_LINK_FRAME_MS, OPUS_LINK_BITRATE / 1000);
    for (size_t i = 0; i < sizeof(cfgs) / sizeof(cfgs[0]) && off < n - 80; ++i) {
        double us = 0;
        int pkt = 0;
        const double snr = sw_run(&cfgs[i], &us, &pkt);
        const double cpu = us / ((double)cfgs[i].frame_ms * 10.0);          /* % of one core */
        off += (size_t)snprintf(out + off, n - off,
                                "%2u ms %2u kbps c%u: %5.1f dB, %3d B/frame, %4.2f KB/s, enc %5.1f%%  %s\n",
                                cfgs[i].frame_ms, cfgs[i].kbps, cfgs[i].complexity,
                                snr, pkt, (double)cfgs[i].kbps * 1000.0 / 8.0 / 1024.0, cpu,
                                (cfgs[i].frame_ms == OPUS_LINK_FRAME_MS &&
                                 cfgs[i].kbps == OPUS_LINK_BITRATE / 1000 &&
                                 cfgs[i].complexity == 0) ? "<== current" : "");
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return out;
}

static SemaphoreHandle_t s_done;
static char s_out[1024];

static void sweep_task(void *arg)
{
    (void)arg;
    opus_sweep(s_out, sizeof(s_out));
    xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}

const char *opus_sweep_task(char *out, size_t n, uint32_t timeout_ms)
{
    if (!s_done) {
        s_done = xSemaphoreCreateBinary();
    }
    if (!s_done) {
        snprintf(out, n, "no semaphore");
        return out;
    }
    (void)xSemaphoreTake(s_done, 0);
    s_out[0] = '\0';
    TaskHandle_t th = NULL;
    if (xTaskCreateWithCaps(sweep_task, "opussweep", 32768, NULL, 5, &th,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        snprintf(out, n, "could not start sweep task");
        return out;
    }
    if (xSemaphoreTake(s_done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        snprintf(out, n, "sweep timeout");
        return out;
    }
    snprintf(out, n, "%s", s_out);
    return out;
}
