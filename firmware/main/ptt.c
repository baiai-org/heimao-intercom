/*
 * PTT state machine -- implementation.
 *
 * See ptt.h for the safety rules this file exists to enforce.
 */
#include "ptt.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "board_hw.h"
#include "sa818_cfg.h"

static const char *TAG = "ptt";

static ptt_state_t s_state = PTT_IDLE;
static int64_t     s_state_entered_us;
static int64_t     s_last_tick_us;
static int64_t     s_tx_started_us;      /* when the radio was actually keyed */
static int64_t     s_tx_deadline_us;     /* absolute release time while keyed */
static int         s_tx_limit_ms;
static int         s_tx_count;
static const char *s_fault_reason;
static bool        s_dry;      /* dry-run: never drive the PTT pad */
static bool        s_rf_keying = true;   /* false: logical TX window only */
static volatile bool s_bridge_keyed;     /* analog-bridge keying domain */
static int64_t     s_bridge_until_us;    /* bridge watchdog deadline */

static StaticSemaphore_t s_lock_storage;
static SemaphoreHandle_t s_lock;
static portMUX_TYPE s_lock_init = portMUX_INITIALIZER_UNLOCKED;
static ptt_owner_t s_owner;
static uint32_t s_owner_token;
static bool s_local_audio;

void ptt_control_lock(void)
{
    taskENTER_CRITICAL(&s_lock_init);
    if (!s_lock) { s_lock = xSemaphoreCreateRecursiveMutexStatic(&s_lock_storage); }
    taskEXIT_CRITICAL(&s_lock_init);
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
}
void ptt_control_unlock(void) { xSemaphoreGiveRecursive(s_lock); }

static int64_t now_us(void) { return esp_timer_get_time(); }

static int elapsed_ms_since(int64_t us)
{
    return (int)((now_us() - us) / 1000);
}

static void enter(ptt_state_t st, const char *why)
{
    s_state = st;
    s_state_entered_us = now_us();
    switch (st) {
    case PTT_IDLE:    ESP_LOGI(TAG, "IDLE%s%s", why ? " -- " : "", why ? why : "");    break;
    case PTT_PREPARE: ESP_LOGI(TAG, "PREPARE%s%s", why ? " -- " : "", why ? why : ""); break;
    case PTT_TX:
        /* Set the absolute deadline here: this is the only place the radio is
         * keyed, so it is the only place the clock should start. */
        s_tx_started_us = now_us();
        s_tx_deadline_us = s_tx_started_us + (int64_t)s_tx_limit_ms * 1000;
        ESP_LOGW(TAG, "TX KEYED (limit %d ms)", s_tx_limit_ms);
        break;
    case PTT_TAIL:    ESP_LOGI(TAG, "TAIL%s%s", why ? " -- " : "", why ? why : "");    break;
    case PTT_FAULT:   ESP_LOGE(TAG, "FAULT -- transmitter released: %s",
                               why ? why : "unknown");                                 break;
    }
}

/*
 * The single driver for the PTT pad: the OR of the machine's physical TX
 * (state TX with RF keying enabled) and the analog-bridge domain.  Both the
 * machine transitions and the bridge API funnel through here, so the pad can
 * never be fought over.  A dry run freezes the pad exactly like before.
 */
static void pad_apply(void)
{
    if (s_dry) {
        return;
    }
    const bool machine = (s_state == PTT_TX) && s_rf_keying;
    board_ptt_assert(machine || s_bridge_keyed);
}

/* The one and only place the machine keys the radio. */
static void key_radio(void)
{
    if (s_dry) {
        /* Dry run: exercise the machine without emitting RF. */
        return;
    }
    ++s_tx_count;
    if (!s_rf_keying) {
        /* Logical TX window (P2P): the machine is in TX, the pad is not. */
        return;
    }
    pad_apply();
}

