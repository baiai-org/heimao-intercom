/*
 * LAN control panel -- implementation.
 *
 * Design notes, taken from the official references rather than invented:
 *
 *   - The server configuration follows examples/protocols/http_server/simple
 *     but overrides three defaults that matter here:
 *       lru_purge_enable = true    (a browser opens several connections at
 *                                   once; without this the sockets run out and
 *                                   the panel silently stops answering)
 *       stack_size       = 8192    (the status handler builds a JSON string)
 *       max_uri_handlers = 16      (the default 8 leaves no room to grow, and
 *                                   the route table needs 13 -- at 12 the last
 *                                   one, /api/done, silently failed to
 *                                   register; see net_http_start())
 *
 *   - Query values are NOT URL-decoded by httpd_query_key_value, as the API
 *     reference states.  Every value this panel sends is a number or a short
 *     token, so nothing that needs decoding is accepted.
 *
 *   - The transmitter is never one click away: /api/talk requires an explicit
 *     confirmation parameter, the duration is capped, and a timer releases PTT
 *     even if the browser disappears.  The firmware's own absolute ceiling in
 *     ptt.c still applies underneath.
 */
#include "net_http.h"
#include "brand.h"
#include "audio_i2s.h"   /* AUDIO_SAMPLE_RATE_HZ / AUDIO_PKT_SAMPLES */

#include <stdlib.h>
#include <inttypes.h>
#include <math.h>
#include <string.h>
#include <strings.h>

#include "audio_codec.h"
#include "board_hw.h"
#include "opus_link.h"
#include "opus_sweep.h"
#include "bridge.h"
#include "ctcss.h"
#include "channels.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_https_server.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_image_format.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net_wifi.h"
#include "device_identity.h"
#include "device_name.h"
#include "app_control.h"
#include "cJSON.h"
#include "panel_address.h"
#include "nvs_flash.h"
#include "ptt.h"
#include "sa818.h"
#include "sa818_cfg.h"
#include "sdr.h"
#include "voice.h"
#include "voice_tail.h"
#include "netcom.h"
#include "netp2pnet.h"
#include "frontpanel.h"
#include "radio_activity.h"
#include "speaker_diag.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

static const char *TAG = "http";

/* The panel reports its own view of the SDR link (see h_sdr_report);
 * /api/status mirrors it so the link can be diagnosed remotely. */
#define SDR_REP_LEN 320   /* the panel's report query outgrew 160 (it carries code= and host= at the end) */
static char s_sdr_rep[SDR_REP_LEN];

/* ------------------------------------------------------------------ log tail
 * /api/status carries the last few log lines: a client that cannot connect
 * (an iPhone rejecting the self-signed certificate, say) never gets to report
 * anything itself, and the only place the reason appears is this log.
 */
#define LOGTAIL_LEN 400
static char s_logtail[LOGTAIL_LEN];
static char s_logerr[LOGTAIL_LEN * 2];
static vprintf_like_t s_prev_vprintf;
/* ESP-IDF invokes this hook concurrently from both CPUs. Protect each ring
 * update and status read; otherwise strlen/memmove can race an append. */
static portMUX_TYPE s_log_mux = portMUX_INITIALIZER_UNLOCKED;

static int logtail_vprintf(const char *fmt, va_list ap)
{
    va_list cp;
    va_copy(cp, ap);
    char line[160];
    const int n = vsnprintf(line, sizeof(line), fmt, cp);
    va_end(cp);
    if (n > 0) {
        portENTER_CRITICAL(&s_log_mux);
        size_t cur = strlen(s_logtail);
        size_t add = strlen(line);
        if (add >= sizeof(s_logtail)) {
            memcpy(s_logtail, line + add - (sizeof(s_logtail) - 1),
                   sizeof(s_logtail) - 1);
            s_logtail[sizeof(s_logtail) - 1] = '\0';
        } else {
            if (cur + add >= sizeof(s_logtail)) {
                const size_t drop = cur + add - (sizeof(s_logtail) - 1);
                memmove(s_logtail, s_logtail + drop, cur - drop + 1);
                cur = strlen(s_logtail);
            }
            memcpy(s_logtail + cur, line, add + 1);
        }
        /* A second ring with just the connection-related lines: the plain tail
         * is overwritten by the I2S/SA818 chatter within a second, so a failed
         * TLS handshake from a client would be gone before anyone could read
         * it.  Keep those (and every error) for a few minutes. */
        static const char *const keys[] = { "ssl", "tls", "httpd", "websocket",
                                            "handshake", "cert", "accept", "E (", "UPGRADE",
                                            /* netp2p link lifecycle: peer add/del, BYE,
                                             * LINKED, blocked -- needed to see whether a
                                             * delete really reached the other board */
                                            "np2p", "BYE", "blocked", "LINKED",
                                            /* module squelch / profile refresh:
                                             * which edge fired and why */
                                            "SA818 refresh", "re-applied",
                                            "squelch", "module squelch",
                                            /* NET-P2P: why a link died */
                                            "quiet for", "tombstoned", "LINKED",
                                            "re-anchored", "public mapping",
                                            "keeps LAN address",
                                            /* Wi-Fi drops explain whole-board stalls */
                                            "disconnected", "stack_min=" };
        bool keep = false;
        for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); ++k) {
            if (strstr(line, keys[k])) { keep = true; break; }
        }
        if (keep) {
            size_t c2 = strlen(s_logerr);
            size_t a2 = strlen(line);
            if (a2 >= sizeof(s_logerr)) {
                memcpy(s_logerr, line + a2 - (sizeof(s_logerr) - 1),
                       sizeof(s_logerr) - 1);
                s_logerr[sizeof(s_logerr) - 1] = '\0';
            } else {
                if (c2 + a2 >= sizeof(s_logerr)) {
                    const size_t drop = c2 + a2 - (sizeof(s_logerr) - 1);
                    memmove(s_logerr, s_logerr + drop, c2 - drop + 1);
                    c2 = strlen(s_logerr);
                }
                memcpy(s_logerr + c2, line, a2 + 1);
            }
        }
        portEXIT_CRITICAL(&s_log_mux);
    }
    return s_prev_vprintf ? s_prev_vprintf(fmt, ap) : 0;
}



/* Hard cap for one web-triggered transmission.  The firmware's absolute limit
 * is longer on purpose; this is the panel's own, tighter bound. */
#define WEB_TALK_MAX_S 60
#define WEB_TALK_DEF_S 30

/* ------------------------------------------------------------------- panel */

/*
 * The page is a single self-contained document: no external assets, no CDN, no
 * build step.  It polls /api/status once a second -- WebSocket support is not
 * enabled in this build (CONFIG_HTTPD_WS_SUPPORT is off), and for a control
 * panel that is a fine trade for the simplicity.
 */
extern const char index_html_start[] asm("_binary_panel_index_html_start");
extern const uint8_t brand_icon_start[] asm("_binary_brand_icon_png_start");
extern const uint8_t brand_icon_end[] asm("_binary_brand_icon_png_end");
extern const uint8_t favicon_start[] asm("_binary_favicon_png_start");
extern const uint8_t favicon_end[] asm("_binary_favicon_png_end");

static esp_err_t h_brand_icon(httpd_req_t *req)
{
    httpd_resp_set_type(req, "image/png");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, (const char *)brand_icon_start, brand_icon_end-brand_icon_start);
}

static esp_err_t h_favicon(httpd_req_t *req)
{
    httpd_resp_set_type(req, "image/png");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, (const char *)favicon_start, favicon_end-favicon_start);
}

static esp_err_t h_brand_script(httpd_req_t *req)
{
    static const char script[] =
        "window.HMBrand=Object.freeze({project:'" HM_PROJECT_NAME "',device:'" HM_DEVICE_NAME
        "',panel:'" HM_PANEL_NAME "',app:'" HM_APP_NAME "',setupPrefix:'" HM_SETUP_PREFIX "'});"
        "function hmApplyBrand(){document.querySelectorAll('[data-hm]').forEach(e=>{e.textContent=HMBrand[e.dataset.hm]||e.textContent});"
        "const t=document.querySelector('title[data-hm-title]');if(t)document.title=HMBrand.panel+(t.dataset.hmTitle?' · '+t.dataset.hmTitle:'')}"
        "if(document.readyState==='loading')document.addEventListener('DOMContentLoaded',hmApplyBrand);else hmApplyBrand();";
    httpd_resp_set_type(req, "application/javascript; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, script, HTTPD_RESP_USE_STRLEN);
}

/* ------------------------------------------------------------------ helpers */

static int query_int(httpd_req_t *req, const char *key, int fallback,
                     bool *found)
{
    char qry[256];
    char val[32];
    if (found) { *found = false; }
    if (httpd_req_get_url_query_len(req) == 0) {
        return fallback;
    }
    if (httpd_req_get_url_query_str(req, qry, sizeof(qry)) != ESP_OK) {
        return fallback;
    }
    if (httpd_query_key_value(qry, key, val, sizeof(val)) != ESP_OK) {
        return fallback;
    }
    if (found) { *found = true; }
    return atoi(val);
}

static void query_str(httpd_req_t *req, const char *key, char *out, size_t len)
{
    char qry[256];
    out[0] = '\0';
    if (httpd_req_get_url_query_len(req) == 0) {
        return;
    }
    if (httpd_req_get_url_query_str(req, qry, sizeof(qry)) != ESP_OK) {
        return;
    }
    (void)httpd_query_key_value(qry, key, out, len);
}

static esp_err_t send_json(httpd_req_t *req, const char *body)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t send_result(httpd_req_t *req, bool ok, const char *err)
{
    char buf[160];
    if (ok) {
        snprintf(buf, sizeof(buf), "{\"ok\":true}");
    } else {
        snprintf(buf, sizeof(buf), "{\"ok\":false,\"err\":\"%s\"}",
                 err ? err : "failed");
    }
    return send_json(req, buf);
}

/*
 * POST /api/tail_upload -- the body is a WAV or MP3 file (raw, no multipart).
 *
 * The browser sends the file's bytes verbatim; the firmware does the format
 * conversion itself (resample to 8 kHz mono, normalise, truncate to 5 s,
 * mu-law encode, store in the storage partition).  A 512 KB cap keeps the
 * worst case at about 11 s of 44.1 kHz stereo -- enough for any real
 * sign-off clip without risking a flash-full upload.
 */
static esp_err_t h_tail_upload(httpd_req_t *req)
{
    /* Reject before allocating or receiving: httpd already parsed the exact
     * Content-Length, and the converter has a deliberate 512 KiB cap. */
    if (req->content_len == 0) {
        return send_result(req, false, "empty body");
    }
    if (req->content_len > (size_t)TAIL_UPLOAD_MAX) {
        return send_result(req, false, "file too large (max 512 KB)");
    }
    if (sdr_enabled() || voice_mode() == VOICE_TALK || ptt_get_state() != PTT_IDLE || ptt_owner() != PTT_OWNER_NONE) {
        char msg[160];
        if (sdr_enabled()) {
            snprintf(msg, sizeof(msg), "SDR收听流仍在运行：上传前请先在APP或对讲页停止收听");
        } else if (voice_mode() == VOICE_TALK) {
            snprintf(msg, sizeof(msg), "手机通话仍在进行：请先结束通话再上传");
        } else if (ptt_get_state() != PTT_IDLE) {
            snprintf(msg, sizeof(msg), "PTT状态机停在%s：请先松开PTT，或断电重启手台", ptt_state_name());
        } else {
            snprintf(msg, sizeof(msg), "PTT被%s占用：请先结束通话或收听，或断电重启手台", ptt_owner_name());
        }
        return send_result(req, false, msg);
    }

    /* A sized allocation avoids reserving the full cap for every small clip.
     * With SPIRAM_USE_MALLOC enabled, a normal malloc of this size is placed
     * in PSRAM, leaving internal DRAM for Wi-Fi, TLS and the audio path. */
    char *buf = malloc(req->content_len);
    if (!buf) {
        return send_result(req, false, "no memory");
    }
    size_t total = 0;
    size_t left = req->content_len;
    const int64_t upload_started = esp_timer_get_time();
    int64_t last_progress = upload_started;
    /* Keep the application read bounded even though the underlying HTTPS
     * transport now accepts standard 16 KiB TLS records. */
    while (left) {
        const int64_t now = esp_timer_get_time();
        if (now-last_progress > 15000000 || now-upload_started > 180000000) {
            free(buf); return send_result(req, false, "upload timed out");
        }
        const size_t want = left < 4096u ? left : 4096u;
        const int r = httpd_req_recv(req, buf + total, want);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            ESP_LOGW(TAG, "tail upload recv failed after %u/%u bytes (%d)",
                     (unsigned)total, (unsigned)req->content_len, r);
            free(buf);
            return send_result(req, false, "recv failed");
        }
        total += (size_t)r;
        left -= (size_t)r;
        last_progress = esp_timer_get_time();
    }

    esp_err_t e = tail_custom_store((const uint8_t *)buf, total);
    free(buf);
    char body[128];
    if (e == ESP_OK) {
        snprintf(body, sizeof(body),
                 "{\"ok\":true,\"seconds\":%.2f,\"bytes\":%u}",
                 (double)tail_custom_samples() / (double)AUDIO_SAMPLE_RATE_HZ,
                 (unsigned)tail_custom_samples());
        return send_json(req, body);
    }
    if (e == ESP_ERR_INVALID_ARG) {
        return send_result(req, false,
            "需要 WAV（16-bit PCM，4.8–96 kHz）或 MP3（32–48 kHz）");
    }
    if (e == ESP_ERR_INVALID_SIZE) {
        return send_result(req, false, "音频太短（至少 0.2 秒）");
    }
    if (e == ESP_ERR_INVALID_STATE) {
        return send_result(req, false, "上一个文件还在转换，稍后再试");
    }
    return send_result(req, false, "存储失败");
}

/* --------------------------------------------------------------- provisioning */

/*
 * Shown when the board has no network yet.  Deliberately tiny and dependency
 * free: it is served to whatever phone happens to connect to the setup AP.
 */
