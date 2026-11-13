/*
 * PTT state machine -- OpenRadioLink-v0.3.
 *
 * This is the ONLY code in the firmware allowed to key the transmitter, and it
 * is deliberately paranoid, because a stuck PTT on a 1 W UHF transmitter is a
 * regulatory and thermal problem, not just a bug.
 *
 * Rules enforced here:
 *   1. The PTT pad is driven from exactly one helper (pad_apply) inside
 *      ptt_tick() and the bridge-key API below.  The pad level is the OR of
 *      two keying domains: the operator's machine (PREPARE -> TX with RF
 *      keying enabled) and the bridge/browser domain. Source ownership makes
 *      them mutually exclusive. Nothing
 *      outside this module may touch the pad.
 *   2. PTT is ACTIVE LOW and R15 pulls it HIGH, so from power-up to the first
 *      explicit request the hardware is already in receive.  ptt_init() then
 *      drives it high explicitly.
 *   3. Every transmission has a hard deadline.  When it expires, ptt_tick()
 *      releases PTT regardless of what the caller wanted.
 *   4. ptt_tick() must be called at least every PTT_TICK_TIMEOUT_MS.  If the
 *      serving task stalls (crash, blocked driver, watchdog bait) the state
 *      machine notices on its next run and drops out of TX.
 *   5. Keying must be asked for explicitly and can be retracted at any time;
 *      ptt_request_rx() is idempotent and always safe.
 *
 * Transmitting also requires a licensed operator and a matched antenna or
 * dummy load.  The firmware cannot check that, so it is stated here.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PTT_IDLE = 0,   /* receiving                                        */
    PTT_PREPARE,    /* requested; audio path settling, not yet keyed    */
    PTT_TX,         /* keyed -- the only transmitting state             */
    PTT_TAIL,       /* unkeyed, holding off before handing RX back      */
    PTT_FAULT       /* released by the safety net; needs a manual clear */
} ptt_state_t;

/* One TX owner at a time, including a reservation before the DAC changes.
 * Tokens identify a single transmission. Lock order: voice control -> PTT
 * control -> audio ring. Never wait for an audio task while holding this lock. */
typedef enum { PTT_OWNER_NONE, PTT_OWNER_TALK, PTT_OWNER_NETWORK, PTT_OWNER_SDR } ptt_owner_t;
void ptt_control_lock(void);
void ptt_control_unlock(void);
bool ptt_claim(ptt_owner_t owner, uint32_t token);
/* Local playback diagnostic reserves the shared DAC without keying anything.
 * Every TX claim is refused until buffered test PCM has drained. */
bool ptt_local_audio_begin(void);
void ptt_local_audio_end(void);
bool ptt_local_audio_active(void);
bool ptt_owned(ptt_owner_t owner, uint32_t token);
ptt_owner_t ptt_owner(void);
uint32_t ptt_owner_token(void);
const char *ptt_owner_name(void);
bool ptt_source_key(ptt_owner_t owner, uint32_t token, bool on);
void ptt_release(ptt_owner_t owner, uint32_t token);

#define PTT_PREPARE_MS          30
#define PTT_TAIL_MS             50
#define PTT_TICK_TIMEOUT_MS    250    /* ptt_tick() must run at least this often */
#define PTT_TX_LIMIT_DEFAULT  30000   /* hard ceiling for a normal transmission  */
#define PTT_TX_LIMIT_TEST      2000   /* ceiling for the manual carrier test     */
/*
 * Absolute ceiling on one transmission.  A voice session extends its deadline
 * repeatedly (see ptt_extend), but never past this, so a stuck talk task cannot
 * hold the transmitter on indefinitely.
 */
#define PTT_TX_ABSOLUTE_MAX_MS 180000
/*
 * Absolute ceiling on bridge keying: the playback task refreshes the bridge
 * key with every active packet, and if it stops refreshing (crash, hang, the
 * engine stopped) ptt_tick() releases the pad after this.
 */
#define PTT_BRIDGE_MAX_MS     30000

/* Drives PTT high and enters IDLE.  Call once, early. */
esp_err_t ptt_init(void);

/*
 * Logical TX window (network intercom): while disabled, the state machine
 * still runs PREPARE -> TX -> deadline -> TAIL, so every safety rule
 * (hard ceiling, retraction, watchdog) keeps applying, but the PTT pad is
 * never driven -- the SA818 stays in RX and the audio goes out over the
 * network instead of the air.  Re-enable before the next real key-up.
 */
