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
/* Keep the physical band and saved channels; clear the operating profile. */
esp_err_t sa818_cfg_reset(void);

/*
 * Re-send the profile once after an over, but only when the station has been
 * idle for a second and stays idle for five more.  Silent by design.
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