static esp_err_t h_setup(httpd_req_t *req)
{
    extern const char setup_html[] asm("_binary_setup_html_start");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    /* Panels get reloaded while they are being changed; a cached copy would
     * show the previous build's markup and look like the change did nothing. */
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, setup_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_control_script(httpd_req_t *req)
{
    extern const char script[] asm("_binary_app_control_js_start");
    httpd_resp_set_type(req, "application/javascript; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, script, HTTPD_RESP_USE_STRLEN);
}

/*
 * Scan results, so the setup page can offer a list instead of asking someone
 * to type an SSID on a phone keyboard.
 *
 * Allocated on the heap rather than on the handler's stack: 32 AP records is
 * about 3.5 KB, and the httpd stack is 6 KB.
 */
static esp_err_t h_scan(httpd_req_t *req)
{
    const size_t cap = 4000;
    char *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        return send_result(req, false, "no memory");
    }
    /* A page load gets the cache; the rescan button asks for a real scan. */
    const bool force = query_int(req, "force", 0, NULL) != 0;
    const esp_err_t err = net_wifi_scan_json(buf, cap, force);
    if (err != ESP_OK) {
        free(buf);
        return send_result(req, false, "scan failed");
    }
    const esp_err_t s = send_json(req, buf);
    free(buf);
    return s;
}

/* Percent-decode in place; the SSID and password arrive URL-encoded. */
static void url_decode(char *s)
{
    char *o = s;
    for (char *i = s; *i; ++i) {
        if (*i == '%' && i[1] && i[2]) {
            char hex[3] = { i[1], i[2], 0 };
            *o++ = (char)strtol(hex, NULL, 16);
            i += 2;
        } else if (*i == '+') {
            *o++ = ' ';
        } else {
            *o++ = *i;
        }
    }
    *o = '\0';
}

static esp_err_t h_provision(httpd_req_t *req)
{
    if (req->method != HTTP_POST) {
        httpd_resp_set_status(req, "405 Method Not Allowed");
        return send_result(req, false, "use POST JSON; credentials must not be in URLs");
    }
    cJSON *body = app_read_json(req);
    if (!body) { return ESP_FAIL; }
    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(body, "ssid");
    const cJSON *pass = cJSON_GetObjectItemCaseSensitive(body, "password");
    const cJSON *host = cJSON_GetObjectItemCaseSensitive(body, "host");
    uint32_t id = 0;
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (cJSON_IsString(ssid) && cJSON_IsString(pass) && (!host || cJSON_IsString(host))) {
        err = net_wifi_profile_save(&id, ssid->valuestring, pass->valuestring, true, 0, host ? host->valuestring : NULL);
    }
    cJSON_Delete(body);
    if (err != ESP_OK) { return send_result(req, false, esp_err_to_name(err)); }
    char reply[128];
    snprintf(reply, sizeof(reply), "{\"ok\":true,\"saved\":true,\"connected\":false,\"scheduled\":true,\"id\":%lu}", (unsigned long)id);
    /* No driver change is scheduled until the response has been sent. The
     * following 1.5 s allows the phone to leave this AP and enable tethering. */
    esp_err_t sent = send_json(req, reply);
    if (sent == ESP_OK) { (void)net_wifi_profile_connect(id, 1500); }
    return sent;
}

static void json_escape_str(const char *in, char *out, size_t out_len);

static esp_err_t h_device_name_script(httpd_req_t *req)
{
    extern const char device_name_script[] asm("_binary_device_name_js_start");
    httpd_resp_set_type(req, "application/javascript; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, device_name_script, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_device_name(httpd_req_t *req)
{
    cJSON *body = app_read_json(req);
    if (!body) { return ESP_FAIL; }
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(body, "custom_name");
    const cJSON *revision = cJSON_GetObjectItemCaseSensitive(body, "name_revision");
    device_name_t current;
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (cJSON_IsString(value) && cJSON_IsNumber(revision) && revision->valuedouble >= 0 &&
        revision->valuedouble <= UINT32_MAX && floor(revision->valuedouble) == revision->valuedouble) {
        /* Trim ASCII spaces; clients also normalize Unicode and trim input. */
        char *name = value->valuestring;
        while (*name == ' ') { ++name; }
        size_t n = strlen(name);
        while (n && name[n - 1] == ' ') { name[--n] = 0; }
        err = device_name_set(name, (uint32_t)revision->valuedouble, &current);
    }
    cJSON_Delete(body);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, err == ESP_ERR_INVALID_ARG ? "400 Bad Request" :
                                   err == ESP_ERR_INVALID_STATE ? "409 Conflict" : "500 Internal Server Error");
        return send_result(req, false, err == ESP_ERR_INVALID_STATE ? "name changed or storage unavailable; refresh before saving" : esp_err_to_name(err));
    }
    /* Read back the canonical value only after NVS commit succeeded. */
    device_name_get(&current);
    net_wifi_name_changed();
    char escaped[200], reply[360];
    json_escape_str(current.name, escaped, sizeof(escaped));
    snprintf(reply, sizeof(reply), "{\"ok\":true,\"device_id\":\"%s\",\"custom_name\":\"%s\",\"name_revision\":%lu}",
             device_identity_id(), escaped, (unsigned long)current.revision);
    return send_json(req, reply);
}

static esp_err_t h_device(httpd_req_t *req)
{
    device_name_t name; device_name_get(&name);
    char escaped[200]; json_escape_str(name.name, escaped, sizeof(escaped));
    char reply[1500];
    snprintf(reply, sizeof(reply),
        "{\"project\":\"" HM_PROJECT_NAME "\",\"device_name\":\"" HM_DEVICE_NAME "\","
        "\"panel_name\":\"" HM_PANEL_NAME "\",\"app_name\":\"" HM_APP_NAME "\","
        "\"device_id\":\"%s\",\"custom_name\":\"%s\",\"name_revision\":%lu,\"model\":\"ESP32-S3-N16R8\",\"firmware\":\"%s\",\"api_version\":2,"
        "\"capabilities\":{\"saved_wifi\":true,\"recovery_ms\":60000,\"control_lease\":true,"
        "\"channels\":99,\"ota\":true,\"files\":true,\"audio\":true,\"json_settings\":true,\"audio_clients\":1,\"radio_activity\":1,\"setup_session\":1,\"custom_name\":1},"
        "\"audio\":{\"format\":\"pcm_s16le\",\"rate\":16000,\"channels\":1,\"websocket\":\"/ws/sdr\"},"
        "\"discovery\":{\"mdns\":\"_openradiolink._tcp\",\"gateway_udp\":54322}}",
        device_identity_id(), escaped, (unsigned long)name.revision, esp_app_get_description()->version);
    return send_json(req, reply);
}

static uint32_t json_profile_id(const cJSON *body)
{
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(body, "id");
    return cJSON_IsNumber(id) && id->valuedouble >= 1 && id->valuedouble <= UINT32_MAX &&
           floor(id->valuedouble) == id->valuedouble ? (uint32_t)id->valuedouble : 0;
}

/* A setup hold is available before acquiring radio/settings control, but only
 * through the AP interface and after identifying this particular handset. */
static bool setup_local_address(httpd_req_t *req)
{
    struct sockaddr_storage local = {0}; socklen_t size = sizeof(local);
    if (getsockname(httpd_req_to_sockfd(req), (struct sockaddr *)&local, &size) != 0) { return false; }
    const uint32_t expected = inet_addr(net_wifi_ap_ip());
    if (local.ss_family == AF_INET) {
        return ((struct sockaddr_in *)&local)->sin_addr.s_addr == expected;
    }
#if CONFIG_LWIP_IPV6
    if (local.ss_family == AF_INET6) {
        /* HTTP(S) listens on a dual-stack socket: IPv4 AP clients can have
         * IPv4-mapped local addresses. Native IPv6 is not the AP's IPv4 route. */
        const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)&local;
        const uint8_t mapped[12] = {0,0,0,0,0,0,0,0,0,0,0xff,0xff};
        return !memcmp(v6->sin6_addr.s6_addr, mapped, sizeof(mapped)) &&
            !memcmp(v6->sin6_addr.s6_addr + 12, &expected, sizeof(expected));
    }
#endif
    return false;
}

static esp_err_t h_setup_session(httpd_req_t *req)
{
    char device[64] = {0};
    if (!net_wifi_setup_ap_active() || !setup_local_address(req) ||
        httpd_req_get_hdr_value_str(req, "X-ORL-Device", device, sizeof(device)) != ESP_OK ||
        strcmp(device, device_identity_id())) {
        httpd_resp_set_status(req, "409 Conflict");
        return send_result(req, false, "setup session requires this handset's AP connection");
    }
    cJSON *body = app_read_json(req);
    if (!body) { return ESP_FAIL; }
    const cJSON *op = cJSON_GetObjectItemCaseSensitive(body, "op");
    const cJSON *session = cJSON_GetObjectItemCaseSensitive(body, "session");
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (cJSON_IsString(op) && cJSON_IsString(session) &&
        (!strcmp(op->valuestring, "renew") || !strcmp(op->valuestring, "release"))) {
        err = net_wifi_setup_session(session->valuestring, !strcmp(op->valuestring, "release"));
    }
    cJSON_Delete(body);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, err == ESP_ERR_INVALID_ARG ? "400 Bad Request" : "409 Conflict");
        return send_result(req, false, esp_err_to_name(err));
    }
    return send_json(req, "{\"ok\":true,\"timeout_ms\":60000}");
}

static esp_err_t h_wifi(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        char *out = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!out) { return send_result(req, false, "no memory"); }
        esp_err_t err = net_wifi_profiles_json(out, 8192);
        esp_err_t sent = err == ESP_OK ? send_json(req, out) : send_result(req, false, esp_err_to_name(err));
        free(out); return sent;
    }
    cJSON *body = app_read_json(req);
    if (!body) { return ESP_FAIL; }
    const cJSON *op = cJSON_GetObjectItemCaseSensitive(body, "op");
    uint32_t id = json_profile_id(body);
    if (cJSON_GetObjectItemCaseSensitive(body, "id") && !id) {
        cJSON_Delete(body); return send_result(req, false, "valid profile id required");
    }
    bool connect = false, setup = false;
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (cJSON_IsString(op) && !strcmp(op->valuestring, "save")) {
        const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(body, "ssid");
        const cJSON *pass = cJSON_GetObjectItemCaseSensitive(body, "password");
        const cJSON *host = cJSON_GetObjectItemCaseSensitive(body, "host");
        const cJSON *autojoin = cJSON_GetObjectItemCaseSensitive(body, "auto_join");
        const cJSON *priority = cJSON_GetObjectItemCaseSensitive(body, "priority");
        if ((!ssid || cJSON_IsString(ssid)) && (!pass || cJSON_IsString(pass)) && (!host || cJSON_IsString(host)) &&
            cJSON_IsBool(autojoin) && cJSON_IsNumber(priority) && floor(priority->valuedouble) == priority->valuedouble &&
            priority->valuedouble >= -100 && priority->valuedouble <= 100) {
            err = net_wifi_profile_save(&id, ssid ? ssid->valuestring : NULL, pass ? pass->valuestring : NULL,
                cJSON_IsTrue(autojoin), (int)priority->valuedouble, host ? host->valuestring : NULL);
        }
    } else if (cJSON_IsString(op) && !strcmp(op->valuestring, "connect") && id) {
        /* Validate before ACK; scheduling happens only after successful send. */
        connect = true; err = net_wifi_profile_can_connect(id);
    } else if (cJSON_IsString(op) && !strcmp(op->valuestring, "forget") && id) {
        err = net_wifi_profile_forget(id);
    } else if (cJSON_IsString(op) && !strcmp(op->valuestring, "setup")) { setup = true; err = ESP_OK; }
    cJSON_Delete(body);
    if (err != ESP_OK) { return send_result(req, false, esp_err_to_name(err)); }
    char reply[160]; snprintf(reply, sizeof(reply), "{\"ok\":true,\"id\":%lu,\"scheduled\":%s}",
        (unsigned long)id, connect || setup ? "true" : "false");
    esp_err_t sent = send_json(req, reply);
    if (sent == ESP_OK && connect) { (void)net_wifi_profile_connect(id, 1500); }
    if (sent == ESP_OK && setup) { (void)net_wifi_setup_ap_start(); }
    return sent;
}

/*
 * Explicit end of provisioning: the operator has read the address and is ready
 * for the setup AP to go away.  Kept separate from /api/provision precisely so
 * that joining the network does not also remove the only page that can report
 * whether it worked.
 */
static esp_err_t h_done(httpd_req_t *req)
{
    if (!net_wifi_setup_ap_active()) {
        return send_result(req, true, NULL);
    }
    if (!net_wifi_connected()) {
        return send_result(req, false, "not connected yet");
    }
    net_wifi_setup_ap_stop();
    ESP_LOGW(TAG, "provisioning finished by user; setup AP closed");
    return send_result(req, true, NULL);
}

/* The tone table, so the panel can offer a dropdown instead of asking anyone
 * to remember that 88.5 Hz is code 0009. */
static esp_err_t h_ctcss(httpd_req_t *req)
{
    char buf[2200];
    if (ctcss_list_json(buf, sizeof(buf)) == 0) {
        return send_result(req, false, "no tones");
    }
    return send_json(req, buf);
}

/* ------------------------------------------------------------- channels ---- */

/* The saved presets, newest last. */
static esp_err_t h_channels(httpd_req_t *req)
{
    /* Reserve for all 99 entries, metadata and worst-case JSON escaping. */
    enum { CHANS_JSON_MAX = CHANNELS_JSON_MAX };
    char *buf = heap_caps_malloc(CHANS_JSON_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = malloc(CHANS_JSON_MAX);
    }
    if (!buf) {
        return send_result(req, false, "no memory");
    }
    if (channels_list_json(buf, CHANS_JSON_MAX) < 0) {
        free(buf);
        return send_result(req, false, "channel response too large");
    }
    const esp_err_t r = send_json(req, buf);
    free(buf);
    return r;
}

/*
 * op=save   name=<label>            store the radio's current settings
 * op=load   i=<index>               apply a stored preset
 * op=edit   i=<index>&name=<label>  rename in place
 * op=del    i=<index>
 */
static esp_err_t h_channel(httpd_req_t *req)
{
    /*
     * Parsed by hand.
     *
     * httpd_query_key_value() gives nothing useful here even though the
     * identical call in h_set() works, so the query is split manually.  Values
     * are also URL-decoded, which the SDK helper does not do -- a name with a
     * space or non-ASCII characters needs it.
     *
     * The first attempt walked backwards from each '=' to locate the key, and
     * failed for "name" while working for "op".  Splitting the pairs once and
     * comparing keys directly has no such ordering to get wrong.
     */
    char op[12] = {0}, name[CHANNEL_NAME_LEN * 2] = {0}, val[16] = {0};
    const char *qs = strchr(req->uri, '?');
    if (qs) {
        const char *pair = qs + 1;
        while (*pair) {
            const char *amp = strchr(pair, '&');
            const size_t pair_len = amp ? (size_t)(amp - pair) : strlen(pair);
            const char *eq = memchr(pair, '=', pair_len);
            if (eq) {
                const size_t klen = (size_t)(eq - pair);
                char *dst = NULL;
                size_t cap = 0;
                if (klen == 2 && !strncmp(pair, "op", 2)) {
                    dst = op;  cap = sizeof(op);
                } else if (klen == 4 && !strncmp(pair, "name", 4)) {
                    dst = name; cap = sizeof(name);
                } else if (klen == 1 && pair[0] == 'i') {
                    dst = val; cap = sizeof(val);
                }
                if (dst && cap) {
                    const char *v = eq + 1;
                    const size_t vlen = pair_len - klen - 1;
                    size_t n = 0;
                    for (size_t k = 0; k < vlen && n + 1 < cap; ++k) {
                        char c = v[k];
                        if (c == '+') {
                            c = ' ';
                        } else if (c == '%' && k + 2 < vlen) {
                            char hex[3] = { v[k + 1], v[k + 2], 0 };
                            c = (char)strtol(hex, NULL, 16);
                            k += 2;
                        }
                        dst[n++] = c;
                    }
                    dst[n] = '\0';
                    ESP_LOGI(TAG, "channel arg: %.*s = \"%s\"", (int)klen, pair, dst);
                }
            }
            if (!amp) {
                break;
            }
            pair = amp + 1;
        }
    }
    const int idx = val[0] ? atoi(val) : -1;

    if (!strcmp(op, "save") || !strcmp(op, "edit")) {
        sa818_cfg_t cfg;
        sa818_cfg_load(&cfg);
        if (!strcmp(op, "edit")) {
            /* Renaming must not disturb the stored radio settings. */
            if (!channels_get(idx, NULL, 0, &cfg)) {
                return send_result(req, false, "no such channel");
            }
        } else if (!sa818_cfg_tx_ready() || !sa818_cfg_compatible(&cfg)) {
            return send_result(req, false, "configure radio before saving channel");
        }
        const int slot = channels_save(idx, name, &cfg);
        if (slot < 0) {
            return send_result(req, false, "store full or NVS error");
        }
        bridge_apply();
        return send_result(req, true, NULL);
    }
    if (!strcmp(op, "del")) {
        const bool deleted = channels_delete(idx);
        if (deleted) {
            bridge_channel_deleted(idx);
        }
        return send_result(req, deleted, "no such channel");
    }
    if (!strcmp(op, "load")) {
        sa818_cfg_t cfg;
        if (!channels_get(idx, NULL, 0, &cfg)) {
            return send_result(req, false, "no such channel");
        }
        char slog[192] = {0};
        const esp_err_t err = sa818_cfg_update(&cfg, slog, sizeof(slog));
        bridge_apply();
        return send_result(req, err == ESP_OK, slog);
    }
    return send_result(req, false, "unknown op");
}

/* The digital code list, same shape as /api/ctcss. */
static esp_err_t h_dcs(httpd_req_t *req)
{
    char *buf = malloc(3000);
    if (!buf) {
        return send_result(req, false, "no memory");
    }
    const int n = ctcss_dcs_list_json(buf, 3000);
    if (n == 0) {
        free(buf);
        return send_result(req, false, "no codes");
    }
    const esp_err_t r = send_json(req, buf);
    free(buf);
    return r;
}

/* Any unknown URL becomes the setup page, which is what makes a phone notice
 * the portal.  The body is not optional: iOS will not treat a bare redirect as
 * a captive portal. -- same note as the official captive_portal example. */