void ptt_set_rf_keying(bool enabled);

/* Is the RF keying domain enabled right now?  False means the PTT machine may
 * be in TX but the radio is deliberately left in RX (network talk).  The panel
 * uses this to say what is actually transmitting. */
bool ptt_rf_keying_enabled(void);

/*
 * Network bridge compatibility API. This always refers to NETWORK token 1;
 * an idle/stop callback can never release a browser or TALK transmission.
 * Browser TX uses ptt_source_key(SDR, transmission_token, on) instead.
 *
 * Call ptt_bridge_key(true) repeatedly (once per active packet) while the
 * RF should stay keyed: each call refreshes the PTT_BRIDGE_MAX_MS watchdog.
 * ptt_bridge_key(false) releases the pad when the bridge domain is done.
 * Both are safe to call from any task.
 */
void ptt_bridge_key(bool on);

/* True while the bridge domain holds the pad. */
bool ptt_bridge_keyed(void);

/* Ask to transmit.  `max_ms` is clamped to [100, PTT_TX_LIMIT_DEFAULT].
 * Refused (returns ESP_ERR_INVALID_STATE) unless the machine is IDLE. */
esp_err_t ptt_request_tx(int max_ms);

/*
 * Push the transmission deadline out by `ms` from now.  Only meaningful while
 * actually transmitting, and only up to PTT_TX_ABSOLUTE_MAX_MS from the moment
 * the radio was keyed.
 *
 * This is how a voice session stays keyed: it re-extends every audio block.  If
 * it stops doing so -- crash, hang, blocked driver -- the deadline fires within
 * the previously granted window and the transmitter is released.  The "always a
 * deadline" property therefore survives, which is the whole point.
 */
esp_err_t ptt_extend(int ms);

/* Milliseconds left before the current transmission is force-released. */
int ptt_tx_remaining_ms(void);

/* Release.  Idempotent; always safe to call; also clears PTT_FAULT. */
void ptt_request_rx(void);

/*
 * Advance the state machine.  Must be called every few tens of milliseconds.
 * Enforces the TX deadline and the tick watchdog.
 */
void ptt_tick(void);

ptt_state_t ptt_get_state(void);
const char *ptt_state_name(void);

int  ptt_tx_elapsed_ms(void);
int  ptt_tx_limit_ms(void);
int  ptt_tx_count(void);         /* how many times the radio has been keyed */
bool ptt_is_transmitting(void);

/* Result of a dry run (see ptt_dry_run). */
typedef struct {
    bool reached_prepare;
    bool reached_tx;
    bool reached_tail;
    bool ended_idle;
    bool deadline_fired;     /* the TX hard limit released the radio */
    int  prepare_ms;
    int  tx_ms;
    int  tail_ms;
    int  keyings;            /* must stay 0: a dry run never keys */
    int  pin_level_after;    /* PTT pad after the run; 1 == released */
} ptt_dry_result_t;

/*
 * Exercise PREPARE -> TX -> TAIL -> IDLE with the radio NOT keyed: key_radio()
 * becomes a no-op for the duration, so this is safe on a bench with no antenna
 * or dummy load attached.
 *
 * This exists because the alternative way to test the state machine is to
 * actually transmit, and that is a licensed act.  It verifies the transition
 * order, that the TX deadline fires, and that the machine returns to IDLE with
 * the pad still released.
 */
esp_err_t ptt_dry_run(int tx_ms, ptt_dry_result_t *out);

/* Bounded task-context event history; no PCM, credentials or addresses. */
enum { PTT_TRACE_RF_ON=1, PTT_TRACE_RF_OFF, PTT_TRACE_COS_ON, PTT_TRACE_COS_OFF,
       PTT_TRACE_RFNET_ON, PTT_TRACE_RFNET_OFF, PTT_TRACE_NET_ON,
       PTT_TRACE_NET_OFF, PTT_TRACE_NET_TIMEOUT, PTT_TRACE_CFG_OK, PTT_TRACE_CFG_FAIL };
typedef struct { uint32_t ms, sequence; uint16_t kind, source; uint32_t a, b; } ptt_trace_event_t;
void ptt_trace_note(unsigned kind, unsigned source, uint32_t a, uint32_t b);
size_t ptt_trace_snapshot(ptt_trace_event_t *out, size_t capacity, uint32_t *sequence);

#ifdef __cplusplus
}
#endif
