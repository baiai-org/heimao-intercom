/*
 * Preset channels.
 *
 * A channel is a whole radio setting under a name: the two frequencies, the
 * bandwidth, the squelch, both subtones and the power setting.  Storing the
 * configuration struct itself rather than a hand-picked subset means a channel
 * preserves the complete operator profile. Keep its historical binary layout.
 *
 * Stored in SPIFFS, with a one-time migration from the legacy NVS namespace.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "sa818_cfg.h"

#define CHANNELS_MAX     99
#define CHANNEL_NAME_LEN 24
#define CHANNELS_JSON_MAX (CHANNELS_MAX * 768 + 3)

void channels_init(void);

int  channels_count(void);

/* Returns false when the index is out of range or the slot is empty. */
bool channels_get(int index, char *name, size_t name_len, sa818_cfg_t *out);

/* Index -1 or >= count appends.  Returns the slot used, or -1 on failure. */
int  channels_save(int index, const char *name, const sa818_cfg_t *cfg);

bool channels_delete(int index);

/* [{"name":"...","tx":438.5,"rx":438.5,"bw":1,"sq":4,"txt":"0008","rxt":"0008"}, ...] */
int  channels_list_json(char *out, size_t out_len);