/* The one and only place the machine releases the radio. */
static void unkey_radio(void)
{
    pad_apply();   /* the pad stays low while the bridge domain is keyed */
}

bool ptt_claim(ptt_owner_t owner, uint32_t token)
{
    ptt_control_lock();
    bool ok = owner != PTT_OWNER_NONE && !s_dry && !s_local_audio;
    if (s_owner != PTT_OWNER_NONE) {
        ok = ok && s_owner == owner && s_owner_token == token;
    } else {
        ok = ok && !s_bridge_keyed &&
            (s_state == PTT_IDLE || (owner == PTT_OWNER_TALK && s_state == PTT_TAIL));
        if (ok) { s_owner = owner; s_owner_token = token; }
    }
    ptt_control_unlock();
    return ok;
}

bool ptt_local_audio_begin(void)
{
    ptt_control_lock();
    const bool ok = !s_local_audio && !s_dry && s_owner == PTT_OWNER_NONE &&
                    s_state == PTT_IDLE && !s_bridge_keyed;
    if (ok) { s_local_audio = true; }
    ptt_control_unlock();
    return ok;
}

void ptt_local_audio_end(void)
{
    ptt_control_lock();
    s_local_audio = false;
    ptt_control_unlock();
}

bool ptt_local_audio_active(void)
{
    ptt_control_lock();
    const bool active = s_local_audio;
    ptt_control_unlock();
    return active;
}

bool ptt_owned(ptt_owner_t owner, uint32_t token)
{
    ptt_control_lock();
    const bool ok = s_owner == owner && s_owner_token == token;
    ptt_control_unlock();
    return ok;
}

ptt_owner_t ptt_owner(void)
{
    ptt_control_lock();
    const ptt_owner_t owner = s_owner;
    ptt_control_unlock();
    return owner;
}

uint32_t ptt_owner_token(void)
{
    ptt_control_lock();
    const uint32_t token = s_owner_token;
    ptt_control_unlock();
    return token;
}

const char *ptt_owner_name(void)
{
    switch (ptt_owner()) {
    case PTT_OWNER_TALK: return "talk";
    case PTT_OWNER_NETWORK: return "network";
    case PTT_OWNER_SDR: return "browser";
    default: return "none";
    }
}

void ptt_release(ptt_owner_t owner, uint32_t token)
{
    ptt_control_lock();
    if (ptt_owned(owner, token)) {
        if (owner == PTT_OWNER_TALK) {
            ptt_request_rx();
            s_rf_keying = true;
        } else {
            s_bridge_keyed = false;
        }
        s_owner = PTT_OWNER_NONE;
        s_owner_token = 0;
        pad_apply();
    }
    ptt_control_unlock();
}

bool ptt_source_key(ptt_owner_t owner, uint32_t token, bool on)
{
    ptt_control_lock();
    bool ok = owner == PTT_OWNER_NETWORK || owner == PTT_OWNER_SDR;
    if (on) {
        ok = ok && sa818_cfg_tx_ready() && s_state == PTT_IDLE && ptt_claim(owner, token);
        if (ok) {
            if (!s_bridge_keyed) { ESP_LOGW(TAG, "TX owner=%s token=%lu", ptt_owner_name(), (unsigned long)token); }
            s_bridge_keyed = true;
            s_bridge_until_us = now_us() + (int64_t)PTT_BRIDGE_MAX_MS * 1000;
            pad_apply();
        } else if (ptt_owned(owner, token)) {
            ptt_release(owner, token);
        }
    } else if (ok) {
        ptt_release(owner, token);  /* an idle network task cannot release SDR */
    }
    ptt_control_unlock();
    return ok;
}

void ptt_bridge_key(bool on)
{
    (void)ptt_source_key(PTT_OWNER_NETWORK, 1, on);
}

static bool ptt_bridge_keyed_locked(void)
{
    return s_bridge_keyed;
}

static bool ptt_rf_keying_enabled_locked(void)
{
    return s_rf_keying;
}

