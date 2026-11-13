/*
 * netcom -- mode manager implementation.  See netcom.h.
 *
 * NVS-backed config + engine seams + hot mode switching.  The NET-P2P and
 * relay (EMQX/MQTT) engines register themselves through
 * netcom_register_engine() before netcom_init() runs.
 */
#include "netcom.h"
#include "netrelay.h"
#include "audio_i2s.h"   /* AUDIO_SAMPLE_RATE_HZ / AUDIO_PKT_SAMPLES */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bridge.h"
#include "ptt.h"
#include "voice.h"
#include "radio_led.h"
#include "sdr.h"

static const char *TAG = "netcom";
#define NVS_NS      "orl"
#define KEY_MODE    "ncmode"
#define KEY_MODE_V  "ncmodev"   /* mode-numbering stamp, see netcom_cfg_load */
#define KEY_ROOM    "ncroom"
#define KEY_BROKER  "ncbrkr"
#define KEY_USER    "ncuser"
#define KEY_PASS    "ncpass"
#define KEY_PSK     "ncpsk"
#define KEY_RF      "ncrf"      /* network talk also keys the local radio */

static netcom_cfg_t s_cfg;
static const netcom_engine_t *s_engine[NETCOM_MODE_MAX + 1];  /* NULL = no engine */
static bool s_engine_up;                     /* last engine start succeeded */
static portMUX_TYPE s_ptt_diag_mux = portMUX_INITIALIZER_UNLOCKED;
static netcom_ptt_diag_t s_ptt_diag;

void netcom_ptt_diag_get(netcom_ptt_diag_t *out)
{
    portENTER_CRITICAL(&s_ptt_diag_mux);
    *out = s_ptt_diag;
    portEXIT_CRITICAL(&s_ptt_diag_mux);
}

void netcom_note_end_sent(int64_t requested_ms, bool ok)
{
    const int64_t now = esp_timer_get_time() / 1000;
    portENTER_CRITICAL(&s_ptt_diag_mux);
    if (ok) {
        ++s_ptt_diag.tx_end_sent;
        s_ptt_diag.tx_sent_ms = now;
        s_ptt_diag.tx_delay_ms = now - requested_ms;
    } else {
        ++s_ptt_diag.tx_end_failed;
    }
    portEXIT_CRITICAL(&s_ptt_diag_mux);
}

void netcom_note_rx_ptt(bool ptt, bool timeout)
{
    const int64_t now = esp_timer_get_time() / 1000;
    portENTER_CRITICAL(&s_ptt_diag_mux);
    s_ptt_diag.rx_ptt = ptt;
    if (timeout) {
        ++s_ptt_diag.rx_timeouts;
    } else if (!ptt) {
        ++s_ptt_diag.rx_ends;
        s_ptt_diag.rx_end_ms = now;
    }
    portEXIT_CRITICAL(&s_ptt_diag_mux);
}

void netcom_note_rx_carrier(bool carrier)
{
    const int64_t now = esp_timer_get_time() / 1000;
    portENTER_CRITICAL(&s_ptt_diag_mux);
    if (s_ptt_diag.rx_carrier && !carrier) {
        s_ptt_diag.rx_release_ms = now;
    }
    s_ptt_diag.rx_carrier = carrier;
    portEXIT_CRITICAL(&s_ptt_diag_mux);
}

const char *netcom_mode_name(uint8_t mode)
{
    switch (mode) {
    case NETCOM_MODE_NETP2P: return "netp2p";
    case NETCOM_MODE_RELAY:  return "relay";
    case NETCOM_MODE_LOCAL:
    default:                 return "local";
    }
}

void netcom_cfg_defaults(netcom_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->mode = NETCOM_MODE_LOCAL;
    cfg->room = NETCOM_ROOM_MIN;
    cfg->broker_port = 1883;
    snprintf(cfg->broker, sizeof(cfg->broker), "127.0.0.1");
}

/* -------------------------------------------------------------------- NVS */