static esp_err_t h_404(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    if (net_wifi_setup_ap_active()) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/setup");
        httpd_resp_send(req, "Redirecting to the setup page", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    return ESP_FAIL;
}

/* ------------------------------------------------------------------- routes */

/* Set once the TLS listener is bound; plain HTTP only redirects when it is. */
static bool s_https_ready;

static esp_err_t h_index(httpd_req_t *req)
{
    /* On a board that is still being provisioned, the root URL is the form. */
    if (net_wifi_setup_ap_active() && !net_wifi_connected()) {
        return h_setup(req);
    }
    /* Plain HTTP is a discovery entry only: redirect the root document to the
     * HTTPS panel. API routes, /setup and the debug pages stay on plain HTTP -- the
     * APP, the fleet scripts and the provisioning flow speak HTTP, and the AP
     * setup form must work before any certificate has been accepted. Only
     * redirect once TLS is actually listening, or the panel would vanish. */
    char host[96], location[64];
    if (req->user_ctx == NULL && s_https_ready &&
        net_wifi_connected() && !net_wifi_setup_ap_active() &&
        panel_https_location(net_wifi_ip(), location, sizeof(location))) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        httpd_resp_set_hdr(req, "Location", location);
        return httpd_resp_send(req, NULL, 0);
    }
    /* Use mDNS for discovery, then keep the page, API and SDR on one IP
     * origin. Some clients load .local HTML but reject its first fetches until
     * the board's IP has been visited. A non-cacheable navigation removes that
     * manual prerequisite and follows DHCP changes without an old 301 cache.
     * HTTPS stays HTTPS; API requests are never redirected or replayed.
     */
    if (net_wifi_connected() && !net_wifi_setup_ap_active() &&
        httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) == ESP_OK &&
        panel_address_location(host, net_wifi_hostname(), net_wifi_ip(),
                               req->user_ctx != NULL, location, sizeof(location))) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        httpd_resp_set_hdr(req, "Location", location);
        return httpd_resp_send(req, NULL, 0);
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    /*
     * Chunked transfer instead of one Content-Length response.
     *
     * The panel is ~115 kB; over TLS a single httpd_resp_send() of that size
     * occasionally died half way (the client saw
     * ERR_CONTENT_LENGTH_MISMATCH / ERR_EMPTY_RESPONSE and the page did not
     * load).  With chunked encoding a short write simply ends the response and
     * the browser retries the page, instead of being handed a Length that does
     * not match the body.  4 kB pieces also keep the single httpd task from
     * blocking for the whole transfer.
     */
    const char *body = index_html_start;
    const size_t total = strlen(body);
    const size_t piece = 4096;
    for (size_t off = 0; off < total; off += piece) {
        const size_t n = (total - off < piece) ? (total - off) : piece;
        if (httpd_resp_send_chunk(req, body + off, n) != ESP_OK) {
            /* client vanished: stop, do not send the terminator */
            return ESP_FAIL;
        }
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* Escape a string for JSON embedding: quotes, backslashes, and control bytes. */
static void json_escape_str(const char *in, char *out, size_t out_len)
{
    size_t o = 0;
    if (!out || out_len == 0) {
        return;
    }
    if (!in) {
        out[0] = '\0';
        return;
    }
    for (const unsigned char *p = (const unsigned char *)in; *p; ++p) {
        const unsigned char c = *p;
        if (o + 7 >= out_len) {
            break;
        }
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c < 0x20) {
            o += (size_t)snprintf(out + o, out_len - o, "\\u%04x", c);
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
}

/* Why did the last boot happen?  Without a serial cable this is the only way to
 * tell a scheduled OTA reboot from a panic or a brownout. */
static const char *reset_reason_name(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "poweron";
    case ESP_RST_EXT:       return "ext";
    case ESP_RST_SW:        return "sw";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "int_wdt";
    case ESP_RST_TASK_WDT:  return "task_wdt";
    case ESP_RST_WDT:       return "wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "sdio";
    case ESP_RST_USB:       return "usb";
    case ESP_RST_JTAG:      return "jtag";
    case ESP_RST_EFUSE:     return "efuse";
    case ESP_RST_PWR_GLITCH:return "pwr_glitch";
    case ESP_RST_CPU_LOCKUP:return "cpu_lockup";
    default:                return "unknown";
    }
}

/* Read back only the verified running app; no arbitrary partition or offset. */
static esp_err_t h_firmware_bin(httpd_req_t *req)
{
    const esp_partition_t *p = esp_ota_get_running_partition();
    if (!p) { return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no running app"); }
    const esp_partition_pos_t pos = { .offset = p->address, .size = p->size };
    esp_image_metadata_t image = {0};
    if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &image) != ESP_OK ||
        !image.image_len || image.image_len > p->size) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "app verification failed");
    }
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=heimao-firmware.bin; filename*=UTF-8''%E9%BB%91%E7%8C%AB%E4%BA%92%E8%81%94-%E8%BF%90%E8%A1%8C%E5%9B%BA%E4%BB%B6.bin");
    char chunk[1024];
    for (size_t off = 0; off < image.image_len; off += sizeof(chunk)) {
        const size_t n = (image.image_len - off) < sizeof(chunk)
                           ? image.image_len - off : sizeof(chunk);
        if (esp_partition_read(p, off, chunk, n) != ESP_OK ||
            httpd_resp_send_chunk(req, chunk, n) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* ------------------------------------------------------------ coredump ---- */

/*
 * GET /api/coredump: stream the coredump partition.
 *
 * The board usually runs with no serial cable, so a panic used to leave no
 * evidence at all (coredump-to-flash was disabled and the panic message goes to
 * UART only).  With CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH the crash is written to
 * the 64 KB "coredump" partition; this endpoint hands it back over the network
 * so it can be decoded against the matching .elf:
 *
 *   espcoredump.py info_corefile -c coredump.bin firmware/build/openradiolink.elf
 *
 * Streamed in 1 KB chunks from the partition (not mmap'd, so a concurrent flash
 * write cannot invalidate the mapping) -- a 64 KB heap buffer would not fit in
 * the internal RAM this firmware has left.
 */
static esp_err_t h_coredump(httpd_req_t *req)
{
    const esp_partition_t *p = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    if (!p) {
        return send_result(req, false, "no coredump partition");
    }
    /*
     * The flash coredump is not a bare ELF at offset 0: the partition starts
     * with a small header (data length + version) and the ELF begins after it.
     * Hand out the ELF itself so the downloaded file can go straight into
     * espcoredump.py without an offset dance.
     */
    uint8_t head[64] = {0};
    if (esp_partition_read(p, 0, head, sizeof(head)) != ESP_OK) {
        return send_result(req, false, "partition read failed");
    }
    size_t elf_off = sizeof(head);
    for (size_t i = 0; i + 4 <= sizeof(head); ++i) {
        if (memcmp(head + i, "\x7f" "ELF", 4) == 0) {
            elf_off = i;
            break;
        }
    }
    if (elf_off == sizeof(head)) {
        return send_result(req, false, "no dump (the last boot did not panic)");
    }
    const size_t dump_len = p->size - elf_off;
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       "attachment; filename=coredump.elf");
    char chunk[1024];
    for (size_t off = 0; off < dump_len; off += sizeof(chunk)) {
        const size_t n = (dump_len - off) < sizeof(chunk)
                             ? (dump_len - off) : sizeof(chunk);
        if (esp_partition_read(p, elf_off + off, chunk, n) != ESP_OK) {
            break;
        }
        if (httpd_resp_send_chunk(req, chunk, n) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    (void)httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* Deliberately a separate uri: h_coredump is long and the route table is the
 * index of "what this server exposes". */

/*
 * GET /api/live: the handful of fields the panel refreshes every second -- the
 * meters, the PTT state and the squelch lamps.
 *
 * /api/status is a ~2 KB document that also pulls the radio profile out of NVS
 * and walks every subsystem.  At 1 Hz from a browser that was the panel's
 * largest recurring cost (on a public link: one TLS round trip per second).
 * Meters do not need any of it, so they get their own ~120 byte answer and the
 * full refresh drops to 3 s.
 */
static esp_err_t h_activity(httpd_req_t *req)
{
    char reply[RADIO_ACTIVITY_JSON_SIZE];
    radio_activity_json(reply, sizeof(reply));
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return send_json(req, reply);
}

static esp_err_t h_live(httpd_req_t *req)
{
    voice_stats_t vs;
    voice_get_stats(&vs);
    const sa818_state_t *st = sa818_state();
    char buf[384];
    snprintf(buf, sizeof(buf),
             "{\"peak\":%d,\"rssi\":%d,\"sql\":%s,\"mod_sq\":%s,"
             "\"ptt\":\"%s\",\"mode\":\"%s\",\"ic_mode\":%d,\"net_ready\":%s,"
             "\"rf_tx\":%s,\"rf_key\":%s,\"vsmp\":%u,\"ipk\":%d,\"opk\":%d,\"bgtx\":%d,"
             "\"bias\":%d,\"base\":%d,\"bpk\":%d,\"sqgpio\":%d}",
             voice_get_squelch_peak(),
             st ? st->rssi : -1,
             voice_squelch_open() ? "true" : "false",
             vs.mod_sq ? "true" : "false",
             ptt_state_name(), voice_mode_name(),
             (int)netcom_mode(),
             netcom_tx_ready() ? "true" : "false",
             netcom_rf_tx() ? "true" : "false",
             /* What the radio is actually doing, not what the PTT machine says:
             * a plain network talk is TX logically but keyed no pad at all. */
             (ptt_get_state() == PTT_TX && ptt_rf_keying_enabled()) ? "true" : "false",
             vs.samples,
             voice_take_meter_in(), voice_take_meter_out(), voice_get_bgtx(),
             voice_get_bias(), voice_get_bias_base(), voice_take_block_peak(),
             voice_get_sq_gpio());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

/* Separate endpoint keeps capture diagnostics out of the crowded status JSON. */
static esp_err_t h_speaker_debug(httpd_req_t *req)
{
    extern const char debug_html[] asm("_binary_speaker_debug_html_start");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, debug_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_speaker_diag_command(httpd_req_t *req)
{
    char op[20], duration[16];
    query_str(req, "op", op, sizeof(op));
    query_str(req, "ms", duration, sizeof(duration));
    char *end = NULL;
    const unsigned long ms = strtoul(duration, &end, 10);
    if (!op[0] || !duration[0] || !end || *end || ms < 250 || ms > 5000) {
        return send_result(req, false, "op and ms=250..5000 required");
    }
    const esp_err_t err = voice_speaker_diag_command(op, (unsigned)ms);
    return send_result(req, err == ESP_OK, esp_err_to_name(err));
}

static unsigned meter_rms(const speaker_meter_t *m)
{
    return m->samples ? (unsigned)sqrt((double)m->squares / (double)m->samples) : 0;
}

static esp_err_t h_speaker_diag(httpd_req_t *req)
{
    speaker_diag_stats_t d;
    audio_i2s_diag_t t;
    board_amp_diag_t a;
    speaker_diag_get(&d);
    audio_i2s_diag_get(&t);
    board_amp_diag_get(&a);
    char buf[3200];
    snprintf(buf, sizeof(buf),
        "{\"mode\":\"%s\",\"reason\":\"%s\",\"session\":%u,\"remaining_ms\":%u,"
        "\"tx_inhibited\":%s,\"captured_samples\":%u,\"capture_target\":%u,\"capture_spk\":%d,"
        "\"receive_windows\":%u,\"receive_work_max_us\":%u,"
        "\"common\":{\"samples\":%llu,\"peak\":%u,\"rms\":%u,\"rails\":%llu},"
        "\"local\":{\"samples\":%llu,\"peak\":%u,\"rms\":%u,\"rails\":%llu},"
        "\"test\":{\"runs\":%u,\"samples\":%llu,\"peak\":%u,\"rms\":%u},"
        "\"codec\":{\"restore_pending\":%s,\"original_reg31\":%u,\"readback_reg31\":%u,"
        "\"mute_applied\":%s,\"apply_error\":%d,\"error\":%d,\"restore_attempts\":%u},"
        "\"amp\":{\"wanted\":%s,\"operator_muted\":%s,\"enabled\":%s,\"shdn_level\":%d,"
        "\"enable_requests\":%u,\"mute_requests\":%u,\"on_edges\":%u,\"off_edges\":%u,\"last_change_us\":%lld},"
        "\"i2s\":{\"requested_frames\":%llu,\"written_frames\":%llu,\"dma_frames\":%llu,"
        "\"dma_callbacks\":%llu,\"calls\":%u,\"errors\":%u,\"short_writes\":%u,"
        "\"concurrent_calls\":%u,\"writer_changes\":%u,\"writer\":\"%s\","
        "\"gap_max_us\":%u,\"receive_gap_max_us\":%u,\"write_max_us\":%u,"
        "\"receive_active\":%s,\"receive_queue_stalls\":%u,\"send_queue_overflows\":%u,"
        "\"rx_queue_overflows\":%u,\"last_error\":%d},"
        "\"note\":\"DMA frames include clocked silence; queue stalls are scheduling evidence, not an exact count of audible missing samples\"}",
        speaker_diag_mode_name(d.mode), d.reason, (unsigned)d.session, (unsigned)d.remaining_ms,
        ptt_local_audio_active() ? "true" : "false", (unsigned)d.captured, (unsigned)d.capacity, d.capture_speaker_step,
        (unsigned)d.receive_windows, (unsigned)d.receive_block_max_us,
        (unsigned long long)d.common.samples, (unsigned)d.common.peak, meter_rms(&d.common), (unsigned long long)d.common.rail_samples,
        (unsigned long long)d.local.samples, (unsigned)d.local.peak, meter_rms(&d.local), (unsigned long long)d.local.rail_samples,
        (unsigned)d.test_runs, (unsigned long long)d.test.samples, (unsigned)d.test.peak, meter_rms(&d.test),
        d.codec_restore_pending ? "true" : "false", (unsigned)d.codec_original, (unsigned)d.codec_readback,
        d.codec_mute_applied ? "true" : "false", d.codec_apply_error, d.codec_error, (unsigned)d.codec_restore_attempts,
        a.wanted ? "true" : "false", a.operator_muted ? "true" : "false", a.enabled ? "true" : "false", a.shdn_level,
        (unsigned)a.enable_requests, (unsigned)a.mute_requests, (unsigned)a.on_edges, (unsigned)a.off_edges,
        (long long)a.last_change_us,
        (unsigned long long)t.requested_frames, (unsigned long long)t.written_frames, (unsigned long long)t.dma_frames,
        (unsigned long long)t.dma_callbacks, (unsigned)t.calls, (unsigned)t.errors, (unsigned)t.short_writes,
        (unsigned)t.concurrent_calls, (unsigned)t.writer_changes, t.writer,
        (unsigned)t.gap_max_us, (unsigned)t.receive_gap_max_us, (unsigned)t.write_max_us,
        t.receive_active ? "true" : "false", (unsigned)t.receive_queue_stalls,
        (unsigned)t.send_queue_overflows, (unsigned)t.receive_queue_overflows, t.last_error);
    return send_json(req, buf);
}

/* Explicit snapshot only: never put 27 I2C reads into the status polling path. */
static esp_err_t h_speaker_hw(httpd_req_t *req)
{
    audio_codec_diag_t d;
    ptt_control_lock();
    if (speaker_diag_output_active() || ptt_local_audio_active() ||
        ptt_owner() != PTT_OWNER_NONE || ptt_get_state() != PTT_IDLE) {
        ptt_control_unlock();
        return send_result(req, false, "stop playback and wait for PTT idle before reading codec");
    }
    const esp_err_t err = audio_codec_diag_read(&d);
    ptt_control_unlock();
    char buf[1300];
    int used = snprintf(buf, sizeof(buf),
        "{\"ok\":%s,\"error\":%d,\"clock_format_match\":%s,\"registers\":{",
        err == ESP_OK ? "true" : "false", err, d.clock_format_match ? "true" : "false");
    for (unsigned i = 0; i < d.count; ++i) {
        used += snprintf(buf + used, sizeof(buf) - used, "%s\"%02X\":%u",
                         i ? "," : "", d.address[i], (unsigned)d.value[i]);
    }
    snprintf(buf + used, sizeof(buf) - used,
        "},\"note\":\"Readback matches software configuration only; physical clocks and analogue waveforms are not measured\"}");
    return send_json(req, buf);
}

static void wav_u32(uint8_t *p, uint32_t value)
{
    for (int i = 0; i < 4; ++i) { p[i] = (uint8_t)(value >> (8 * i)); }
}

static esp_err_t h_speaker_pcm(httpd_req_t *req)
{
    char which[12];
    query_str(req, "path", which, sizeof(which));
    if (strcmp(which, "common") && strcmp(which, "local")) {
        return send_result(req, false, "path must be common or local");
    }
    const int16_t *common, *local;
    size_t n;
    if (!speaker_diag_capture_acquire(&common, &local, &n)) {
        return send_result(req, false, "recording is empty or not finished");
    }
    const int16_t *pcm = !strcmp(which, "common") ? common : local;
    uint8_t header[44] = {0};
    memcpy(header, "RIFF", 4); wav_u32(header + 4, 36 + n * 2);
    memcpy(header + 8, "WAVEfmt ", 8); wav_u32(header + 16, 16);
    header[20] = 1; header[22] = 1;
    wav_u32(header + 24, AUDIO_SAMPLE_RATE_HZ); wav_u32(header + 28, AUDIO_SAMPLE_RATE_HZ * 2);
    header[32] = 2; header[34] = 16;
    memcpy(header + 36, "data", 4); wav_u32(header + 40, n * 2);
    httpd_resp_set_type(req, "audio/wav");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=receive-pcm.wav");
    esp_err_t err = httpd_resp_send_chunk(req, (const char *)header, sizeof(header));
    for (size_t offset = 0; err == ESP_OK && offset < n;) {
        size_t take = n - offset;
        if (take > 1024) { take = 1024; }
        err = httpd_resp_send_chunk(req, (const char *)(pcm + offset), take * sizeof(int16_t));
        offset += take;
    }
    if (err == ESP_OK) { err = httpd_resp_send_chunk(req, NULL, 0); }
    speaker_diag_capture_release();
    return err;
}

static esp_err_t h_adc(httpd_req_t *req)
{
    voice_adc_diag_t d;
    voice_adc_diag_get(&d);
    char buf[768];
    const int n = snprintf(buf, sizeof(buf),
        "{\"running\":%s,\"session\":%u,\"clock_exact\":%s,"
        "\"requested_hz\":%u,\"configured_hz\":%u,\"source_hz\":%u,"
        "\"div_num\":%u,\"div_a\":%u,\"div_b\":%u,\"interval\":%u,"
        "\"produced_hz\":%u,\"read_hz\":%u,\"elapsed_ms\":%u,"
        "\"produced_samples\":%llu,\"read_samples\":%llu,"
        "\"ovf_total\":%u,\"ovf_session\":%u,\"pool_clears\":%u,"
        "\"i2s_write_max_us\":%u}",
        d.running ? "true" : "false", (unsigned)d.session,
        d.clock_exact ? "true" : "false",
        (unsigned)d.requested_hz, (unsigned)d.configured_hz, (unsigned)d.source_hz,
        (unsigned)d.div_num, (unsigned)d.div_a, (unsigned)d.div_b, (unsigned)d.interval,
        (unsigned)d.produced_hz, (unsigned)d.read_hz, (unsigned)d.elapsed_ms,
        (unsigned long long)d.produced_samples, (unsigned long long)d.read_samples,
        (unsigned)d.ovf_total, (unsigned)d.ovf_session, (unsigned)d.pool_clears,
        (unsigned)d.i2s_write_max_us);
    if (n < 0 || (size_t)n >= sizeof(buf)) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

/* Separate compact response: keep the existing status buffer/clients stable. */
static esp_err_t h_radio(httpd_req_t *req)
{
    sa818_cfg_t cfg;
    sa818_cfg_load(&cfg);
    char why[128], buf[640];
    const bool compatible = sa818_cfg_validate(&cfg, why, sizeof(why)) == ESP_OK;
    const bool ready = sa818_cfg_tx_ready();
    if (compatible && !ready) {
        snprintf(why, sizeof(why), "profile not acknowledged; submit panel frequencies");
    }
    const sa818_band_t band = sa818_cfg_band();
    board_sa818_power_io_t io;
    board_sa818_power_io(&io);
    snprintf(buf, sizeof(buf),
             "{\"band\":\"%s\",\"model\":\"%s\",\"min\":%.1f,\"max\":%.1f,"
             "\"ready\":%s,\"compatible\":%s,\"reason\":\"%s\","
             "\"power_applied\":%d,\"power_pending\":%s,\"power_control\":\"open_drain\","
             "\"hl_open_drain\":%s,\"hl_pull_up\":%s,\"hl_pull_down\":%s,\"hl_output\":%s}",
             sa818_band_name(band), band == SA818_BAND_UHF ? "SA818S-U" :
             band == SA818_BAND_VHF ? "SA818S-V" : "UNSET",
             (double)sa818_band_min(band), (double)sa818_band_max(band),
             ready ? "true" : "false", compatible ? "true" : "false", why,
             board_sa818_high_power_get() ? 1 : 0,
             board_sa818_power_pending() ? "true" : "false",
             io.open_drain ? "true" : "false", io.pull_up ? "true" : "false",
             io.pull_down ? "true" : "false", io.output ? "true" : "false");
    return send_json(req, buf);
}

static esp_err_t h_status(httpd_req_t *req)
{
    device_name_t name; device_name_get(&name);
    char esc_name[200]; json_escape_str(name.name, esc_name, sizeof(esc_name));
    sa818_cfg_t cfg;
    sa818_cfg_load(&cfg);

    voice_stats_t vs;
    voice_get_stats(&vs);

    int vs_eq[2][6];
    voice_eq_get(0, vs_eq[0]);
    voice_eq_get(1, vs_eq[1]);

    int mic_db = 0;
    const int mic_idx = audio_codec_get_mic_gain(&mic_db);

    /* Read the codec volume back out of the chip rather than echoing what was
     * asked for, so the panel shows the state that actually holds. */
    int cvol = 0;
    audio_codec_get_volume(&cvol);

    const sa818_state_t *st = sa818_state();

    uint32_t ix_tx = 0, ix_rx = 0;
    int ix_peers = 0;
    const char *ix_state = "idle";
    netcom_stats(&ix_tx, &ix_rx, &ix_peers, &ix_state);
    char ix_diag[256] = "";
    netcom_diag(ix_diag, sizeof(ix_diag));
    netcom_ptt_diag_t ptd;
    netcom_ptt_diag_get(&ptd);
    bridge_rx_stats_t br_rx;
    bridge_rx_stats(&br_rx);
    bool ix_pass_set = false;
    {
        const char *p = "";
        bool set = false;
        netcom_credentials(NULL, &p, &set);
        /* only meaningful while a username is set: clearing the username drops
         * the pair, but the password string stays in the config */
        ix_pass_set = set && p[0] != '\0';
    }

    char br_chan_name[CHANNEL_NAME_LEN] = "";
    if (bridge_chan() >= 1 && bridge_chan() <= channels_count()) {
        channels_get(bridge_chan() - 1, br_chan_name,
                     sizeof(br_chan_name), NULL);
    }

    char esc_ssid[100], esc_ap_ssid[100], esc_host[100];
    char esc_broker[80], esc_diag[256], esc_br_name[80], esc_user[80];
    json_escape_str(net_wifi_ssid(), esc_ssid, sizeof(esc_ssid));
    char ip6all[192] = "[]";
    (void)net_wifi_ip6_json(ip6all, sizeof(ip6all));
    json_escape_str(net_wifi_setup_ssid(), esc_ap_ssid, sizeof(esc_ap_ssid));
    json_escape_str(net_wifi_hostname(), esc_host, sizeof(esc_host));
    uint32_t wifi_disc = 0;
    int wifi_disc_reason = -1;
    int64_t wifi_disc_age = -1;
    net_wifi_disc_stats(&wifi_disc, &wifi_disc_reason, &wifi_disc_age);
    json_escape_str(netcom_broker_str(), esc_broker, sizeof(esc_broker));
    {
        const char *u = "";
        netcom_credentials(&u, NULL, NULL);
        json_escape_str(u, esc_user, sizeof(esc_user));
    }
    json_escape_str(ix_diag, esc_diag, sizeof(esc_diag));
    json_escape_str(br_chan_name, esc_br_name, sizeof(esc_br_name));

    const esp_app_desc_t *app = esp_app_get_description();
    char app_sha[65];
    for (int i = 0; i < 32; ++i) { snprintf(app_sha + 2 * i, 3, "%02x", app->app_elf_sha256[i]); }
    /* HTTP and HTTPS run concurrently. Keep the growing JSON response off
     * their fixed task stacks, with a separate PSRAM allocation per request. */
    enum { STATUS_JSON_SIZE = 4608 };
    char *buf = heap_caps_malloc(STATUS_JSON_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "status buffer allocation failed");
    }
    snprintf(buf, STATUS_JSON_SIZE,
        "{"
        "\"device_id\":\"%s\",\"custom_name\":\"%s\",\"name_revision\":%lu,"
        "\"build\":{\"version\":\"%s\",\"date\":\"%s\",\"time\":\"%s\",\"sha256\":\"%s\"},"
        "\"rst\":\"%s\","
        "\"wifi\":{\"connected\":%s,\"ip\":\"%s\",\"ip6\":\"%s\",\"ip6all\":%s,\"ssid\":\"%s\","
        "\"state\":\"%s\",\"rssi\":%d,"
        "\"disc\":%u,\"disc_reason\":%d,\"disc_age_s\":%lld,"
        "\"ap\":%s,\"ap_ip\":\"%s\",\"ap_ssid\":\"%s\",\"host\":\"%s\"},"
        "\"mode\":\"%s\",\"ptt\":\"%s\",\"rssi\":%d,\"peak\":%d,"
        "\"sql_open\":%s,\"mod_sq\":%s,"
        "\"eq_spk\":[%d,%d,%d,%d,%d,%d],\"eq_mic\":[%d,%d,%d,%d,%d,%d],"
        "\"clip\":%u,\"ovf\":%u,\"underruns\":%u,"
        "\"gain\":%d,\"txgain\":%d,\"ntxgain\":%d,\"bgtx\":%d,\"cvol\":%d,\"spk\":%d,\"micgain\":%d,\"mic_db\":%d,\"adcvol\":%d,"
        "\"sqgate\":%d,\"rxnetgain\":%d,\"txpre\":%d,"
        "\"tx_count\":%d,\"uptime\":%d,\"heap\":%u,\"heapInt\":%u,\"heapIntMax\":%u,\"psram\":%u,"
        /* The web-audio path streams raw PCM at this rate; the browser must not
         * assume one, or a rate change turns playback into a pitch shift. */
        "\"audio_rate\":%d,"
        "\"freq\":{\"tx\":%.4f,\"rx\":%.4f,\"bw\":%d,\"sq\":%d,\"vol\":%d,"
        "\"ctcss_tx\":\"%s\",\"ctcss_rx\":\"%s\",\"power\":%d,"
        "\"flt\":[%d,%d,%d]},"
        "\"intercom\":{\"mode\":%d,\"room\":%u,\"state\":\"%s\","
        "\"tx\":%u,\"rx\":%u,\"peers\":%d,\"qdrop\":%u,\"broker\":\"%s\",\"diag\":\"%s\","
        "\"user\":\"%s\",\"pass\":%d},"
        "\"ptt_net\":{\"tx_end_req\":%u,\"tx_end_sent\":%u,\"tx_end_fail\":%u,"
        "\"tx_end_ms\":%lld,\"tx_sent_ms\":%lld,\"tx_delay_ms\":%lld,"
        "\"rx_end\":%u,\"rx_end_ms\":%lld,\"rx_release_ms\":%lld,"
        "\"rx_ptt\":%d,\"rx_carrier\":%d,\"rx_timeout\":%u},"
        "\"bridge\":{\"en\":%d,\"chan\":%d,\"chan_name\":\"%s\","
        "\"active\":%d,\"keyed\":%d,\"rx_open\":%d,\"rx_starts\":%u,\"rx_ends\":%u,"
        "\"rx_start_ms\":%lld,\"rx_end_ms\":%lld},"
        "\"tail\":{\"id\":%d,\"custom_s\":%.2f,\"modes\":%u}",
        device_identity_id(), esc_name, (unsigned long)name.revision,
        app->version, app->date, app->time, app_sha,
        reset_reason_name(),
        net_wifi_connected() ? "true" : "false",
        net_wifi_ip(), net_wifi_ip6(), ip6all, esc_ssid, net_wifi_state_name(),
        net_wifi_rssi_dbm(),
        (unsigned)wifi_disc, wifi_disc_reason, (long long)(wifi_disc_age / 1000),
        net_wifi_setup_ap_active() ? "true" : "false",
        net_wifi_ap_ip(), esc_ap_ssid,
        esc_host,
        voice_mode_name(), ptt_state_name(),
        st ? st->rssi : -1,
        voice_get_squelch_peak(),
        voice_squelch_open() ? "true" : "false",
        vs.mod_sq ? "true" : "false",
        /* 6-band EQ, [bass, warm, body, clear, pres, air] dB, per direction. */
        vs_eq[0][0], vs_eq[0][1], vs_eq[0][2],
        vs_eq[0][3], vs_eq[0][4], vs_eq[0][5],
        vs_eq[1][0], vs_eq[1][1], vs_eq[1][2],
        vs_eq[1][3], vs_eq[1][4], vs_eq[1][5],
        (unsigned)voice_hard_clip_total(), (unsigned)voice_pool_ovf_total(),
        (unsigned)vs.underruns,
        voice_get_gain(), voice_get_tx_gain(), voice_get_net_tx_gain(),
        voice_get_bgtx(), cvol, voice_get_speaker_level(),
        mic_idx, mic_db, audio_codec_get_adc_volume(),
        voice_get_squelch_threshold(),
        voice_get_net_rx_gain(), voice_get_tx_preemph() ? 1 : 0,
        ptt_tx_count(), (int)(esp_timer_get_time() / 1000000),
        (unsigned)esp_get_free_heap_size(),
        /* internal RAM is the scarce resource: task stacks, lwIP pbufs, httpd
         * sessions and the WiFi driver all come from here, and "wifi: mem fail"
         * or a dropped control frame show up long before anything panics. */
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        /* PSRAM headroom: the NET-P2P jitter buffers (~135 KB for four peers
         * plus the self-test slot) live there, so the acceptance soak watches
         * this number for drift. */
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        AUDIO_SAMPLE_RATE_HZ,
        cfg.tx_mhz, cfg.rx_mhz, cfg.bandwidth, cfg.squelch, cfg.volume,
        cfg.tx_ctcss, cfg.rx_ctcss, cfg.high_power,
        cfg.filter_pre_deemph, cfg.filter_highpass, cfg.filter_lowpass,
        /* intercom (netcom): mode / room / engine state / stats / broker. */
        (int)netcom_mode(), (unsigned)netcom_room(), ix_state,
        (unsigned)ix_tx, (unsigned)ix_rx, ix_peers, (unsigned)netcom_tx_qdrop(), esc_broker,
        esc_diag, esc_user, ix_pass_set ? 1 : 0,
        (unsigned)ptd.tx_end_requests, (unsigned)ptd.tx_end_sent, (unsigned)ptd.tx_end_failed,
        (long long)ptd.tx_end_ms, (long long)ptd.tx_sent_ms, (long long)ptd.tx_delay_ms,
        (unsigned)ptd.rx_ends, (long long)ptd.rx_end_ms, (long long)ptd.rx_release_ms,
        ptd.rx_ptt ? 1 : 0, ptd.rx_carrier ? 1 : 0, (unsigned)ptd.rx_timeouts,
        /* analog bridge: selection + live state. */
        bridge_en() ? 1 : 0, bridge_chan(), esc_br_name,
        bridge_active() ? 1 : 0, bridge_keyed() ? 1 : 0,
        br_rx.open ? 1 : 0, (unsigned)br_rx.starts, (unsigned)br_rx.ends,
        (long long)br_rx.start_ms, (long long)br_rx.end_ms,
        tail_get(), (double)tail_custom_samples() / (double)AUDIO_SAMPLE_RATE_HZ,
        (unsigned)tail_modes_get());

    /* v0.4 front panel: the four remappable key actions plus the live LED and
     * key state, so the panel can show what the hardware is doing.  Tiny (four
     * fixed strings and a handful of integers), so it is appended directly and
     * dropped whole if the buffer is full. */
    {
        const int bl = (int)strlen(buf);
        frontpanel_state_t fp;
        frontpanel_get_state(&fp);
        const int w = snprintf(buf + bl, STATUS_JSON_SIZE - (size_t)bl,
            ",\"fp\":{\"led_rx\":%d,\"led_tx\":%d,\"ptt\":%d,\"chan\":%d,\"chan_n\":%d,"
            "\"keys\":[%d,%d,%d,%d],\"act\":[\"%s\",\"%s\",\"%s\",\"%s\"],"
            "\"led_policy\":\"rf_steady_net_5hz\",\"rx_source\":\"%s\",\"tx_source\":\"%s\","
            "\"rf_rx\":%d,\"rf_tx\":%d,\"net_rx\":%d,\"net_tx\":%d,"
            "\"cos_valid\":%d,\"cos_open\":%d,\"net_rx_events\":%u,\"net_tx_events\":%u}",
            fp.led_rx ? 1 : 0, fp.led_tx ? 1 : 0, fp.ptt_pressed ? 1 : 0,
            fp.channel, fp.channel_count,
            fp.level[0], fp.level[1], fp.level[2], fp.level[3],
            frontpanel_action_name(fp.act[0]), frontpanel_action_name(fp.act[1]),
            frontpanel_action_name(fp.act[2]), frontpanel_action_name(fp.act[3]),
            radio_led_mode_name(fp.leds.rx_mode), radio_led_mode_name(fp.leds.tx_mode),
            fp.leds.rf_rx, fp.leds.rf_tx, fp.leds.net_rx, fp.leds.net_tx,
            fp.leds.cos_valid, fp.leds.cos_open,
            (unsigned)fp.leds.net_rx_events, (unsigned)fp.leds.net_tx_events);
        if (w <= 0 || (size_t)(bl + w) >= STATUS_JSON_SIZE) {
            buf[bl] = '\0';
        }
    }

    /* NET-P2P public-internet state: emitted in every mode so the panel can
     * show the public address / NAT hint while the radio sits in any mode. */
    int bl = (int)strlen(buf);

    /* SDR web-audio state: on/off, whether a browser is attached, and the
     * packet counters.  Inserted before the NET-P2P fragment because that one
     * owns the closing brace of the root object. */
    {
        /* Include source/session/end reason and full-width lifetime counters
         * without cutting a JSON key when counters grow. */
        char frag[1280];
        sdr_status_json(frag, sizeof(frag));
        const int fl = (int)strlen(frag);
        if (fl > 0 && bl + 1 + fl < (int)STATUS_JSON_SIZE - 360) {
            buf[bl] = ',';
            memcpy(buf + bl + 1, frag, (size_t)fl);
            buf[bl + 1 + fl] = '\0';
            bl += 1 + fl;
        }
    }
    /* The tail of the log: a client that cannot even complete the TLS
     * handshake never reports anything, and its reason is only in the log. */
    {
        char frag[LOGTAIL_LEN + 32];
        portENTER_CRITICAL(&s_log_mux);
        json_escape_str(s_logtail, frag, sizeof(frag));
        portEXIT_CRITICAL(&s_log_mux);
        const int fl = (int)strlen(frag);
        if (fl > 0 && bl + fl + 16 < (int)STATUS_JSON_SIZE - 480) {
            bl += snprintf(buf + bl, STATUS_JSON_SIZE - bl, ",\"log\":\"%s\"", frag);
        if (bl >= (int)STATUS_JSON_SIZE - 1) { bl = (int)STATUS_JSON_SIZE - 1; }
        }
        portENTER_CRITICAL(&s_log_mux);
        json_escape_str(s_logerr, frag, sizeof(frag));
        portEXIT_CRITICAL(&s_log_mux);
        const int fl2 = (int)strlen(frag);
        if (fl2 > 0 && bl + fl2 + 16 < (int)STATUS_JSON_SIZE - 360) {
            bl += snprintf(buf + bl, STATUS_JSON_SIZE - bl, ",\"logx\":\"%s\"", frag);
        if (bl >= (int)STATUS_JSON_SIZE - 1) { bl = (int)STATUS_JSON_SIZE - 1; }
        }
    }
    /* The panel's own view of the SDR link (see h_sdr_report): the one piece of
     * state the board cannot observe by itself. */
    {
        char frag[420];
        json_escape_str(s_sdr_rep, frag, sizeof(frag));
        const int fl = (int)strlen(frag);
        if (fl > 0 && bl + fl + 16 < (int)STATUS_JSON_SIZE - 480) {
            bl += snprintf(buf + bl, STATUS_JSON_SIZE - bl, ",\"sdr_rep\":\"%s\"", frag);
        if (bl >= (int)STATUS_JSON_SIZE - 1) { bl = (int)STATUS_JSON_SIZE - 1; }
        }
    }
    /* Reserve room for the whole NET-P2P header, not just one byte: the helper
     * always closes its own array and object, but if the header itself were cut
     * in half the document would end inside a string.  When the base JSON has
     * grown this large the section is dropped instead, which the panel renders
     * as "no NET-P2P data" rather than a parse error. */
    if (bl < (int)STATUS_JSON_SIZE - 360) {
        buf[bl] = ',';
        netp2pnet_status_json(buf + bl + 1, STATUS_JSON_SIZE - bl - 2);
        /* The helper terminates its own output with NUL.  Replacing that NUL
         * with the closing brace of the root object is only safe if a new NUL
         * goes right behind it: send_json() uses HTTPD_RESP_USE_STRLEN, so a
         * missing terminator makes strlen() run on into untouched stack, which
         * FreeRTOS still holds as 0xA5 fill, and the client receives valid
         * JSON followed by binary rubbish. */
        const int nl = (int)strlen(buf + bl + 1);
        if (bl + 2 + nl < (int)STATUS_JSON_SIZE) {
            buf[bl + 1 + nl] = '}';
            buf[bl + 2 + nl] = '\0';
        } else {
            buf[bl + 1 + nl] = '\0';
        }
    } else {
        /* Section dropped: still close the root object, otherwise the document
         * the panel receives would be missing its last brace. */
        buf[bl] = '}';
        buf[bl + 1] = '\0';
    }
    const esp_err_t result = send_json(req, buf);
    free(buf);
    return result;
}

/*
 * One setting per request.
 *
 * Kept as a flat key/value pair rather than a REST resource tree: the panel is
 * a control surface, not an API for third parties, and a switch statement is
 * far easier to audit for "which keys can change the radio" than a routing
 * table would be.
 */

/*
 * Preview auto-monitor.  In local mode with no voice path running, nothing
 * owns the DAC, so an armed preview would be consumed by nobody and the
 * button would look dead.  Borrow the monitor task for the tone's duration
 * (settle fades skipped), then stop it again -- unless the operator took
 * over in the meantime (monitor on/off button or a fresh preview arm).
 */
/*
 * The preview borrows the monitor task through the shared borrow API in
 * voice.c (voice_monitor_borrow/release, "preview"): the monitor starts only
 * when nothing else owns the DAC, and stops again when the tone is over --
 * unless the operator took over in the meantime, or the analog bridge is
 * still holding the monitor for its RF -> net tap.
 */
static esp_timer_handle_t s_prev_stop_timer;
static volatile bool s_prev_auto;

static void prev_stop_cb(void *arg)
{
    (void)arg;
    if (!s_prev_auto) {
        return;
    }
    s_prev_auto = false;
    voice_monitor_release("preview");
}

static void prev_auto_begin(int id)
{
    if (netcom_mode() != NETCOM_MODE_LOCAL || voice_mode() != VOICE_OFF) {
        return;
    }
    if (voice_monitor_instant() != ESP_OK) {
        return;
    }
    s_prev_auto = true;
    voice_monitor_borrow("preview");
    if (!s_prev_stop_timer) {
        const esp_timer_create_args_t args = {
            .callback = prev_stop_cb,
            .name     = "prev_stop",
        };
        if (esp_timer_create(&args, &s_prev_stop_timer) != ESP_OK) {
            s_prev_auto = false;
            voice_monitor_release("preview");
            return;
        }
    }
    /* Tone length plus margin so the last sample has left the DAC. */
    uint64_t ms = (uint64_t)(tail_samples(id) / 8) + 1000;
    if (ms < 1500) {
        ms = 1500;
    }
    (void)esp_timer_start_once(s_prev_stop_timer, ms * 1000ULL);
}

static void prev_auto_release(void)
{
    if (!s_prev_auto) {
        return;
    }
    s_prev_auto = false;
    if (s_prev_stop_timer) {
        (void)esp_timer_stop(s_prev_stop_timer);
    }
    voice_monitor_release("preview");
}

/*
 * "Erase everything" needs the HTTP response to be on the wire first: the
 * partition that holds the panel's own settings is about to be wiped.  A
 * one-shot timer gives the socket a moment to flush, then does the same two
 * calls the SW1 10 s gesture does.
 */
static void factory_wipe_cb(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "full factory reset: erasing NVS and rebooting");
    (void)nvs_flash_erase();
    esp_restart();
}

static void factory_wipe_schedule(void)
{
    static esp_timer_handle_t t;
    if (!t) {
        const esp_timer_create_args_t args = {
            .callback = factory_wipe_cb,
            .name     = "factory_wipe",
        };
        if (esp_timer_create(&args, &t) != ESP_OK) {
            ESP_LOGE(TAG, "factory wipe timer create failed");
            return;
        }
    }
    (void)esp_timer_stop(t);
    (void)esp_timer_start_once(t, 700 * 1000);
}

static bool parse_setting_request(httpd_req_t *req, char *key, size_t key_len, char *val, size_t val_len)
{
    key[0] = val[0] = 0;
    char content_type[64] = {0};
    (void)httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type));
    if (req->method == HTTP_POST && !strncasecmp(content_type, "application/json", 16)) {
        cJSON *body = app_read_json(req);
        if (!body) { return false; }
        const cJSON *k = cJSON_GetObjectItemCaseSensitive(body, "k");
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(body, "v");
        const bool valid = cJSON_IsString(k) && cJSON_IsString(v) &&
            strlen(k->valuestring) < key_len && strlen(v->valuestring) < val_len;
        if (valid) { strcpy(key, k->valuestring); strcpy(val, v->valuestring); }
        cJSON_Delete(body);
        if (!valid) { (void)send_result(req, false, "setting k/v must be bounded strings"); }
        return valid;
    }
    if (req->method == HTTP_POST && req->content_len > 0) {
        char body[512];
        const size_t want = (size_t)req->content_len;
        if (want >= sizeof(body)) {
            (void)send_result(req, false, "set body too long"); return false;
        }
        size_t got = 0;
        while (got < want) {
            const int r = httpd_req_recv(req, body + got, want - got);
            if (r <= 0) {
                (void)send_result(req, false, "set body read failed"); return false;
            }
            got += (size_t)r;
        }
        body[got] = '\0';
        char *vp = strstr(body, "&v=");
        if (strncmp(body, "k=", 2) == 0 && vp) {
            *vp = 0;
            url_decode(body + 2); url_decode(vp + 3);
            if (strlen(body + 2) >= key_len || strlen(vp + 3) >= val_len) {
                (void)send_result(req, false, "setting too long"); return false;
            }
            strcpy(key, body + 2); strcpy(val, vp + 3);
        }
    } else {
        query_str(req, "k", key, key_len);
        query_str(req, "v", val, val_len);
        url_decode(val);
    }
    return true;
}

static esp_err_t h_set(httpd_req_t *req)
{
    char key[24], val[96];
    if (!parse_setting_request(req, key, sizeof(key), val, sizeof(val))) { return ESP_FAIL; }

    if (!key[0]) {
        return send_result(req, false, "missing k");
    }
    const int n = atoi(val);
    bool ok = true;

    if (!strcmp(key, "opussweep")) {
        /*
         * Quality sweep (opus_sweep.c): ten configurations over three seconds
         * of synthetic voiced speech each, so it takes ~10 s on the board.
         */
        static char sweep[1024];
        opus_sweep_task(sweep, sizeof(sweep), 60000);
        for (char *q = sweep; *q; ++q) {
            if (*q == '\n') { *q = '|'; }
        }
        static char body[1200];
        snprintf(body, sizeof(body), "{\"ok\":true,\"sweep\":\"%s\"}", sweep);
        return send_json(req, body);
    } else if (!strcmp(key, "opusbench")) {
        /* Run the Opus encode/decode benchmark once and report the cost. */
        static char bench[320];
        static char json[420];
        opus_link_bench_task(bench, sizeof(bench), 60000);
        ESP_LOGI(TAG, "%s", bench);
        /* send_result() drops the message when ok==true, so answer directly. */
        snprintf(json, sizeof(json), "{\"ok\":true,\"bench\":\"%s\"}", bench);
        return send_json(req, json);
    } else if (!strcmp(key, "dacmute")) {
        ptt_control_lock();
        if (ptt_local_audio_active()) {
            ptt_control_unlock();
            return send_result(req, false, "finish local speaker test first");
        }
        /*
         * Bench control, no serial cable needed (opening the USB port resets the
         * board, which ruins a listening test):
         *   0 = normal
         *   1 = mute the codec DAC (data path silent, clocks still running)
         *   2 = stop the I2S port entirely (clocks gone as well)
         * Comparing 1 and 2 separates "noise comes in over the audio data" from
         * "the I2S clocks couple into the module's mic line".
         */
        esp_err_t merr = ESP_OK;
        if (n == 2) {
            merr = audio_i2s_set_enabled(false);
        } else {
            (void)audio_i2s_set_enabled(true);
            merr = audio_codec_mute(n != 0);
        }
        ptt_control_unlock();
        if (merr != ESP_OK) {
            ok = false;
        }
    } else if (!strcmp(key, "gain")) {
        voice_set_gain(n);
    } else if (!strcmp(key, "txgain")) {
        voice_set_tx_gain(n);
    } else if (!strcmp(key, "ntxgain")) {
        /* Network/app PCM only; the handset capsule keeps txgain. */
        voice_set_net_tx_gain(n);
    } else if (!strcmp(key, "cvol")) {
        /* Codec DAC volume: a bench control, not the operator volume.  The
         * panel's "喇叭音量" is spk_vol below. */
        voice_set_output_volume(n);
    } else if (!strcmp(key, "spk_vol")) {
        /* Speaker volume: eight steps, 0 = amplifier off, 5 = 0 dB reference. */
        if (n < VOICE_SPK_MIN || n > VOICE_SPK_MAX) {
            return send_result(req, false, "spk_vol must be 0..7");
        }
        voice_set_speaker_level(n);
    } else if (!strcmp(key, "micgain")) {
        if (n < 0 || n > 7) { return send_result(req, false, "micgain must be 0..7"); }
        voice_set_mic_gain_index(n);       /* persists, so a reboot keeps it */
        ok = true;
    } else if (!strcmp(key, "adcvol")) {
        ok = (audio_codec_set_adc_volume(n) == ESP_OK);
    } else if (!strcmp(key, "sqgpio")) {
        /* SA818 pin 1 (Audio ON) wired to this GPIO: 0 disables, else the pin. */
        voice_set_sq_gpio(atoi(val));
        ok = true;
    } else if (!strcmp(key, "bgtx")) {
        /* 16 = unity, 48 = 3x.  Only affects audio re-transmitted from the net. */
        voice_set_bgtx(atoi(val));      /* setter persists it in NVS */
        ok = true;
    } else if (!strcmp(key, "safilt")) {
        /*
         * The SA818's own voice low-pass (AT+SETFILTER, third field: 0 = normal,
         * 1 = bypass).  In a bridge the audio crosses TWO of them -- once in the
         * receiving module and once in the transmitting one -- which is the most
         * likely reason the result still sounds muffled even after the firmware
         * anti-alias filter is out of the way.  Exposed so the operator can hear
         * the difference instead of arguing about it.
         */
        sa818_cfg_t cfg;
        char lg[160] = "";
        if (sa818_cfg_load(&cfg) == ESP_OK) {
            cfg.filter_lowpass = n ? 1 : 0;
            const esp_err_t e = sa818_cfg_update(&cfg, lg, sizeof(lg));
            bridge_apply();
            if (e != ESP_OK) { return send_result(req, false, lg); }
            ESP_LOGW(TAG, "module voice low-pass %s (%s)",
                     n ? "BYPASSED" : "normal", lg);
        }
        ok = (sa818_cfg_load(&cfg) == ESP_OK && cfg.filter_lowpass == (n ? 1 : 0));
    } else if (!strcmp(key, "rxnetgain")) {
        if (n < 0 || n > 255) { return send_result(req, false, "rxnetgain must be 0..255"); }
        voice_set_net_rx_gain(n);
    } else if (!strcmp(key, "txpre")) {
        voice_set_tx_preemph(n != 0);
    } else if (!strcmp(key, "sqgate")) {
        voice_set_squelch_threshold(n);
    } else if (!strcmp(key, "eq_reset")) {
        voice_eq_reset();
    } else if (!strncmp(key, "eq_", 3)) {
        /*
         * eq_<spk|mic>_<bass|warm|body|clear|pres|air>: change one band of
         * one direction's 6-band EQ, keeping the other five at their current
         * values.  The direction is a 3-char prefix (it is followed by '_'),
         * the band name must match exactly.
         */
        static const char *dir_names[2] = { "spk", "mic" };
        static const char *band_names[6] =
            { "bass", "warm", "body", "clear", "pres", "air" };
        int dir = -1, band = -1;
        for (int d = 0; d < 2 && dir < 0; ++d) {
            if (!strncmp(key + 3, dir_names[d], 3)) {
                dir = d;
            }
        }
        for (int b = 0; b < 6 && band < 0; ++b) {
            if (!strcmp(key + 7, band_names[b])) {
                band = b;
            }
        }
        if (dir < 0 || band < 0 || key[6] != '_') {
            return send_result(req, false, "unknown eq key");
        }
        /*
         * Boost is capped at +6 dB.  The soft limiter is gone (the operator
         * removed it on purpose), so the only thing between a stacked +12 dB
         * boost and a hard clamp16 is nothing -- three bands at +4 dB already
         * clipped.  Cut stays at -12 dB; the cap is a real limit, not a UI hint.
         */
        if (n < -12 || n > 6) {
            return send_result(req, false, "eq band out of range -12..6");
        }
        int cur[6];
        voice_eq_get(dir, cur);
        cur[band] = n;
        voice_eq_set(dir, cur);
    } else if (!strcmp(key, "auto")) {
        voice_set_autostart(n != 0);
    } else if (!strcmp(key, "wifi_radio_off")) {
        if (strcmp(val, "1")) {
            return send_result(req, false, "wifi_radio_off requires v=1");
        }
        if (ptt_is_transmitting() || voice_mode() == VOICE_TALK) {
            return send_result(req, false, "PTT must be idle before stopping Wi-Fi");
        }
        const esp_err_t err = net_wifi_debug_radio_off_schedule();
        if (err != ESP_OK) {
            return send_result(req, false, esp_err_to_name(err));
        }
        return send_json(req,
                         "{\"ok\":true,\"wifi\":\"stopping\","
                         "\"delay_ms\":1500,\"reboot_required\":true}");
    } else if (!strcmp(key, "ix_mode")) {
        /* Intercom mode: 0 = local, 1 = NET-P2P (public-internet STUN hole
         * punch), 2 = relay (EMQX).  The old ESP-NOW mode is gone. */
        esp_err_t err = netcom_set_mode((uint8_t)n);
        if (err != ESP_OK) {
            return send_result(req, false, "mode must be 0, 1 or 2");
        }
    } else if (!strcmp(key, "ix_room")) {
        esp_err_t err = netcom_set_room((uint16_t)n);
        if (err != ESP_OK) {
            return send_result(req, false, "room must be 1000..9999");
        }
    } else if (!strcmp(key, "ix_broker")) {
        /* Relay server, "host" or "host:port".  val is 23 chars: plenty for
         * an IPv4 address plus a port. */
        esp_err_t err = netcom_set_broker(val);
        if (err != ESP_OK) {
            return send_result(req, false, "broker must be host or host:port");
        }
    } else if (!strcmp(key, "ix_user")) {
        /*
         * MQTT username for the relay (EMQX) mode; "" clears the credentials and
         * returns to an anonymous connection.  The two fields are set
         * separately, so carry the other one over -- and copy it out of the
         * config first, because the setter writes into it.
         */
        char cur[NETCOM_PASS_LEN];
        const char *p = "";
        netcom_credentials(NULL, &p, NULL);
        snprintf(cur, sizeof(cur), "%s", p);
        if (netcom_set_credentials(val, cur) != ESP_OK) {
            return send_result(req, false, "user: max 31 printable chars");
        }
    } else if (!strcmp(key, "ix_pass")) {
        char cur[NETCOM_USER_LEN];
        const char *u = "";
        netcom_credentials(&u, NULL, NULL);
        snprintf(cur, sizeof(cur), "%s", u);
        if (netcom_set_credentials(cur, val) != ESP_OK) {
            return send_result(req, false, "pass: max 47 printable chars");
        }
    } else if (!strcmp(key, "ix_rf")) {
        /* Does a network talk also key the local radio?  1 = same mic audio on
         * air as well, 0 = network only (the radio stays in RX). */
        if (netcom_set_rf_tx(n != 0) != ESP_OK) {
            return send_result(req, false, "rf: could not be saved");
        }
    } else if (!strcmp(key, "np2p_peer_add")) {
        /* NET-P2P peer, "name,ip:port" (name up to 7 chars). */
        char name[8] = {0};
        char addr[64];
        const char *comma = strchr(val, ',');
        if (!comma || (size_t)(comma - val) >= sizeof(name)) {
            return send_result(req, false, "peer add: name,ip:port");
        }
        size_t nl = (size_t)(comma - val);
        memcpy(name, val, nl);
        name[nl] = '\0';
        if (snprintf(addr, sizeof(addr), "%s", comma + 1) >= (int)sizeof(addr)) {
            return send_result(req, false, "peer add: name,ip:port");
        }
        /* "[v6]:port" or "ip:port"; a bare IPv6 literal keeps the default */
        uint16_t port = 54321;
        if (addr[0] == '[') {
            char *rb = strchr(addr, ']');
            if (!rb) {
                return send_result(req, false, "peer add: [ipv6]:port");
            }
            *rb = '\0';
            memmove(addr, addr + 1, strlen(addr));
            if (rb[1] == ':') {
                port = (uint16_t)atoi(rb + 2);
            }
        } else if (strchr(addr, ':') != NULL && strchr(addr, ':') == strrchr(addr, ':')) {
            char *colon = strrchr(addr, ':');
            if (colon == addr) {
                return send_result(req, false, "peer add: name,ip:port");
            }
            *colon = '\0';
            port = (uint16_t)atoi(colon + 1);
        }
        esp_err_t err = netp2pnet_peer_add(name, addr, port);
        if (err != ESP_OK) {
            const char *msg = (err == ESP_ERR_INVALID_ARG)
                ? "peer add: bad ip or port"
                : (err == ESP_ERR_NOT_FOUND) ? "peer add: table full"
                : "peer add: failed";
            return send_result(req, false, msg);
        }
    } else if (!strcmp(key, "np2p_peer_del")) {
        esp_err_t err = netp2pnet_peer_del(n);
        if (err != ESP_OK) {
            return send_result(req, false, "peer del: no such peer");
        }
    } else if (!strcmp(key, "np2p_knock")) {
        esp_err_t err = netp2pnet_peer_knock(n);
        if (err != ESP_OK) {
            return send_result(req, false, "knock: no such peer");
        }
    } else if (!strcmp(key, "np2p_hub")) {
        esp_err_t err = netp2pnet_hub_set(n != 0);
        if (err != ESP_OK) {
            return send_result(req, false, "hub: 0 or 1");
        }
    } else if (!strcmp(key, "np2p_stun")) {
        /* STUN server override "host:port", or "" for the built-in list. */
        esp_err_t err = netp2pnet_stun_set(val);
        if (err != ESP_OK) {
            return send_result(req, false, "stun: host:port or empty");
        }
        netp2pnet_stun_now();
    } else if (!strcmp(key, "bridge_en")) {
        /* Analog <-> network bridge: forward the RF receive audio into the
         * room and re-transmit the room audio on the air (P2P / relay only). */
        esp_err_t err = bridge_set_en(n != 0);
        if (err != ESP_OK) {
            return send_result(req, false, "bridge_en: 0 or 1");
        }
    } else if (!strcmp(key, "bridge_chan")) {
        /* 0 = no channel; otherwise the 1-based preset index. */
        esp_err_t err = bridge_set_chan(n);
        if (err != ESP_OK) {
            return send_result(req, false, "bridge_chan: 0 or a saved channel");
        }
    } else if (!strcmp(key, "bridge_test")) {
        /* Self-test: N ms of 1 kHz into the active engine's jitter buffer,
         * so the bridge's net -> RF direction can be checked on one board. */
        netcom_test_tone(n);
    } else if (!strcmp(key, "tail")) {
        esp_err_t err = tail_set(n);
        if (err != ESP_OK) {
            return send_result(req, false, "tail: 0..8（8=自定义，需先上传）");
        }
    } else if (!strcmp(key, "tail_modes")) {
        esp_err_t err = tail_modes_set(n);
        if (err != ESP_OK) {
            return send_result(req, false,
                               "tail_modes: 1..7（位 1/2/4 对应 MODE 00/01/02）");
        }
    } else if (!strcmp(key, "tail_preview")) {
        if (ptt_local_audio_active()) {
            return send_result(req, false, "finish local speaker test first");
        }
        /* Arm a preview through whichever DAC owner is active right now
         * (local monitor task, or the network engines' playback task).  In
         * local idle there is no DAC owner, so borrow the monitor task for
         * the tone's duration instead. */
        if (n && n >= TAIL_COUNT) {
            return send_result(req, false, "tail_preview: 0..8");
        }
        if (n) {
            prev_auto_begin(n);
            tail_preview_arm(n);
        } else {
            prev_auto_release();
            tail_preview_cancel();
        }
    } else if (!strcmp(key, "tail_custom_del")) {
        (void)tail_custom_delete();
    } else if (!strcmp(key, "monitor")) {
        /* The operator took over the monitor: no borrow (preview or bridge)
         * may outlive an explicit on/off. */
        voice_monitor_operator_takeover();
        /* Never start monitor mode while the transmitter is keyed: the two
         * paths share the codec and the I2S clocks. */
        if (n && ptt_is_transmitting()) {
            return send_result(req, false, "PTT is transmitting");
        }
        ok = (n ? (voice_start(VOICE_MONITOR) == ESP_OK)
                : (voice_stop() == ESP_OK));
    } else if (!strcmp(key, "factory_reset")) {
        /*
         * Restore every OPERATING parameter to its factory default: radio
         * profile, audio chain, EQ, intercom mode/room/broker, analog bridge
         * and the end-of-transmission tone.
         *
         * Deliberately kept: the WiFi credentials and the saved channel bank.
         * Erasing the credentials would close the network the panel is being
         * served over -- a "reset" that makes the device unreachable is not a
         * reset, it is a bricking.  The saved channels are user data, not
         * settings, and the panel says so next to the button.
         */
        if (voice_mode() == VOICE_TALK) {
            voice_request_talk_end();
            for (int w = 0; w < 100 && ptt_get_state() != PTT_IDLE; ++w) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
        voice_monitor_operator_takeover();
        netcom_set_mode(NETCOM_MODE_LOCAL);
        (void)bridge_set_en(false);
        (void)bridge_set_chan(0);
        (void)netcom_set_room(NETCOM_ROOM_MIN);
        (void)netcom_set_broker("127.0.0.1:1883");
        ok = sa818_cfg_reset() == ESP_OK;
        bridge_apply();
        ESP_LOGW(TAG, "factory reset: radio waits for operator profile; band retained");
        voice_restore_defaults();
        (void)tail_set(TAIL_OFF);
        (void)tail_modes_set(TAIL_MODE_ALL);
        netp2pnet_factory_reset();
        frontpanel_reset();
        ESP_LOGW(TAG, "panel defaults restored (WiFi credentials and saved "
                      "channels kept)");
    } else if (!strcmp(key, "factory_wipe")) {
        /*
         * The complete reset: erase the whole NVS partition and reboot -- byte
         * for byte what holding SW1 for 10 s does.  WiFi credentials, the saved
         * channel bank, the front-panel key mapping, the NET-P2P peer table and
         * every other saved setting go, and the board comes back up as its own
         * setup access point.
         *
         * The response has to leave before the partition is erased (the browser
         * is still attached to it), so the erase is deferred to a one-shot
         * timer.  By the time it fires the panel has its answer and the reboot
         * is the expected outcome rather than a mystery.
         */
        if (voice_mode() == VOICE_TALK) {
            voice_request_talk_end();
            for (int w = 0; w < 100 && ptt_get_state() != PTT_IDLE; ++w) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
        factory_wipe_schedule();
    } else if (!strcmp(key, "rf_band")) {
        const sa818_band_t band = !strcmp(val, "UHF") ? SA818_BAND_UHF :
                                  !strcmp(val, "VHF") ? SA818_BAND_VHF : SA818_BAND_UNSET;
        const esp_err_t e = sa818_cfg_set_band(band);
        bridge_apply();
        return send_result(req, e == ESP_OK,
                           "select UHF or VHF with PTT released; retry if storage busy");
    } else if (!strcmp(key, "power")) {
        if (strcmp(val, "0") && strcmp(val, "1")) {
            return send_result(req, false, "power must be 0 or 1");
        }
        char slog[128] = {0};
        const esp_err_t e = sa818_cfg_set_power(n != 0, slog, sizeof(slog));
        return send_result(req, e == ESP_OK, slog);
    } else if (!strcmp(key, "freq_tx") || !strcmp(key, "freq_rx")
               || !strcmp(key, "freq_pair")
               || !strcmp(key, "bw") || !strcmp(key, "sq")
               || !strcmp(key, "vol")
               || !strcmp(key, "ctcss_tx") || !strcmp(key, "ctcss_rx")) {
        sa818_cfg_t cfg;
        sa818_cfg_load(&cfg);
        if (!strcmp(key, "freq_tx")) {
            if (!sa818_freq_parse(val, &cfg.tx_mhz)) {
                return send_result(req, false, "invalid TX frequency (12.5k grid)");
            }
        } else if (!strcmp(key, "freq_rx")) {
            if (!sa818_freq_parse(val, &cfg.rx_mhz)) {
                return send_result(req, false, "invalid RX frequency (12.5k grid)");
            }
        } else if (!strcmp(key, "freq_pair")) {
            char *sep = strchr(val, ',');
            if (!sep) { sep = strchr(val, '/'); }
            if (!sep) {
                return send_result(req, false, "freq_pair: tx,rx");
            }
            *sep = '\0';
            if (!sa818_freq_parse(val, &cfg.tx_mhz) ||
                !sa818_freq_parse(sep + 1, &cfg.rx_mhz)) {
                return send_result(req, false, "invalid TX/RX frequencies (12.5k grid)");
            }
        } else if (!strcmp(key, "bw")) {
            if (strcmp(val, "0") && strcmp(val, "1")) {
                return send_result(req, false, "bandwidth must be 0 or 1");
            }
            cfg.bandwidth = n;
        } else if (!strcmp(key, "sq")) {
            char *end;
            long value = strtol(val, &end, 10);
            if (!val[0] || *end || value < 0 || value > 8) {
                return send_result(req, false, "squelch must be 0-8");
            }
            cfg.squelch = (int)value;
        } else if (!strcmp(key, "ctcss_tx") || !strcmp(key, "ctcss_rx")) {
            /*
             * Accept the code the dropdown sends, and also the frequency a
             * human would type, so the endpoint is usable from a URL bar.
             * ctcss_normalise() is the single place that decides what a tone
             * value means.
             */
            char norm[8];
            if (!ctcss_normalise(val, norm)) {
                return send_result(req, false, "unknown tone");
            }
            char *dst = (key[6] == 't') ? cfg.tx_ctcss : cfg.rx_ctcss;
            const size_t cap = (key[6] == 't') ? sizeof(cfg.tx_ctcss)
                                               : sizeof(cfg.rx_ctcss);
            snprintf(dst, cap, "%s", norm);
        } else {
            char *end;
            long value = strtol(val, &end, 10);
            if (!val[0] || *end || value < 1 || value > 8) {
                return send_result(req, false, "volume must be 1-8");
            }
            cfg.volume = (int)value;
        }
        char slog[160];
        const esp_err_t err = sa818_cfg_update(&cfg, slog, sizeof(slog));
        bridge_apply();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "web set %s=%s -> %s", key, val, slog);
        } else {
            ESP_LOGW(TAG, "web set %s=%s failed: %s", key, val,
                     esp_err_to_name(err));
        }
        return send_result(req, err == ESP_OK, slog);
    } else if (!strncmp(key, "fp_key", 6) && key[6] >= '0' && key[6] <= '3' &&
               key[7] == '\0') {
        /*
         * v0.4 front panel: remap one function key.  The PTT key is not in
         * this table on purpose -- it is a level control, not an action.
         */
        const int act = frontpanel_action_from_name(val);
        if (act < 0) {
            return send_result(req, false,
                "unknown action: off ch+ ch- vol+ vol- sql+ sql- power monitor mode bridge");
        }
        const esp_err_t err = frontpanel_key_set_action(key[6] - '0', (uint8_t)act);
        if (err != ESP_OK) {
            return send_result(req, false, esp_err_to_name(err));
        }
        ok = true;
    } else {
        return send_result(req, false, "unknown key");
    }

    ESP_LOGI(TAG, "web: %s = %s", key, !strcmp(key, "ix_pass") ? "<redacted>" : val);
    return send_result(req, ok, ok ? NULL : "rejected by driver");
}

/* -------------------------------------------------------------- transmitter */

static esp_timer_handle_t s_talk_timer;
static uint32_t s_web_talk_token;
static uint32_t sdr_query_id(httpd_req_t *req, const char *key);

/* ------------------------------------------------------------------ h_talk */

static void talk_timeout_cb(void *arg)
{
    /*
     * Request the talk end without releasing the PTT here.  voice_stop() waits
     * for the audio task and touches the I2S -- not something to do from the
     * esp_timer task; and a plain ptt_request_rx() would unkey the radio
     * before the end-of-transmission tail tone, so the tone would never reach
     * the air.  voice_request_talk_end() only sets a flag: the talk task
     * finishes the transmission (tail included, transmitter keyed), releases
     * the PTT itself, and comes back to MONITOR on its own (see talk_task in
     * voice.c).
     */
    ESP_LOGW(TAG, "web talk timer expired -- ending talk (tail included)");
    (void)voice_request_talk_end_for((uint32_t)(uintptr_t)arg);
}

static esp_err_t h_talk(httpd_req_t *req)
{
    const bool on = query_int(req, "on", 0, NULL) != 0;

    if (!on) {
        /*
         * Flag, not unkey: the tail tone must still be transmitted, and the
         * talk task is the one allowed to release the PTT after it (see
         * talk_timeout_cb above).  A stray release with no talk running only
         * sets the flag, which the next voice_start() clears.
         */
        const uint32_t token = sdr_query_id(req, "tx");
        if (!voice_request_talk_end_for(token)) {
            return send_result(req, false, "transmission token no longer owns TALK");
        }
        if (s_talk_timer && s_web_talk_token == token) {
            (void)esp_timer_stop(s_talk_timer);
        }
        ESP_LOGW(TAG, "web: PTT release requested (tail pending)");
        return send_result(req, true, NULL);
    }

    /* Explicit confirmation, so a stray GET (a prefetch, a link scanner) can
     * never key a transmitter. */
    if (query_int(req, "confirm", 0, NULL) != 1) {
        return send_result(req, false, "confirm=1 required");
    }
    /*
     * A talk in MODE 01/02 with the engine not connected (or no peer linked)
     * would swallow the audio without a trace.  Answer with the reason so the
     * panel can show it instead of starting a talk that goes nowhere.  Checked
     * before the monitor is stopped, so a refused press leaves the receiver
     * exactly as it was.
     */
    if (netcom_mode() != NETCOM_MODE_LOCAL && !netcom_tx_ready()) {
        ESP_LOGW(TAG, "web: network talk refused -- %s", netcom_tx_ready_hint());
        return send_result(req, false, netcom_tx_ready_hint());
    }
    if ((netcom_mode() == NETCOM_MODE_LOCAL || netcom_rf_tx()) && !sa818_cfg_tx_ready()) {
        return send_result(req, false, "radio not ready; configure module band and frequencies");
    }
    /* voice_start atomically reserves TX before stopping MONITOR. */
    int secs = query_int(req, "secs", WEB_TALK_DEF_S, NULL);
    if (secs < 5) { secs = 5; }
    if (secs > WEB_TALK_MAX_S) { secs = WEB_TALK_MAX_S; }

    const esp_err_t err = voice_start(VOICE_TALK);
    if (err != ESP_OK) {
        return send_result(req, false, esp_err_to_name(err));
    }

    const uint32_t token = voice_talk_token();
    if (s_talk_timer) {
        (void)esp_timer_stop(s_talk_timer);
        (void)esp_timer_delete(s_talk_timer);
        s_talk_timer = NULL;
    }
    {
        const esp_timer_create_args_t args = {
            .callback = talk_timeout_cb,
            .arg      = (void *)(uintptr_t)token,
            .name     = "web_talk",
        };
        if (esp_timer_create(&args, &s_talk_timer) != ESP_OK) {
            (void)voice_request_talk_end_for(token);
            return send_result(req, false, "timer");
        }
    }
    s_web_talk_token = token;
    (void)esp_timer_stop(s_talk_timer);
    (void)esp_timer_start_once(s_talk_timer, (uint64_t)secs * 1000000ULL);
    ESP_LOGW(TAG, "web: PTT keyed for %d s", secs);
    char reply[80];
    snprintf(reply, sizeof(reply), "{\"ok\":true,\"tx\":%lu}", (unsigned long)token);
    return send_json(req, reply);
}

/* --------------------------------------------------------------- firmware OTA */

/*
 * Firmware upload from the panel (事件日志 -> 上传升级固件).
 *
 * The body is the raw application image (openradiolink.bin), written to the
 * inactive OTA partition.  esp_ota_end() validates the image before it is made
 * bootable, so a dropped upload or a wrong file leaves the running image in
 * place -- the worst case is a rejected file, not a dead board.
 */
static esp_err_t h_ota(httpd_req_t *req)
{
    const size_t total = req->content_len;
    if (total < 0x10000) {
        return send_result(req, false, "image too small");
    }
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        return send_result(req, false, "no OTA partition");
    }
    if (total > part->size) {
        return send_result(req, false, "image larger than the OTA slot");
    }
    if (voice_mode() == VOICE_TALK || ptt_get_state() != PTT_IDLE || ptt_owner() != PTT_OWNER_NONE) {
        char msg[160];
        if (voice_mode() == VOICE_TALK) {
            snprintf(msg, sizeof(msg), "手机通话仍在进行：请先结束通话再升级");
        } else if (ptt_get_state() != PTT_IDLE) {
            snprintf(msg, sizeof(msg), "PTT状态机停在%s：请先松开PTT再升级", ptt_state_name());
        } else {
            snprintf(msg, sizeof(msg), "PTT被%s占用：请先结束通话或收听再升级", ptt_owner_name());
        }
        return send_result(req, false, msg);
    }

    /*
     * Put the radio in a state where a flash write is safe BEFORE writing.
     *
     * The flash cache is disabled for the length of each write, and the network
     * engines plus the web-audio path run on PSRAM stacks / touch PSRAM buffers
     * the whole time.  That is the "OTA fails while the relay or P2P engine is
     * running" report in the handover: a race, not a certainty, which is why it
     * could not be reproduced on demand -- and why it is not worth leaving to
     * chance.  Stopping them here makes it impossible, and removes the "switch
     * to MODE 00 first" step from the operator's checklist.
     *
     * The mode is deliberately NOT restored: the board comes back in LOCAL
     * after the upgrade, which is the safe state (no engine auto-connecting).
     */
    if (netcom_mode() != NETCOM_MODE_LOCAL) {
        ESP_LOGW(TAG, "OTA: stopping the network engine (mode %s -> local)",
                 netcom_mode_name(netcom_mode()));
        if (netcom_set_mode(NETCOM_MODE_LOCAL) != ESP_OK) {
            return send_result(req, false, "network tasks did not stop");
        }
    }
    /* Also retry a previous timed-out stop with s_on already cleared. */
    if (sdr_set_enabled(false) != ESP_OK) {
        return send_result(req, false, "SDR tasks did not stop");
    }

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(part, OTA_SIZE_UNKNOWN, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin: %s", esp_err_to_name(err));
        return send_result(req, false, "ota begin failed");
    }

    static char buf[2048];
    size_t left  = total;
    size_t wrote = 0;
    const int64_t upload_started = esp_timer_get_time();
    int64_t last_progress = upload_started;
    while (left > 0) {
        const int64_t now = esp_timer_get_time();
        if (now-last_progress > 15000000 || now-upload_started > 180000000) {
            esp_ota_abort(handle); return send_result(req, false, "upload timed out");
        }
        const int want = (int)((left < sizeof(buf)) ? left : sizeof(buf));
        const int got  = httpd_req_recv(req, buf, want);
        if (got == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (got <= 0) {
            esp_ota_abort(handle);
            ESP_LOGE(TAG, "OTA upload interrupted after %u bytes",
                     (unsigned)wrote);
            return send_result(req, false, "upload interrupted");
        }
        last_progress = esp_timer_get_time();
        err = esp_ota_write(handle, buf, (size_t)got);
        if (err != ESP_OK) {
            esp_ota_abort(handle);
            ESP_LOGE(TAG, "OTA write: %s", esp_err_to_name(err));
            return send_result(req, false, "flash write failed");
        }
        wrote += (size_t)got;
        left  -= (size_t)got;
    }
    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA image rejected: %s", esp_err_to_name(err));
        return send_result(req, false, "image rejected (wrong file?)");
    }
    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA set boot: %s", esp_err_to_name(err));
        return send_result(req, false, "cannot set boot partition");
    }
    ESP_LOGW(TAG, "OTA: %u bytes -> '%s', rebooting", (unsigned)wrote,
             part->label);
    send_result(req, true, NULL);       /* answer before the socket goes away */
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
    return ESP_OK;
}

/* ------------------------------------------------------------ SDR web audio */

/* Full uint32 IDs, independent of signed query_int / atoi. */
static uint32_t sdr_query_id(httpd_req_t *req, const char *key)
{
    char value[16]; query_str(req, key, value, sizeof(value));
    if (!value[0] || value[0] == '-') { return 0; }
    char *end = NULL;
    const unsigned long long v = strtoull(value, &end, 10);
    return end && !*end && v > 0 && v <= UINT32_MAX ? (uint32_t)v : 0;
}

static void sdr_reply_json(char *reply, size_t n, bool ok, uint32_t id, bool activity)
{
    char state[256]; sdr_state_json(state, sizeof(state), ok, id);
    if (activity) {
        char snapshot[RADIO_ACTIVITY_JSON_SIZE];
        radio_activity_json(snapshot, sizeof(snapshot));
        snprintf(reply, n, "%.*s,\"activity\":%s}", (int)strlen(state)-1, state, snapshot);
    } else { snprintf(reply, n, "%s", state); }
}

static esp_err_t sdr_reply(httpd_req_t *req, bool ok, uint32_t id)
{
    char reply[256 + RADIO_ACTIVITY_JSON_SIZE];
    sdr_reply_json(reply, sizeof(reply), ok, id, query_int(req, "activity", 0, NULL) == 1);
    return send_json(req, reply);
}

static esp_err_t h_sdr(httpd_req_t *req)
{
    const bool on = query_int(req, "on", 0, NULL) != 0;
    const bool force = query_int(req, "force", 0, NULL) != 0;
    bool ok;
    if (on) {
        ok = sdr_client_open(sdr_query_id(req, "client"));
    } else {
        ok = sdr_client_close(sdr_query_id(req, "sid"), force);
    }
    if (!ok) {
        return send_result(req, false, on ? "audio session busy or TALK active"
                                          : "audio session transmitting");
    }
    return sdr_reply(req, ok, 0);
}

static esp_err_t h_sdr_state(httpd_req_t *req)
{
    return sdr_reply(req, sdr_session_valid(sdr_query_id(req, "sid")), 0);
}

/*
 * /api/sdr_report: the panel tells the board what IT sees (secure context,
 * WebSocket state, audio-context state, frames received, last error).  It is
 * mirrored in /api/status as "sdr_rep", so a panel that cannot play audio can
 * still be diagnosed remotely instead of by guesswork.
 */
static esp_err_t h_sdr_report(httpd_req_t *req)
{
    char buf[256];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) != ESP_OK) {
        buf[0] = '\0';
    }
    if (buf[0] == '\0') {
        strlcpy(s_sdr_rep, "(empty)", sizeof(s_sdr_rep));
    } else if (strlen(buf) < sizeof(s_sdr_rep)) {
        strcpy(s_sdr_rep, buf);
    } else {
        memcpy(s_sdr_rep, buf, sizeof(s_sdr_rep) - 1);
        s_sdr_rep[sizeof(s_sdr_rep) - 1] = '\0';
    }
    ESP_LOGI(TAG, "browser report: %s", s_sdr_rep);
    return send_result(req, true, NULL);
}