static void ptt_set_rf_keying_locked(bool enabled)
{
    if (s_rf_keying == enabled) {
        return;
    }
    s_rf_keying = enabled;
    pad_apply();
    ESP_LOGI(TAG, "RF keying %s (the TX window is %s)",
             enabled ? "enabled" : "disabled",
             enabled ? "real again" : "logical only, radio stays in RX");
}

static esp_err_t ptt_init_locked(void)
{
    s_state = PTT_IDLE;
    s_state_entered_us = now_us();
    s_last_tick_us = s_state_entered_us;
    s_tx_limit_ms = PTT_TX_LIMIT_DEFAULT;
    s_tx_count = 0;
    s_fault_reason = NULL;
    unkey_radio();
    ESP_LOGI(TAG, "initialised: PTT released (GPIO%d = %d), radio in RX",
             PIN_SA818_PTT, gpio_get_level(PIN_SA818_PTT));
    return ESP_OK;
}

static esp_err_t ptt_request_tx_locked(int max_ms)
{
    if (!s_dry && s_rf_keying && !sa818_cfg_tx_ready()) {
        ESP_LOGW(TAG, "RF TX refused: configure installed module band/profile in panel");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_local_audio || s_state != PTT_IDLE || s_bridge_keyed ||
        (s_owner != PTT_OWNER_NONE && s_owner != PTT_OWNER_TALK)) {
        ESP_LOGW(TAG, "TX refused: state is %s", ptt_state_name());
        return ESP_ERR_INVALID_STATE;
    }
    if (max_ms <= 0 || max_ms > PTT_TX_LIMIT_DEFAULT) {
        max_ms = PTT_TX_LIMIT_DEFAULT;
    }
    if (max_ms < 100) {
        max_ms = 100;
    }
    if (s_owner == PTT_OWNER_NONE) { s_owner = PTT_OWNER_TALK; s_owner_token = 0; }
    s_tx_limit_ms = max_ms;
    s_fault_reason = NULL;
    enter(PTT_PREPARE, NULL);
    return ESP_OK;
}

static void ptt_request_rx_locked(void)
{
    if (s_owner == PTT_OWNER_TALK) { s_owner = PTT_OWNER_NONE; s_owner_token = 0; }
    s_fault_reason = NULL;
    switch (s_state) {
    case PTT_PREPARE:
        /* Never actually keyed, so go straight back. */
        enter(PTT_IDLE, "cancelled before keying");
        unkey_radio();
        break;
    case PTT_TX:
        enter(PTT_TAIL, "requested");
        unkey_radio();
        break;
    case PTT_TAIL:
    case PTT_IDLE:
        unkey_radio();
        break;
    case PTT_FAULT:
        enter(PTT_IDLE, "cleared by request");
        unkey_radio();
        break;
    }
}