esp_err_t netcom_cfg_save(const netcom_cfg_t *cfg)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return ESP_FAIL;
    }
    esp_err_t err = nvs_set_u8(h, KEY_MODE, cfg->mode);
    if (err == ESP_OK) {
        /* Stamp the mode numbering (see netcom_cfg_load): from here on a
         * stored 1 means NET-P2P, not the retired ESP-NOW mode. */
        err = nvs_set_u8(h, KEY_MODE_V, NETCOM_MODE_NUMBERING);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(h, KEY_ROOM, cfg->room);
    }
    if (err == ESP_OK && cfg->broker[0]) {
        err = nvs_set_str(h, KEY_BROKER, cfg->broker);
    }
    if (err == ESP_OK) {
        if (cfg->user[0]) {
            err = nvs_set_str(h, KEY_USER, cfg->user);
            if (err == ESP_OK) {
                /* the password may legitimately be empty (EMQX allows
                 * username-only auth); store it as the empty string so the
                 * pair is unambiguous */
                err = nvs_set_str(h, KEY_PASS, cfg->pass);
            }
        } else {
            /* credentials cleared: drop the stale pair, otherwise a later
             * anonymous session would silently reuse them on reload */
            (void)nvs_erase_key(h, KEY_USER);
            (void)nvs_erase_key(h, KEY_PASS);
        }
    }
    if (err == ESP_OK && cfg->psk_set) {
        err = nvs_set_blob(h, KEY_PSK, cfg->psk, sizeof(cfg->psk));
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, KEY_RF, cfg->rf_tx ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "save: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t netcom_cfg_load(netcom_cfg_t *cfg)
{
    netcom_cfg_defaults(cfg);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return ESP_OK;   /* fresh partition: defaults */
    }
    uint8_t mode = 0;
    uint8_t numbering = 0;
    bool mode_migrated = false;
    const bool new_numbering =
        (nvs_get_u8(h, KEY_MODE_V, &numbering) == ESP_OK &&
         numbering >= NETCOM_MODE_NUMBERING);
    if (nvs_get_u8(h, KEY_MODE, &mode) == ESP_OK) {
        /*
         * Mode numbers changed on 2026-10-16 (the ESP-NOW mode was removed and
         * the public mode moved from 3 to 1), so an OLD stored value has to be
         * read as "what the operator chose then":
         *
         *   3  was public NET-P2P  -> 1  same thing, new number
         *   1  was ESP-NOW P2P     -> 0  that mode is gone; do NOT let a stored
         *                               1 silently put the radio on the public
         *                               network, which is what a literal read
         *                               would do
         *   0 / 2                  -> unchanged
         *
         * The stamp key is what makes this safe both ways: a value written by
         * THIS firmware (stamped) is read literally, so selecting NET-P2P (1)
         * and rebooting does not get "migrated" back to local.  Anything saved
         * from here on carries the stamp.
         *
         * The migrated value is written back at the end of the load, once the
         * rest of the config is in place (saving here would persist defaults
         * over a room and broker that have not been read yet).
         */
        if (!new_numbering && mode == 3) {
            cfg->mode = NETCOM_MODE_NETP2P;
            mode_migrated = true;
        } else if (!new_numbering && mode == 1) {   /* historical NETCOM_MODE_P2P */
            cfg->mode = NETCOM_MODE_LOCAL;
            mode_migrated = true;
        } else if (mode <= NETCOM_MODE_MAX) {
            cfg->mode = mode;
        } else {
            cfg->mode = NETCOM_MODE_LOCAL;
            mode_migrated = true;
        }
    }
    uint16_t room = 0;
    if (nvs_get_u16(h, KEY_ROOM, &room) == ESP_OK &&
        room >= NETCOM_ROOM_MIN && room <= NETCOM_ROOM_MAX) {
        cfg->room = room;
    }
    size_t len = sizeof(cfg->broker);
    if (nvs_get_str(h, KEY_BROKER, cfg->broker, &len) == ESP_OK) {
        /* re-parse the port out of "host:port" */
        char *colon = strrchr(cfg->broker, ':');
        if (colon && colon > cfg->broker) {
            int p = atoi(colon + 1);
            if (p >= 1 && p <= 65535) {
                cfg->broker_port = (uint16_t)p;
            }
        }
    }
    len = sizeof(cfg->psk);
    if (nvs_get_blob(h, KEY_PSK, cfg->psk, &len) == ESP_OK &&
        len == sizeof(cfg->psk)) {
        cfg->psk_set = true;
    }
    size_t ulen = sizeof(cfg->user);
    if (nvs_get_str(h, KEY_USER, cfg->user, &ulen) == ESP_OK && cfg->user[0]) {
        size_t plen = sizeof(cfg->pass);
        if (nvs_get_str(h, KEY_PASS, cfg->pass, &plen) != ESP_OK) {
            cfg->pass[0] = '\0';   /* username-only auth */
        }
        cfg->creds_set = true;
    }
    uint8_t rf = 0;
    if (nvs_get_u8(h, KEY_RF, &rf) == ESP_OK) {
        cfg->rf_tx = rf != 0;
    }
    nvs_close(h);

    if (mode_migrated) {
        ESP_LOGW(TAG, "stored mode %u uses the old numbering -> %s (saved)",
                 mode, netcom_mode_name(cfg->mode));
        (void)netcom_cfg_save(cfg);
    }
    return ESP_OK;
}