/*
 * /ws/sdr: the browser's audio socket (see sdr.h).  The server calls the
 * handler once for the upgrade request (method GET) and then once per binary
 * frame.  The browser pulls the radio audio: every frame it sends (a 1-byte
 * tick while listening, microphone chunks while talking) is answered with
 * whatever the monitor produced since the previous frame.  Sending from THIS
 * task is essential -- async sends from another task used to write frames
 * into whatever session reused the old socket descriptor after a client
 * vanished, corrupting the TLS stream of an unrelated HTTPS request.
 */
#define SDR_WS_PKT AUDIO_PKT_SAMPLES   /* one 20 ms web-audio chunk */
/*
 * Packets collected per WebSocket frame (one frame per browser tick).  The
 * cap only binds during catch-up: a steady 50 ms tick yields ~400 samples, so
 * 2 packets.  6 is what a recovering link uses to drain its backlog in one
 * write.  Lower it to 3 to halve the worst-case write (696 B instead of
 * 1392 B): a smaller write blocks the httpd task for less time when the TLS
 * send buffer is full, at the cost of taking two ticks to catch up.
 */
#define SDR_WS_MAX_PKTS 6

/*
 * One buffer pair, shared by both SDR receive handlers.
 *
 * It used to be a `static` pair inside each handler.  That had two problems:
 * two concurrent callers of the SAME handler (two panel tabs, or the iOS HTTP
 * poller running beside a WebSocket client) wrote into the same memory with no
 * locking, and the two pairs together cost ~19 KB of .bss -- the scarcest
 * resource on this board.  One pair under a mutex is correct and 8 KB smaller.
 *
 * Sized for the larger consumer (the HTTP poller: 8 packets = 232 ms).
 */
