/*
 * netrelay -- EMQX/MQTT relay engine (netcom mode 2).  See netrelay.c.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Register the relay engine; call before netcom_init(). */
void netrelay_init(void);

/* Frames the TX queue had to drop because the broker stalled (panel counter). */
uint32_t netrelay_tx_qdrop(void);

#ifdef __cplusplus
}
#endif
