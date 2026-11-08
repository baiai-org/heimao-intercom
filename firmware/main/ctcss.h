/*
 * CTCSS tone table -- OpenRadioLink-v0.3
 *
 * The SA818 generates and decodes the sub-audible tone itself; nothing is
 * synthesised in software here.  What this module provides is the mapping
 * between the two ways a tone is named:
 *
 *   - the 4-character code the AT command actually takes ("0009"), and
 *   - the frequency people know it by (88.5 Hz).
 *
 * The ordering is the EIA/TIA-603 standard tone list, which is the same table
 * printed in the SA818 manual and used by essentially every Chinese radio
 * module: 67.0 Hz upwards in the usual sequence, then 254.1 Hz as an extra.
 *
 * Tone numbers run from 1, so code "0001" is 67.0 Hz and "0039" is 254.1 Hz.
 * DCS codes are not in this table; the SA818 takes them in a different form
 * ("023N", "023I") and they pass through as typed.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Frequency for a 4-digit CTCSS code, or 0 when the code is not a tone we
 * know.  "0000" (off) also returns 0, so callers that need to distinguish
 * must check for it explicitly. */
float ctcss_hz_from_code(const char *code);

/* Build the code for a frequency, matching to the nearest standard tone within
 * a small tolerance so that "88.5" and "088.5" and "88.50" all work.  Returns
 * false when nothing matches. */
bool ctcss_code_from_hz(float hz, char out[8]);

/* Accepts what an operator would plausibly type and normalises it to a code:
 *   "0000"/"off"/""   -> off         -> true, out = "0000"
 *   "88.5"            -> "0009"
 *   "9"               -> "0009"
 *   "0009"            -> "0009"
 * Returns false (and leaves out as "0000") when the value makes no sense.
 * A value already in SA818 DCS form ("023N") is passed through unchanged. */
bool ctcss_normalise(const char *input, char out[8]);

/* True for the "off" value. */
bool ctcss_is_off(const char *code);

/* Human-readable form for logs and the panel: "88.5 Hz" or "off". */
const char *ctcss_describe(const char *code, char *buf, size_t buf_len);

/* The whole table as JSON, for the panel's dropdowns:
 *   [{"code":"0009","hz":88.5,"label":"88.5 Hz"}, ...]
 * Returns the number of entries written. */
int ctcss_list_json(char *out, size_t out_len);

/* The digital codes from the manual's Appendix 2, as JSON:
 *   [{"code":"023","label":"023"}, ...]
 * Both N and I forms exist for every code, so the panel offers the code and
 * the polarity separately rather than a list twice the length. */
int ctcss_dcs_list_json(char *out, size_t out_len);