#define SDR_BUF_PKTS 8
static int16_t s_sdr_pcm[SDR_WS_PKT * SDR_BUF_PKTS * 2];
static SemaphoreHandle_t s_sdr_buf_mux;

/* Take/give around a buffer user.  A second client waits for the first rather
 * than corrupting its data; if the wait itself times out the caller skips this
 * tick instead of running unlocked. */
enum { SDR_LOCK_NONE = 0, SDR_LOCK_HELD = 1, SDR_LOCK_FAIL = -1 };

static int sdr_buf_lock(void)
{
    if (!s_sdr_buf_mux) {
        return SDR_LOCK_FAIL;      /* never use shared storage without its lock */
    }
    /* The holder can block in a WS send for up to send_wait_timeout (5 s), so
     * wait longer than that before giving up on this tick. */
    return xSemaphoreTake(s_sdr_buf_mux, pdMS_TO_TICKS(7000)) == pdTRUE
               ? SDR_LOCK_HELD : SDR_LOCK_FAIL;
}

static void sdr_buf_unlock(int state)
{
    if (state == SDR_LOCK_HELD) {
        xSemaphoreGive(s_sdr_buf_mux);
    }
}

typedef struct {
    httpd_handle_t server;
    int fd;
    uint32_t sid;
    uint32_t control_generation;
    bool activity;
    uint32_t activity_signature;
    int64_t activity_sent_us;
} sdr_ws_context_t;