static void ptt_tick_locked(void)
{
    const int64_t t = now_us();

    /* Watchdog: if this function stopped being called while keyed, the radio
     * must not stay keyed.  Checked before anything else. */
    if (s_state == PTT_TX && elapsed_ms_since(s_last_tick_us) > PTT_TICK_TIMEOUT_MS) {
        s_fault_reason = "ptt_tick() stalled while keyed";
        enter(PTT_FAULT, s_fault_reason);
        unkey_radio();
        s_last_tick_us = t;
        return;
    }
    s_last_tick_us = t;

    /*
     * Analog-bridge keying watchdog: the playback task refreshes the bridge
     * key with every active packet.  If it dies with the radio keyed (crash,
     * engine stopped mid-tone, blocked driver) this releases the pad, so a
     * stuck bridge can never hold the transmitter on.
     */
    if (s_bridge_keyed && t >= s_bridge_until_us) {
        s_bridge_keyed = false;
        s_owner = PTT_OWNER_NONE; s_owner_token = 0;
        pad_apply();
        ESP_LOGE(TAG, "bridge key watchdog -- RF released after %d ms",
                 PTT_BRIDGE_MAX_MS);
    }

    switch (s_state) {
    case PTT_IDLE:
        break;

    case PTT_PREPARE:
        if (elapsed_ms_since(s_state_entered_us) >= PTT_PREPARE_MS) {
            enter(PTT_TX, NULL);
            key_radio();
        }
        break;

    case PTT_TX:
        if (t >= s_tx_deadline_us) {
            s_fault_reason = "TX deadline reached";
            enter(PTT_TAIL, "TX deadline reached");
            unkey_radio();
        }
        break;

    case PTT_TAIL:
        if (elapsed_ms_since(s_state_entered_us) >= PTT_TAIL_MS) {
            enter(PTT_IDLE, NULL);
            /* Token zero is the legacy/manual carrier-test caller. Voice
             * tokens remain reserved until their audio task acknowledges. */
            if (s_owner == PTT_OWNER_TALK && s_owner_token == 0) { s_owner = PTT_OWNER_NONE; }
        }
        break;

    case PTT_FAULT:
        /* Stays released until someone calls ptt_request_rx(). */
        break;
    }
}

static ptt_state_t ptt_get_state_locked(void) { return s_state; }

static const char * ptt_state_name_locked(void)
{
    if (s_bridge_keyed && s_state == PTT_IDLE) {
        return "TX(BRIDGE)";
    }
    switch (s_state) {
    case PTT_IDLE:    return "IDLE";
    case PTT_PREPARE: return "PREPARE";
    case PTT_TX:      return "TX";
    case PTT_TAIL:    return "TAIL";
    case PTT_FAULT:   return "FAULT";
    }
    return "?";
}

static int ptt_tx_elapsed_ms_locked(void)
{
    if (s_state != PTT_TX) {
        return 0;
    }
    return elapsed_ms_since(s_state_entered_us);
}

static esp_err_t ptt_extend_locked(int ms)
{
    if (s_state != PTT_TX) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ms <= 0) {
        ms = 1000;
    }
    const int64_t want = now_us() + (int64_t)ms * 1000;
    const int64_t hard = s_tx_started_us + (int64_t)PTT_TX_ABSOLUTE_MAX_MS * 1000;
    s_tx_deadline_us = (want < hard) ? want : hard;
    return ESP_OK;
}

static int ptt_tx_remaining_ms_locked(void)
{
    if (s_state != PTT_TX) {
        return 0;
    }
    const int64_t left = (s_tx_deadline_us - now_us()) / 1000;
    return left > 0 ? (int)left : 0;
}

static int ptt_tx_limit_ms_locked(void)    { return s_tx_limit_ms; }
static int ptt_tx_count_locked(void)       { return s_tx_count; }
static bool ptt_is_transmitting_locked(void) { return s_state == PTT_TX || s_bridge_keyed; }