/* ------------------------------------------------- broker host:port parse */

static const netcom_engine_t *active_engine(void);
static esp_err_t engine_stop(const netcom_engine_t *e);
static esp_err_t engine_start(const netcom_engine_t *e);

esp_err_t netcom_set_broker(const char *host_port)
{
    if (!host_port || !host_port[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    char buf[NETCOM_BROKER_LEN];
    size_t n = strlen(host_port);
    if (n >= sizeof(buf)) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(buf, host_port, n + 1);

    uint16_t port = s_cfg.broker_port ? s_cfg.broker_port : 1883;
    /*
     * Accept IPv4 "host:port", IPv6 "[addr]:port" and a bare IPv6 literal.
     * Splitting on the LAST colon (the old code) shreds every IPv6 address, so
     * an EMQX broker on an IPv6 network could never be entered.
     */
    if (buf[0] == '[') {
        char *rb = strchr(buf, ']');
        if (!rb) {
            return ESP_ERR_INVALID_ARG;
        }
        *rb = '\0';
        memmove(buf, buf + 1, strlen(buf));      /* drop the '[' */
        if (rb[1] == ':') {
            int p = atoi(rb + 2);
            if (p < 1 || p > 65535) {
                return ESP_ERR_INVALID_ARG;
            }
            port = (uint16_t)p;
        }
    } else if (strchr(buf, ':') != NULL && strchr(buf, ':') == strrchr(buf, ':')) {
        /* exactly one colon -> host:port (a bare IPv6 has several) */
        char *colon = strrchr(buf, ':');
        if (colon > buf) {
            *colon = '\0';
            int p = atoi(colon + 1);
            if (p < 1 || p > 65535) {
                return ESP_ERR_INVALID_ARG;
            }
            port = (uint16_t)p;
        }
    }
    if (!buf[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Assemble "host:port" in a wide local buffer first so the 31-char
     * config field can never be truncated mid-write. */
    char hostport[40];
    snprintf(hostport, sizeof(hostport), "%s:%u", buf, port);
    if (strlen(hostport) + 1 > sizeof(s_cfg.broker)) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(s_cfg.broker, hostport, strlen(hostport) + 1);
    s_cfg.broker_port = port;
    esp_err_t err = netcom_cfg_save(&s_cfg);
    ESP_LOGI(TAG, "broker -> %s", s_cfg.broker);
    if (s_cfg.mode == NETCOM_MODE_RELAY && s_engine_up) {
        const netcom_engine_t *e = active_engine();
        err = engine_stop(e);
        if (err != ESP_OK) { return err; }
        err = engine_start(e);
        bridge_mode_changed();
    }
    return err;
}

/* -------------------------------------------------- MQTT credentials (relay) */

/*
 * Usernames embed in a query string and in the MQTT CONNECT packet; a control
 * character would corrupt both, so reject them rather than store something
 * that cannot be used.
 */
static bool cred_ok(const char *s)
{
    for (const char *c = s; *c; ++c) {
        if ((unsigned char)*c < 0x20 || *c == 0x7f) {
            return false;
        }
    }
    return true;
}

esp_err_t netcom_set_credentials(const char *user, const char *pass)
{
    const char *u = user ? user : "";
    const char *p = pass ? pass : "";
    if (strlen(u) >= sizeof(s_cfg.user) || strlen(p) >= sizeof(s_cfg.pass) ||
        !cred_ok(u) || !cred_ok(p)) {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(s_cfg.user, sizeof(s_cfg.user), "%s", u);
    snprintf(s_cfg.pass, sizeof(s_cfg.pass), "%s", p);
    s_cfg.creds_set = (s_cfg.user[0] != '\0');

    esp_err_t err = netcom_cfg_save(&s_cfg);
    ESP_LOGI(TAG, "mqtt creds -> %s (user '%s', password %s)",
             s_cfg.creds_set ? "set" : "anonymous", s_cfg.user,
             s_cfg.pass[0] ? "set" : "empty");

    /* Apply immediately: the operator is looking at the panel diagnostics when
     * they type a password, so a wrong one has to read "not authorized" now,
     * not after the next mode switch. */
    if (s_cfg.mode == NETCOM_MODE_RELAY && s_engine_up) {
        const netcom_engine_t *e = active_engine();
        err = engine_stop(e);
        if (err != ESP_OK) { return err; }
        err = engine_start(e);
        bridge_mode_changed();
    }
    return err;
}

void netcom_credentials(const char **user, const char **pass, bool *set)
{
    if (user) { *user = s_cfg.user; }
    if (pass) { *pass = s_cfg.pass; }
    if (set)  { *set  = s_cfg.creds_set; }
}

/* ------------------------------------------------------------ engine stub */

static esp_err_t stub_start(const netcom_cfg_t *cfg)
{
    ESP_LOGI(TAG, "engine started (stub) mode=%s room=%u -- audio path lands in stage 2/3",
             netcom_mode_name(cfg->mode), cfg->room);
    return ESP_OK;
}
static esp_err_t stub_stop(void)
{
    ESP_LOGI(TAG, "engine stopped");
    return ESP_OK;
}
static void stub_stats(uint32_t *tx, uint32_t *rx, int *peers, const char **state)
{
    *tx = 0; *rx = 0; *peers = 0; *state = "stub";
}

static const netcom_engine_t s_stub = {
    .name = "stub",
    .start = stub_start,
    .stop = stub_stop,
    .tx_sample = NULL,
    .rx_sample = NULL,
    .stats = stub_stats,
};

esp_err_t netcom_register_engine(uint8_t mode, const netcom_engine_t *engine)
{
    if (mode > NETCOM_MODE_MAX || mode == NETCOM_MODE_LOCAL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_engine[mode] = engine;
    ESP_LOGI(TAG, "engine for mode %s registered: %s",
             netcom_mode_name(mode), engine ? engine->name : "(removed)");
    return ESP_OK;
}

/* ------------------------------------------------------------- switching */

static const netcom_engine_t *active_engine(void)
{
    if (s_cfg.mode == NETCOM_MODE_LOCAL) {
        return NULL;
    }
    return s_engine[s_cfg.mode];
}

static esp_err_t engine_stop(const netcom_engine_t *e)
{
    /* Close the old RF stream while its engine still owns a valid TX queue.
     * bridge_apply blocks the tap before releasing its monitor borrow. */
    s_engine_up = false;
    sdr_rx_network_mode(NETCOM_MODE_LOCAL);
    radio_led_reset_net();
    bridge_mode_changed();
    if (e && e->stop) {
        const esp_err_t err = e->stop();
        if (err != ESP_OK) {
            return err;
        }
    }
    portENTER_CRITICAL(&s_ptt_diag_mux);
    s_ptt_diag.rx_ptt = s_ptt_diag.rx_carrier = false;
    portEXIT_CRITICAL(&s_ptt_diag_mux);
    radio_led_reset_net();
    return ESP_OK;
}

static esp_err_t engine_start(const netcom_engine_t *e)
{
    if (!e) {
        s_engine_up = false;
        return ESP_OK;
    }
    /* A registered engine without a start is a contract bug; treat stubs leniently. */
    if (!e->start) {
        s_engine_up = false;
        return ESP_OK;
    }
    sdr_rx_network_mode(s_cfg.mode);
    const esp_err_t err = e->start(&s_cfg);
    s_engine_up = (err == ESP_OK);
    if (err != ESP_OK) { sdr_rx_network_mode(NETCOM_MODE_LOCAL); }
    return err;
}

esp_err_t netcom_set_mode(uint8_t mode)
{
    if (mode > NETCOM_MODE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (mode == s_cfg.mode && (mode == NETCOM_MODE_LOCAL || s_engine_up)) {
        return ESP_OK;
    }
    /*
     * The network engines need the WiFi radio, which is already up by the
     * time the panel or console can reach this (see app_main: netcom_init
     * runs after net_wifi_start).  Nothing here may block for long.
     */
    if (voice_mode() == VOICE_TALK) {
        const esp_err_t err = voice_stop();
        if (err != ESP_OK) { return err; }
    }
    ptt_set_rf_keying(true);
    const esp_err_t stopped = engine_stop(active_engine());
    if (stopped != ESP_OK) { return stopped; }
    s_cfg.mode = mode;
    esp_err_t err = netcom_cfg_save(&s_cfg);
    if (mode == NETCOM_MODE_LOCAL) {
        ESP_LOGI(TAG, "mode -> local (SA818 radio, base behaviour)");
        bridge_mode_changed();
        return err;
    }
    const netcom_engine_t *e = s_engine[mode];
    if (!e) {
        ESP_LOGW(TAG, "mode -> %s: engine not registered yet (stage 2/3), config stored",
                 netcom_mode_name(mode));
        bridge_mode_changed();
        return err;
    }
    err = engine_start(e);
    ESP_LOGI(TAG, "mode -> %s (room %u, %s)", netcom_mode_name(mode), s_cfg.room,
             err == ESP_OK ? "engine up" : esp_err_to_name(err));
    bridge_mode_changed();
    return err;
}

esp_err_t netcom_set_room(uint16_t room)
{
    if (room < NETCOM_ROOM_MIN || room > NETCOM_ROOM_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (room == s_cfg.room) {
        return ESP_OK;
    }
    s_cfg.room = room;
    esp_err_t err = netcom_cfg_save(&s_cfg);
    const netcom_engine_t *e = s_engine[s_cfg.mode];
    if (e && e->on_room_change) {
        e->on_room_change(room);
    }
    ESP_LOGI(TAG, "room -> %u", room);
    return err;
}

/* --------------------------------------------------------------- accessors */

uint8_t netcom_mode(void)
{
    return s_cfg.mode;
}

uint16_t netcom_room(void)
{
    return s_cfg.room;
}

const char *netcom_broker_str(void)
{
    return s_cfg.broker;
}

uint16_t netcom_broker_port(void)
{
    return s_cfg.broker_port;
}

void netcom_psk(uint8_t *out, bool *set)
{
    *set = s_cfg.psk_set;
    if (s_cfg.psk_set && out) {
        memcpy(out, s_cfg.psk, NETCOM_PSK_LEN);
    }
}

/*
 * Route shaped TX audio to the active engine (8 kHz, 16-bit mono).  The TALK
 * task calls this in P2P mode instead of writing the SA818's MIC_IN.  In
 * local mode, or while an engine has no TX hook, this is a no-op.
 */
void netcom_tx_sample(const int16_t *pcm, size_t n, bool ptt)
{
    const netcom_engine_t *e = active_engine();
    if (e && e->tx_sample) {
        e->tx_sample(pcm, n, ptt);
    }
}

void netcom_tx_end(void)
{
    const netcom_engine_t *e = active_engine();
    if (e && e->tx_end) {
        const int64_t now = esp_timer_get_time() / 1000;
        portENTER_CRITICAL(&s_ptt_diag_mux);
        ++s_ptt_diag.tx_end_requests;
        s_ptt_diag.tx_end_ms = now;
        portEXIT_CRITICAL(&s_ptt_diag_mux);
        e->tx_end();
    }
}

bool netcom_engine_running(void)
{
    return s_engine_up && active_engine() != NULL;
}

/*
 * Can audio be sent right now?
 *
 * "Engine started" is not the same as "audio can go out": the relay starts
 * before the broker has accepted us, and a P2P tunnel is useless until a peer
 * is linked.  Without this check a talk in MODE 01/02 fed the engine frames
 * that went nowhere -- no error, no carrier, nothing at all -- which reads
 * exactly like "network intercom is not implemented".
 */
bool netcom_tx_ready(void)
{
    if (s_cfg.mode == NETCOM_MODE_LOCAL) {
        return true;
    }
    const netcom_engine_t *e = active_engine();
    if (!s_engine_up || !e || !e->stats) {
        return false;
    }
    uint32_t tx = 0, rx = 0;
    int peers = 0;
    const char *state = NULL;
    e->stats(&tx, &rx, &peers, &state);
    if (state && (strcmp(state, "connecting") == 0 ||
                  strcmp(state, "down") == 0 ||
                  strcmp(state, "off") == 0 ||
                  strcmp(state, "stopped") == 0 ||
                  strcmp(state, "no-net") == 0)) {
        return false;
    }
    if (s_cfg.mode == NETCOM_MODE_NETP2P && peers <= 0) {
        return false;
    }
    return true;
}

bool netcom_rf_tx(void)
{
    return s_cfg.rf_tx;
}

esp_err_t netcom_set_rf_tx(bool on)
{
    if (s_cfg.rf_tx == on) {
        return ESP_OK;
    }
    netcom_cfg_t next = s_cfg;
    next.rf_tx = on;
    const esp_err_t err = netcom_cfg_save(&next);
    if (err == ESP_OK) {
        s_cfg.rf_tx = on;
        ESP_LOGI(TAG, "network talk: local RF transmission %s", on ? "ON" : "OFF");
    }
    return err;
}

const char *netcom_tx_ready_hint(void)
{
    if (s_cfg.mode == NETCOM_MODE_RELAY) {
        return "网络中继未就绪：EMQX 未连接（检查 broker 地址与用户名密码）";
    }
    if (s_cfg.mode == NETCOM_MODE_NETP2P) {
        return "公网直连未就绪：还没有对端连通（在面板添加对端并等待打洞完成）";
    }
    return "网络未就绪";
}

void netcom_test_tone(int ms)
{
    const netcom_engine_t *e = active_engine();
    if (e && e->test_tone) {
        e->test_tone(ms);
    }
}

/* ------------------------------------------------------------------------ */
uint32_t netcom_tx_qdrop(void)
{
    /* Frames the relay's TX queue had to drop when the broker stalled (P2P has
     * no such queue and always reports 0). */
    return (s_cfg.mode == NETCOM_MODE_RELAY) ? netrelay_tx_qdrop() : 0;
}

void netcom_stats(uint32_t *tx_msgs, uint32_t *rx_msgs, int *peers, const char **state)
{
    *tx_msgs = 0; *rx_msgs = 0; *peers = 0; *state = "idle";
    const netcom_engine_t *e = active_engine();
    if (e && e->stats) {
        e->stats(tx_msgs, rx_msgs, peers, state);
    } else if (s_cfg.mode != NETCOM_MODE_LOCAL && !e) {
        *state = "no-engine";
    }
}

void netcom_diag(char *out, size_t n)
{
    out[0] = '\0';
    const netcom_engine_t *e = active_engine();
    if (e && e->diag) {
        e->diag(out, n);
    }
}

/* ------------------------------------------------------------------- init */

esp_err_t netcom_init(void)
{
    netcom_cfg_load(&s_cfg);

    /* Placeholder engines for any mode whose real engine has not registered
     * yet, so mode switching stays exercisable end to end.  netp2pnet.c and
     * netrelay.c replace them at boot via netcom_register_engine(). */
    if (!s_engine[NETCOM_MODE_NETP2P]) {
        s_engine[NETCOM_MODE_NETP2P] = &s_stub;
    }
    if (!s_engine[NETCOM_MODE_RELAY]) {
        s_engine[NETCOM_MODE_RELAY] = &s_stub;
    }

    ESP_LOGI(TAG, "init: mode=%s room=%u broker=%s psk=%s",
             netcom_mode_name(s_cfg.mode), s_cfg.room,
             s_cfg.broker,
             s_cfg.psk_set ? "set" : "off");

    return engine_start(active_engine());
}