static void sdr_ws_context_free(void *arg)
{
    sdr_ws_context_t *ctx = arg;
    sdr_ws_detach(ctx->server, ctx->fd, ctx->sid);
    free(ctx);
}

static esp_err_t h_ws_sdr(httpd_req_t *req)
{
    const int fd = httpd_req_to_sockfd(req);
    if (req->method == HTTP_GET) {
        sdr_ws_context_t *ctx = calloc(1, sizeof(*ctx));
        if (!ctx) { return ESP_ERR_NO_MEM; }
        ctx->server = req->handle; ctx->fd = fd; ctx->sid = sdr_query_id(req, "sid");
        ctx->control_generation = app_control_generation();
        ctx->activity = query_int(req, "activity", 0, NULL) == 1;
        if (!sdr_ws_attach(req->handle, fd, ctx->sid)) { free(ctx); return ESP_FAIL; }
        req->sess_ctx = ctx; req->free_ctx = sdr_ws_context_free;
        return ESP_OK;
    }
    /* ESP-IDF clears req->uri for every WS frame. The handshake query is
     * available only once; retain its sid in the socket's session context. */
    sdr_ws_context_t *ctx = req->sess_ctx;
    const uint32_t sid = ctx ? ctx->sid : 0;
    if (!sdr_ws_owner(req->handle, fd, sid)) { return ESP_FAIL; }
    uint8_t buf[2049];
    httpd_ws_frame_t f = {.len = 0};
    /* Query the actual size first. The one-shot receive API reports an error
     * for an oversized frame; it must never leave a held PTT behind. */
    esp_err_t err = httpd_ws_recv_frame(req, &f, 0);
    if (err == ESP_OK && f.len > sizeof(buf)-1) { err = ESP_ERR_INVALID_SIZE; }
    if (err == ESP_OK && f.len != 0) {
        f.payload = buf;
        err = httpd_ws_recv_frame(req, &f, sizeof(buf)-1);
    }
    if (err != ESP_OK || f.type == HTTPD_WS_TYPE_CLOSE) {
        sdr_ws_detach(req->handle, fd, sid);
        return err != ESP_OK ? err : ESP_FAIL;
    }
    if (f.type == HTTPD_WS_TYPE_PING) {
        f.type = HTTPD_WS_TYPE_PONG;
        return httpd_ws_send_frame(req, &f);
    }
    if (f.type == HTTPD_WS_TYPE_TEXT) {
        buf[f.len] = 0;
        unsigned on = 0; uint32_t csid = 0, tx = 0, id = 0; int consumed = 0;
        const bool parsed = sscanf((const char *)buf,
            "ptt%u:%" SCNu32 ":%" SCNu32 ":%" SCNu32 "%n",
            &on, &csid, &tx, &id, &consumed) == 4 && consumed == (int)f.len && on <= 1;
        const bool ok = parsed && csid == sid && sdr_ptt_request(sid, tx, on != 0);
        char reply[256 + RADIO_ACTIVITY_JSON_SIZE];
        sdr_reply_json(reply, sizeof(reply), ok, id, ctx->activity);
        httpd_ws_frame_t ack = {.type=HTTPD_WS_TYPE_TEXT, .final=true,
                               .payload=(uint8_t *)reply, .len=strlen(reply)};
        return httpd_ws_send_frame(req, &ack);
    }
    if (f.type != HTTPD_WS_TYPE_BINARY || !f.len) { return ESP_OK; }
    if (f.len > 1) {
        if (f.len < 10 || (f.len & 1)) {
            sdr_ws_detach(req->handle, fd, sid); return ESP_ERR_INVALID_SIZE;
        }
        const uint32_t csid = (uint32_t)buf[0] | (uint32_t)buf[1]<<8 |
                              (uint32_t)buf[2]<<16 | (uint32_t)buf[3]<<24;
        const uint32_t tx = (uint32_t)buf[4] | (uint32_t)buf[5]<<8 |
                            (uint32_t)buf[6]<<16 | (uint32_t)buf[7]<<24;
        if (csid != sid) { return ESP_FAIL; }
        if (sdr_tx_push_bytes(sid, tx, buf+8, f.len-8)) { sdr_note_ws_frame(f.len-8); }
    }
    /* Opt-in, bounded state frames on the existing heartbeat/PCM exchange.
     * Send only on change or once per second, including silent RX. Never send
     * from another task: that previously corrupted reused HTTPS sockets. */
    if (ctx->activity) {
        char snapshot[RADIO_ACTIVITY_JSON_SIZE];
        const uint32_t signature = radio_activity_json(snapshot, sizeof(snapshot));
        const int64_t now = esp_timer_get_time();
        if (!ctx->activity_sent_us || signature != ctx->activity_signature ||
            now - ctx->activity_sent_us >= 1000000) {
            httpd_ws_frame_t event = {.type=HTTPD_WS_TYPE_TEXT, .final=true,
                .payload=(uint8_t *)snapshot, .len=strlen(snapshot)};
            err = httpd_ws_send_frame(req, &event);
            if (err != ESP_OK) { return err; }
            ctx->activity_signature = signature;
            ctx->activity_sent_us = now;
        }
    }
    /*
     * Collect up to 6 packets (174 ms) and send them as ONE WebSocket frame.
     *
     * They used to go out as six separate frames.  Every frame is a TLS record,
     * and over the internet (measured: 180 ms RTT on the public path) each small
     * record waits for its own ACK, so the handler managed only ~16 frames/s and
     * the stream ran at 3.5 k samples/s instead of 8 k -- audio played for a
     * second, starved, played again.  One coalesced frame per tick removes that
     * round-trip cost entirely.
     */
    int16_t *pcm = s_sdr_pcm;
    uint8_t *out = (uint8_t *)s_sdr_pcm;
    const int lock = sdr_buf_lock();
    if (lock == SDR_LOCK_FAIL) {
        return ESP_OK;              /* another client is mid-send: next tick */
    }
    size_t total = 0;
    for (int i = 0; i < SDR_WS_MAX_PKTS; ++i) {
        const size_t n = sdr_rx_take(pcm + total, SDR_WS_PKT);
        if (n == 0) {
            break;
        }
        total += n;
        sdr_note_rx_sent(n);
    }
    if (total == 0) {
        sdr_buf_unlock(lock);
        return ESP_OK;
    }
    for (size_t k = 0; k < total; ++k) {
        out[2 * k]     = (uint8_t)(pcm[k] & 0xFF);
        out[2 * k + 1] = (uint8_t)((uint16_t)pcm[k] >> 8);
    }
    httpd_ws_frame_t w = {
        .type    = HTTPD_WS_TYPE_BINARY,
        .final   = true,
        .payload = out,
        .len     = total * 2,
    };
    const esp_err_t serr = httpd_ws_send_frame(req, &w);
    sdr_buf_unlock(lock);
    if (serr != ESP_OK) {
        /*
         * The socket refused the frame (typically the TLS/TCP send buffer is
         * full on a slow link).  The audio has already left the ring, so these
         * samples are lost on the board; counting them is what tells "the wire
         * is slow" apart from "the board dropped it".
         */
        sdr_note_ws_send_err();
    }
    return ESP_OK;
}



