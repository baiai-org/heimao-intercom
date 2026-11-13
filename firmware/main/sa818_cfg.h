/*
 * Radio operating profile -- OpenRadioLink-v0.3.
 *
 * Old module revisions do not provide configuration read-back. The operator's
 * NVS profile remains authoritative; do not infer the hardware band by tuning.
 *
 * Transmitting is a licensed activity.  Nothing in this file keys the radio;
 * that is ptt.c's job, and ptt.c refuses to key unless explicitly asked.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "sa818_freq.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int   bandwidth;          /* 0 = 12.5 kHz narrow, 1 = 25 kHz wide        */
    float tx_mhz;
    float rx_mhz;
    char  tx_ctcss[8];        /* "0000" off, NS code e.g. "0008", DCS "754N" */
    int   squelch;            /* 0..8, 0 = monitor                            */
    char  rx_ctcss[8];
    int   volume;             /* module AF volume 1..8                        */
    int   high_power;         /* 1 = release open drain; 0 = ground H/L      */
    bool  tail;               /* AT+SETTAIL                                    */
    int   filter_pre_deemph;  /* 1 = bypass, 0 = normal                       */
    int   filter_highpass;
    int   filter_lowpass;
} sa818_cfg_t;

/* No automatic frequency: fresh devices wait for panel configuration. */
void sa818_cfg_defaults(sa818_cfg_t *cfg);

/* Validate without changing operator values. */
esp_err_t sa818_cfg_validate(const sa818_cfg_t *cfg, char *log, size_t log_len);
bool sa818_cfg_compatible(const sa818_cfg_t *cfg);

/* Called after NVS initialization, before tasks. Hardware band has its own key;
 * the persisted profile and channel binary layout must not grow. Old UHF-only
 * installations retain their band and values; a fresh device starts UNSET. */
esp_err_t sa818_cfg_init(void);
sa818_band_t sa818_cfg_band(void);
esp_err_t sa818_cfg_set_band(sa818_band_t band);
bool sa818_cfg_tx_ready(void);
/* Power alone does not resend frequencies; hardware defers it while keyed. */
esp_err_t sa818_cfg_set_power(bool high, char *log, size_t log_len);

/* NVS-backed load/save.  Load falls back to the defaults when the namespace is
 * empty or the stored blob is the wrong size. */
esp_err_t sa818_cfg_load(sa818_cfg_t *cfg);

/*
 * Send the whole profile to the module and check the ACKs.
 * AT commands only -- this never keys the transmitter.
 * `log` receives a one-line human readable summary (may be NULL).
 */
esp_err_t sa818_cfg_apply(const sa818_cfg_t *cfg, char *log, size_t log_len);
/* Operator change: apply and persist under one configuration lock. */
esp_err_t sa818_cfg_update(const sa818_cfg_t *cfg, char *log, size_t log_len);
enum { SA818_EDIT_TX = 1u, SA818_EDIT_RX = 2u, SA818_EDIT_BW = 4u,
    SA818_EDIT_SQ = 8u, SA818_EDIT_VOLUME = 16u, SA818_EDIT_TXT = 32u,
    SA818_EDIT_RXT = 64u, SA818_EDIT_POWER = 128u, SA818_EDIT_TAIL = 256u,
    SA818_EDIT_FILTER_PRE = 512u, SA818_EDIT_FILTER_HP = 1024u, SA818_EDIT_FILTER_LP = 2048u };
esp_err_t sa818_cfg_update_fields(const sa818_cfg_t *values, uint32_t fields,
                                 char *log, size_t log_len);
/* Partial edits merge under the apply lock, avoiding stale snapshot overwrite. */
esp_err_t sa818_cfg_update_changed(const sa818_cfg_t *before, const sa818_cfg_t *after,
                                  char *log, size_t log_len);
typedef enum { SA818_APPLY_NONE, SA818_APPLY_BUSY, SA818_APPLY_GROUP,
    SA818_APPLY_VOLUME, SA818_APPLY_FILTER, SA818_APPLY_TAIL,
    SA818_APPLY_STORAGE, SA818_APPLY_READY } sa818_apply_stage_t;
typedef struct {
    unsigned revision, applied_revision, stage, attempts;
    int error;
    bool ready, recovery_pending, maintenance_pending;
} sa818_cfg_status_t;
/* Software ACK/commit state, never advertised as hardware parameter readback. */
void sa818_cfg_status(sa818_cfg_status_t *out);

/* Keep the physical band and saved channels; clear the operating profile. */
esp_err_t sa818_cfg_reset(void);

/*
 * Check after an over; healthy profiles are never periodically retuned. Recover
 * uncertain configuration only after one idle second and ten more. Recovery is bounded
 * to three attempts per generation and never rewrites unchanged NVS.
 */
void sa818_cfg_arm_refresh(void);
void sa818_cfg_refresh_tick(void);

void sa818_cfg_print(const sa818_cfg_t *cfg);

/* Convenience: change only the squelch and re-send the group command. */
esp_err_t sa818_cfg_set_squelch(int squelch, char *log, size_t log_len);

/*
 * Deferred application: store the profile now, push it to the module only once
 * the PTT has been idle and no call is running for 60 s (a retune mutes the
 * receiver and eats the head of the next over).
 */
esp_err_t sa818_cfg_store_only(const sa818_cfg_t *cfg);
void sa818_cfg_request_apply(void);
bool sa818_cfg_apply_pending(void);
int  sa818_cfg_apply_due_ms(void);   /* -1 = nothing pending */
void sa818_cfg_defer_tick(void);     /* call about once a second */

#ifdef __cplusplus
}
#endif
