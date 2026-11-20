/*
 * netrelay -- EMQX/MQTT relay engine (netcom mode 2).  See netrelay.c.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
unsigned netrelay_cleanup_idle(int64_t now, int64_t idle_ms);

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Register the relay engine; call before netcom_init(). */
void netrelay_init(void);

typedef struct {
    const char *state;
    const char *reason;
} netrelay_link_t;

/* Broker publish availability and the separately confirmed receive link. */
bool netrelay_broker_connected(void);
netrelay_link_t netrelay_link_status(void);

/* Frames the TX queue had to drop because the broker stalled (panel counter). */
uint32_t netrelay_tx_qdrop(void);

#ifdef __cplusplus
}
#endif