/*
 * HTTP fallback for the SDR audio.
 *
 * iOS/Safari refuses a WebSocket to a self-signed certificate even after the
 * page itself was allowed ("Continue"): the browser opens a fresh TLS
 * connection for wss://, validates it strictly and aborts it (-0x7280 in the
 * board log).  Plain fetch() requests from the same page ARE covered by the
 * exception, so the very same PCM stream can travel as ordinary requests:
 *
 *   GET  /api/sdr_ptt?on=1|0   key / unkey (the explicit PTT)
 *   POST /api/sdr_tx           one chunk of microphone audio
 *   GET  /api/sdr_rx           one chunk of radio audio (binary)
 *
 * The panel picks this transport automatically when the WebSocket fails.
 */
static esp_err_t h_sdr_test(httpd_req_t *req)
{
    if (!sdr_session_valid(sdr_query_id(req, "sid"))) { return send_result(req, false, "audio session expired"); }
    const int f = query_int(req, "f", 1000, NULL);
    sdr_play_test_tone(f);
    return send_result(req, true, NULL);
}

static esp_err_t h_sdr_ptt(httpd_req_t *req)
{
    const bool on = query_int(req, "on", 0, NULL) != 0;
    const bool ok = sdr_ptt_request(sdr_query_id(req, "sid"), sdr_query_id(req, "tx"), on);
    return sdr_reply(req, ok, sdr_query_id(req, "id"));
}