esp_err_t ptt_dry_run(int tx_ms, ptt_dry_result_t *out)
{
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    ptt_control_lock();
    if (s_state != PTT_IDLE || s_owner != PTT_OWNER_NONE || s_dry || s_local_audio) {
        ptt_control_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (tx_ms < 200 || tx_ms > 5000) {
        tx_ms = 600;
    }
    const int keyings_before = s_tx_count;
    s_dry = true;
    esp_err_t err = ptt_request_tx(tx_ms);
    if (err != ESP_OK) {
        s_dry = false;
        ptt_control_unlock();
        return err;
    }
    ptt_control_unlock();
    if (out) { out->reached_prepare = true; }

    const int64_t t0 = now_us();
    int64_t t_prepare = 0, t_tx = 0, t_tail = 0;
    bool saw_tx = false, saw_tail = false;

    /*
     * Drive the machine from here instead of relying on the ptt task, so the
     * run is deterministic.  The ptt task simply finds nothing to do when it
     * gets a turn.
     */
    for (int guard = 0; guard < 4000; ++guard) {
        ptt_tick();
        const ptt_state_t st = ptt_get_state();
        const int64_t t = now_us();
        if (!saw_tx && st == PTT_TX) {
            saw_tx = true;
            t_prepare = t;
        }
        if (saw_tx && !saw_tail && st == PTT_TAIL) {
            saw_tail = true;
            t_tx = t;
            if (out) { out->deadline_fired = true; }
        }
        if (saw_tail && st == PTT_IDLE) {
            t_tail = t;
            break;
        }
        if (st == PTT_FAULT) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    if (out) {
        out->reached_tx   = saw_tx;
        out->reached_tail = saw_tail;
        out->ended_idle   = (ptt_get_state() == PTT_IDLE);
        out->prepare_ms   = t_prepare ? (int)((t_prepare - t0) / 1000) : 0;
        out->tx_ms        = (saw_tx && saw_tail) ? (int)((t_tx - t_prepare) / 1000) : 0;
        out->tail_ms      = (saw_tail && t_tail) ? (int)((t_tail - t_tx) / 1000) : 0;
    }

    /* Belt and braces: leave both the machine and the pad released. */
    ptt_control_lock();
    ptt_request_rx();
    s_dry = false;
    unkey_radio();
    ptt_control_unlock();

    if (out) {
        out->keyings = s_tx_count - keyings_before;
        out->pin_level_after = gpio_get_level(PIN_SA818_PTT);
    }
    return ESP_OK;
}

/* Public API: all state and 64-bit deadlines are protected across cores. */
bool ptt_bridge_keyed(void) { ptt_control_lock(); bool result = ptt_bridge_keyed_locked(); ptt_control_unlock(); return result; }
bool ptt_rf_keying_enabled(void) { ptt_control_lock(); bool result = ptt_rf_keying_enabled_locked(); ptt_control_unlock(); return result; }
void ptt_set_rf_keying(bool enabled) { ptt_control_lock(); ptt_set_rf_keying_locked(enabled); ptt_control_unlock(); }
esp_err_t ptt_init(void) { ptt_control_lock(); esp_err_t result = ptt_init_locked(); ptt_control_unlock(); return result; }
esp_err_t ptt_request_tx(int max_ms) { ptt_control_lock(); esp_err_t result = ptt_request_tx_locked(max_ms); ptt_control_unlock(); return result; }
void ptt_request_rx(void) { ptt_control_lock(); ptt_request_rx_locked(); ptt_control_unlock(); }
void ptt_tick(void) { ptt_control_lock(); ptt_tick_locked(); ptt_control_unlock(); }
ptt_state_t ptt_get_state(void) { ptt_control_lock(); ptt_state_t result = ptt_get_state_locked(); ptt_control_unlock(); return result; }
const char * ptt_state_name(void) { ptt_control_lock(); const char * result = ptt_state_name_locked(); ptt_control_unlock(); return result; }
int ptt_tx_elapsed_ms(void) { ptt_control_lock(); int result = ptt_tx_elapsed_ms_locked(); ptt_control_unlock(); return result; }
esp_err_t ptt_extend(int ms) { ptt_control_lock(); esp_err_t result = ptt_extend_locked(ms); ptt_control_unlock(); return result; }
int ptt_tx_remaining_ms(void) { ptt_control_lock(); int result = ptt_tx_remaining_ms_locked(); ptt_control_unlock(); return result; }
int ptt_tx_limit_ms(void) { ptt_control_lock(); int result = ptt_tx_limit_ms_locked(); ptt_control_unlock(); return result; }
int ptt_tx_count(void) { ptt_control_lock(); int result = ptt_tx_count_locked(); ptt_control_unlock(); return result; }
bool ptt_is_transmitting(void) { ptt_control_lock(); bool result = ptt_is_transmitting_locked(); ptt_control_unlock(); return result; }