static esp_err_t h_sdr_tx(httpd_req_t *req)
{
    const uint32_t sid = sdr_query_id(req, "sid"), tx = sdr_query_id(req, "tx");
    if (req->content_len <= 0 || req->content_len > 8192 || (req->content_len & 1)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid PCM length"); return ESP_FAIL;
    }
    if (!sdr_session_valid(sid)) { return ESP_FAIL; }
    uint8_t buf[513]; size_t carry = 0, left = (size_t)req->content_len;
    while (left) {
        const size_t want = left < 512 ? left : 512;
        const int r = httpd_req_recv(req, (char *)buf+carry, want);
        if (r <= 0) { return ESP_FAIL; }
        left -= (size_t)r;
        const size_t total = carry + (size_t)r, even = total & ~(size_t)1;
        /* TCP fragments can end halfway through a sample. Carry that byte
         * across reads and validate this same TX token on EVERY fragment. */
        if (even && !sdr_tx_push_bytes(sid, tx, buf, even)) { return ESP_FAIL; }
        carry = total - even;
        if (carry) { buf[0] = buf[even]; }
    }
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, NULL, 0);
}

#define SDR_HTTP_RX_PKTS 16     /* 320 ms catches up over high-RTT HTTP */

static esp_err_t h_sdr_rx(httpd_req_t *req)
{
    if (!sdr_session_valid(sdr_query_id(req, "sid"))) { return ESP_FAIL; }
    /* A WebSocket listener owns the stream: the audio comes from one ring, so a
     * second consumer would split every sentence.  Answer empty instead of
     * letting a panel that fell back to HTTP silently steal the audio. */
    if (sdr_ws_client_attached()) {
        httpd_resp_set_type(req, "application/octet-stream");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_send(req, NULL, 0);
    }
    /* Shared with the WebSocket handler -- the httpd task stack is 10 kB and a
     * 3.7 KB + 7.4 KB pair would not fit on it anyway.  The poller's 8 packets
     * are what the shared buffer is sized for. */
    _Static_assert(SDR_HTTP_RX_PKTS <= SDR_BUF_PKTS * 2, "SDR buffer too small");
    int16_t *pcm = s_sdr_pcm;
    uint8_t *out = (uint8_t *)s_sdr_pcm;
    const int lock = sdr_buf_lock();
    if (lock == SDR_LOCK_FAIL) {
        /* Someone else holds the buffer; answer empty so the poller retries. */
        httpd_resp_set_type(req, "application/octet-stream");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_send(req, NULL, 0);
    }
    size_t want = SDR_WS_PKT * SDR_HTTP_RX_PKTS;
    const size_t n = sdr_rx_take(pcm, want);
    for (size_t i = 0; i < n; ++i) {
        out[2 * i]     = (uint8_t)(pcm[i] & 0xFF);
        out[2 * i + 1] = (uint8_t)((uint16_t)pcm[i] >> 8);
    }
    if (n > 0) {
        sdr_note_rx_sent(n);
    }
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    /* The response is sent FROM `out`, so the buffer must stay locked across it. */
    const esp_err_t err = httpd_resp_send(req, (const char *)out, n * 2);
    sdr_buf_unlock(lock);
    return err;
}

/* -------------------------------------------------------------------- start */

/* Embedded self-signed certificate (CN=orl.local, SAN orl.local).  It is NOT
 * committed: firmware/main/CMakeLists.txt generates a fresh one at configure
 * time with tools/gen_dev_cert.py, or run that script yourself:
 *   python tools/gen_dev_cert.py --out firmware/main --cn orl.local
 * It is a LAN device certificate, not a public trust anchor: the browser shows
 * its "not private" warning once and the user accepts it.  In exchange the page
 * becomes a SECURE CONTEXT, which is the only way browsers expose the
 * microphone (navigator.mediaDevices) -- and the SDR mode needs it. */
extern const uint8_t servercert_pem_start[] asm("_binary_servercert_pem_start");
extern const uint8_t serverkey_pem_start[]  asm("_binary_serverkey_pem_start");

static const httpd_uri_t s_routes[] = {
    { .uri = "/brand-icon.png", .method = HTTP_GET, .handler = h_brand_icon },
    { .uri = "/favicon.png", .method = HTTP_GET, .handler = h_favicon },
    { .uri = "/brand.js", .method = HTTP_GET, .handler = h_brand_script },
    { .uri = "/app-control.js", .method = HTTP_GET, .handler = h_control_script },
    { .uri = "/api/device", .method = HTTP_GET, .handler = h_device },
    { .uri = "/api/device/name", .method = HTTP_POST, .handler = h_device_name },
    { .uri = "/device-name.js", .method = HTTP_GET, .handler = h_device_name_script },
    { .uri = "/api/control", .method = HTTP_GET, .handler = app_control_http },
    { .uri = "/api/control", .method = HTTP_POST, .handler = app_control_http },
    { .uri = "/api/wifi", .method = HTTP_GET, .handler = h_wifi },
    { .uri = "/api/wifi", .method = HTTP_POST, .handler = h_wifi },
    { .uri = "/api/wifi/setup_session", .method = HTTP_POST, .handler = h_setup_session },
    { .uri = "/debug/speaker", .method = HTTP_GET, .handler = h_speaker_debug },
    { .uri = "/api/speaker_diag", .method = HTTP_GET, .handler = h_speaker_diag },
    { .uri = "/api/speaker_diag", .method = HTTP_POST, .handler = h_speaker_diag_command },
    { .uri = "/api/speaker_hw", .method = HTTP_GET, .handler = h_speaker_hw },
    { .uri = "/api/speaker_pcm", .method = HTTP_GET, .handler = h_speaker_pcm },
    { .uri = "/",              .method = HTTP_GET, .handler = h_index,     },
    { .uri = "/setup",         .method = HTTP_GET, .handler = h_setup,     },
    { .uri = "/api/scan",      .method = HTTP_GET, .handler = h_scan,      },
    { .uri = "/api/ctcss",     .method = HTTP_GET, .handler = h_ctcss,     },
    { .uri = "/api/dcs",       .method = HTTP_GET, .handler = h_dcs,       },
    { .uri = "/api/channels",  .method = HTTP_GET, .handler = h_channels,  },
    { .uri = "/api/channel",   .method = HTTP_GET, .handler = h_channel,   },
    { .uri = "/api/status",    .method = HTTP_GET, .handler = h_status,    },
    { .uri = "/api/radio",     .method = HTTP_GET, .handler = h_radio,     },
    { .uri = "/api/firmware.bin", .method = HTTP_GET, .handler = h_firmware_bin },
    { .uri = "/api/live",      .method = HTTP_GET, .handler = h_live,      },
    { .uri = "/api/activity",  .method = HTTP_GET, .handler = h_activity,  },
    { .uri = "/api/coredump",  .method = HTTP_GET, .handler = h_coredump,  },
    { .uri = "/api/set",       .method = HTTP_GET, .handler = h_set,       },
    { .uri = "/api/set",       .method = HTTP_POST, .handler = h_set,      },
    { .uri = "/api/tail_upload", .method = HTTP_POST, .handler = h_tail_upload },
    { .uri = "/api/talk",      .method = HTTP_GET, .handler = h_talk,      },
    { .uri = "/api/provision", .method = HTTP_GET, .handler = h_provision, },
    { .uri = "/api/provision", .method = HTTP_POST, .handler = h_provision, },
    { .uri = "/api/done",      .method = HTTP_GET, .handler = h_done,      },
    { .uri = "/api/sdr",       .method = HTTP_GET, .handler = h_sdr,       },
    { .uri = "/api/sdr_state", .method = HTTP_GET, .handler = h_sdr_state },
    { .uri = "/api/sdr_report", .method = HTTP_GET, .handler = h_sdr_report },
    { .uri = "/api/sdr_ptt",   .method = HTTP_GET,  .handler = h_sdr_ptt },
    { .uri = "/api/sdr_test",  .method = HTTP_GET,  .handler = h_sdr_test },
    { .uri = "/api/sdr_tx",    .method = HTTP_POST, .handler = h_sdr_tx  },
    { .uri = "/api/sdr_rx",    .method = HTTP_GET,  .handler = h_sdr_rx  },
    { .uri = "/api/adc",       .method = HTTP_GET,  .handler = h_adc,      },
    { .uri = "/api/ota",       .method = HTTP_POST, .handler = h_ota,      },
    { .uri = "/ws/sdr",        .method = HTTP_GET, .handler = h_ws_sdr,
      .is_websocket = true, .handle_ws_control_frames = true },
};

static const char s_https_panel_context;

static bool route_needs_control(const httpd_uri_t *route, httpd_req_t *req)
{
    if (route->handler == h_wifi) { return route->method == HTTP_POST; }
    if (route->handler == h_scan) { return query_int(req, "force", 0, NULL) != 0; }
    return route->handler == h_device_name || route->handler == h_set || route->handler == h_channel || route->handler == h_tail_upload ||
        route->handler == h_talk || route->handler == h_provision || route->handler == h_done ||
        route->handler == h_sdr || route->handler == h_sdr_state || route->handler == h_sdr_report ||
        route->handler == h_sdr_ptt || route->handler == h_sdr_test || route->handler == h_sdr_tx ||
        route->handler == h_sdr_rx || route->handler == h_ws_sdr || route->handler == h_ota ||
        route->handler == h_speaker_diag_command;
}

static esp_err_t controlled_dispatch(httpd_req_t *req)
{
    const httpd_uri_t *route = req->user_ctx;
    const bool websocket = route->handler == h_ws_sdr;
    if (websocket && req->method != HTTP_GET) {
        const sdr_ws_context_t *ctx = req->sess_ctx;
        if (!ctx || !app_control_generation_begin(ctx->control_generation)) { return ESP_FAIL; }
    } else if (route_needs_control(route, req)) {
        if (!app_control_begin(req, websocket)) { return ESP_FAIL; }
    } else { return route->handler(req); }
    esp_err_t result = route->handler(req);
    app_control_end();
    return result;
}

static void revoke_phone_control(void)
{
    const uint32_t sid = sdr_current_session();
    if (sid) { (void)sdr_client_close(sid, false); }
    if (s_web_talk_token) { (void)voice_request_talk_end_for(s_web_talk_token); }
}

static void register_routes(httpd_handle_t server, bool secure)
{
    for (size_t i = 0; i < sizeof(s_routes) / sizeof(s_routes[0]); ++i) {
        httpd_uri_t route = s_routes[i];
        if (route.handler == h_index && secure) {
            route.user_ctx = (void *)&s_https_panel_context;
        }
        if (route.handler != h_index && route.handler != app_control_http && route.handler != h_device) {
            route.handler = controlled_dispatch;
            route.user_ctx = (void *)&s_routes[i];
        }
        const esp_err_t r = httpd_register_uri_handler(server, &route);
        if (r != ESP_OK) {
            ESP_LOGE(TAG, "register %s: %s", s_routes[i].uri, esp_err_to_name(r));
        }
    }
    (void)httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, h_404);
}

esp_err_t net_http_start(void)
{
    /* Initialise SDR's static control locks before either HTTP server runs. */
    sdr_init();
    device_identity_init();
    radio_activity_init();
    ESP_RETURN_ON_ERROR(app_control_init(revoke_phone_control), TAG, "control lease init");
    /*
     * One mutex for the shared SDR buffers (see s_sdr_pcm): the WebSocket
     * handler and the iOS HTTP poller use them from different httpd tasks, and
     * there are two servers (plain + TLS) running the same route table.
     */
    if (!s_sdr_buf_mux) {
        s_sdr_buf_mux = xSemaphoreCreateMutex();
        if (!s_sdr_buf_mux) {
            ESP_LOGW(TAG, "SDR buffer mutex alloc failed -- SDR audio not serialised");
        }
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    /*
     * A browser opens several connections at once.  With the default
     * lru_purge_enable = false, once max_open_sockets is reached the server
     * simply stops accepting and the panel appears to hang; purging the least
     * recently used session is what keeps it responsive.
     */
    config.lru_purge_enable  = true;
    /*
     * Keep-alive is essential, not an optimisation: a TLS handshake on this
     * board costs ~470 ms while a request on an established connection costs
     * ~22 ms.  With the IDF default (keep_alive_enable = false) every request
     * of the panel's polling reopened the connection, which no client can keep
     * up with -- on iOS that showed up as choppy receive audio and microphone
     * audio that never got through.
     */
    config.keep_alive_enable       = true;
    config.keep_alive_idle = 10;
    config.keep_alive_interval     = 2;
    config.keep_alive_count        = 3;
    config.stack_size        = 10240;  /* h_status JSON + the OTA/WS handlers */
    config.max_uri_handlers  = sizeof(s_routes) / sizeof(s_routes[0]) + 2;
    config.max_open_sockets  = 9;      /* a browser tab holds 2-3 with keep-alive */
    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 5;

    httpd_handle_t server = NULL;
    const esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        return err;
    }
    register_routes(server, false);

    /*
     * HTTPS on 443, same panel and API.  Only the microphone (SDR) actually
     * requires it -- browsers hand out navigator.mediaDevices exclusively to
     * secure contexts -- but serving the whole panel there costs nothing extra
     * now that the certificate is embedded, and it keeps wss:// same-origin.
     */
    httpd_ssl_config_t scfg = HTTPD_SSL_CONFIG_DEFAULT();
    scfg.servercert    = servercert_pem_start;
    scfg.servercert_len = strlen((const char *)servercert_pem_start) + 1;
    scfg.prvtkey_pem   = serverkey_pem_start;
    scfg.prvtkey_len   = strlen((const char *)serverkey_pem_start) + 1;
    scfg.httpd.lru_purge_enable  = true;
    scfg.httpd.keep_alive_enable       = true;
    scfg.httpd.keep_alive_idle = 10;
    scfg.httpd.keep_alive_interval     = 2;
    scfg.httpd.keep_alive_count        = 3;
    scfg.httpd.stack_size        = 10240;
    scfg.httpd.max_uri_handlers  = sizeof(s_routes) / sizeof(s_routes[0]) + 2;
    scfg.httpd.max_open_sockets  = 5;   /* Each TLS session costs internal RAM
                                         * (way more than plain HTTP), and the
                                         * panel needs that RAM for the PTT
                                         * task.  The panel holds two pollers
                                         * plus the SDR WebSocket; idle extra
                                         * sockets are purged (lru_purge). */
    scfg.httpd.recv_wait_timeout = 5;
    scfg.httpd.send_wait_timeout = 5;

    httpd_handle_t tls = NULL;
    const esp_err_t terr = httpd_ssl_start(&tls, &scfg);
    if (terr == ESP_OK) {
        register_routes(tls, true);
        s_https_ready = true;
    } else {
        ESP_LOGW(TAG, "https start failed: %s (SDR mic needs https)",
                 esp_err_to_name(terr));
    }

    s_prev_vprintf = esp_log_set_vprintf(logtail_vprintf);

    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "panel ready: http://<ip>/ and https://<ip>/ (build %s)",
             app ? app->version : "?");
    return ESP_OK;
}
