/*
 * CTCSS tone table -- implementation.
 */
#include "ctcss.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The standard list.  Index 0 of the array is tone number 1, so the code is
 * the array index plus one; keeping that arithmetic in one place is the whole
 * point of the table.
 */
static const float TONES[] = {
    /*
     * The SA818S manual, Appendix 3 "Analog subtone comparison table",
     * transcribed literally.  38 entries, coding 1..38.
     *
     * NOTE THE GAP AT THE BOTTOM: the sequence runs 67, 71.9, 74.4 ... -- there
     * is NO 69.3 Hz, which the generic tone lists that circulate online do
     * include.  An earlier version of this table was built from such a list and
     * was therefore shifted by one position from entry 2 onwards, so every tone
     * except the first was wrong.
     *
     * Measured on the air against a service monitor, before the manual was
     * available: coding 0001 -> 67.0 Hz, 0009 -> 91.5 Hz, 0012 -> 100.0 Hz.
     * All three agree with this table.  (They also happen to agree with a table
     * missing 88.5 Hz instead, which is why measurement alone could not settle
     * it -- the manual does.)
     *
     * 88.5 Hz is coding 0008 and is perfectly usable.
     */
    67.0f,  71.9f,  74.4f,  77.0f,  79.7f,  82.5f,  85.4f,  88.5f,
    91.5f,  94.8f,  97.4f, 100.0f, 103.5f, 107.2f, 110.9f, 114.8f,
   118.8f, 123.0f, 127.3f, 131.8f, 136.5f, 141.3f, 146.2f, 151.4f,
   156.7f, 162.2f, 167.9f, 173.8f, 179.9f, 186.2f, 192.8f, 203.5f,
   210.7f, 218.1f, 225.7f, 233.6f, 241.8f, 250.3f,
};
#define TONE_COUNT ((int)(sizeof(TONES) / sizeof(TONES[0])))

float ctcss_hz_from_code(const char *code)
{
    if (!code) {
        return 0.0f;
    }
    const int n = atoi(code);
    if (n < 1 || n > TONE_COUNT) {
        return 0.0f;
    }
    return TONES[n - 1];
}

bool ctcss_code_from_hz(float hz, char out[8])
{
    if (hz <= 0.0f) {
        return false;
    }
    for (int i = 0; i < TONE_COUNT; ++i) {
        /* 0.05 Hz is well inside the tolerance of every radio that will ever
         * hear this, and wide enough that "88.5" typed as 88.49 or 88.51
         * still lands on the right tone. */
        const float d = TONES[i] - hz;
        if (d < 0.05f && d > -0.05f) {
            snprintf(out, 8, "%04d", i + 1);
            return true;
        }
    }
    return false;
}

bool ctcss_is_off(const char *code)
{
    if (!code || !code[0]) {
        return true;
    }
    if (!strcmp(code, "0000") || !strcmp(code, "off") || !strcmp(code, "OFF")) {
        return true;
    }
    return false;
}

bool ctcss_normalise(const char *input, char out[8])
{
    snprintf(out, 8, "0000");
    if (!input) {
        return true;                       /* nothing asked for == off */
    }

    /* Skip leading spaces, since this comes from a console line. */
    while (*input == ' ') {
        ++input;
    }
    if (!*input) {
        return true;
    }
    if (ctcss_is_off(input)) {
        return true;
    }

    /*
     * DCS (digital subtone): three OCTAL digits plus N (normal) or I
     * (inverted), exactly as SA818S manual Appendix 2 tabulates it -- 023N,
     * 025N, ... 754I and so on.
     *
     * Validated rather than passed through: the octal range is 0-7 per digit,
     * so "289N" or "02XN" is a typing mistake, and a mistake that reaches the
     * module either selects an unintended code or is silently ignored.  Saying
     * "unknown tone" is more useful than forwarding it.
     */
    const size_t len = strlen(input);
    if (len >= 4 && (input[len - 1] == 'N' || input[len - 1] == 'I'
                     || input[len - 1] == 'n' || input[len - 1] == 'i')) {
        bool octal = true;
        for (size_t k = 0; k < len - 1; ++k) {
            if (input[k] < '0' || input[k] > '7') {
                octal = false;
                break;
            }
        }
        if (octal && len - 1 == 3) {
            snprintf(out, 8, "%c%c%c%c", input[0], input[1], input[2],
                     (input[len - 1] == 'I' || input[len - 1] == 'i') ? 'I' : 'N');
            return true;
        }
        return false;
    }

    /* A frequency: anything containing a decimal point, or a value above the
     * tone-number range. */
    const bool decimal = (strchr(input, '.') != NULL);
    const float value = strtof(input, NULL);
    if (decimal || value > (float)TONE_COUNT) {
        return ctcss_code_from_hz(value, out);
    }

    /* Otherwise it is a tone number. */
    const int n = atoi(input);
    if (n >= 1 && n <= TONE_COUNT) {
        snprintf(out, 8, "%04d", n);
        return true;
    }
    return false;
}

const char *ctcss_describe(const char *code, char *buf, size_t buf_len)
{
    if (ctcss_is_off(code)) {
        snprintf(buf, buf_len, "off");
        return buf;
    }
    const float hz = ctcss_hz_from_code(code);
    if (hz > 0.0f) {
        /* One decimal: every standard tone has exactly one. */
        snprintf(buf, buf_len, "%.1f Hz (%s)", (double)hz, code);
        return buf;
    }
    /* DCS: three octal digits + N/I, per manual Appendix 2. */
    snprintf(buf, buf_len, "DCS %s%s", code,
             (code[strlen(code) - 1] == 'I') ? " (inverted)" : " (normal)");
    return buf;
}

/*
 * Digital codes, SA818S manual Appendix 2.
 *
 * Transcribed from the manual itself rather than from one of the DCS lists that
 * circulate online: those disagree with each other about which codes are in the
 * set, and a code that is not in the module's table cannot be selected at all.
 * 83 entries.
 *
 * Each code exists in two forms, normal (N) and inverted (I); the polarity is
 * chosen separately in the user interface.
 */
static const char *DCS_CODES[] = {
    "023", "025", "026", "031", "032", "043", "047", "051", "054", "065", "071", "072", "073", "074",
    "114", "115", "116", "125", "131", "132", "134", "143", "152", "155", "156", "162", "165", "172",
    "174", "205", "223", "226", "243", "244", "245", "251", "261", "263", "265", "271", "306", "311",
    "315", "331", "343", "346", "351", "364", "365", "371", "411", "412", "413", "423", "431", "432",
    "445", "464", "465", "466", "503", "506", "516", "532", "546", "565", "606", "612", "624", "627",
    "631", "632", "654", "662", "664", "703", "712", "723", "731", "732", "734", "743", "754",
};
#define DCS_COUNT ((int)(sizeof(DCS_CODES) / sizeof(DCS_CODES[0])))

int ctcss_dcs_list_json(char *out, size_t out_len)
{
    int n = 0;
    size_t o = 0;
    if (!out || out_len < 8) {
        return 0;
    }
    o += (size_t)snprintf(out + o, out_len - o, "[");
    for (int i = 0; i < DCS_COUNT; ++i) {
        if (o + 40 >= out_len) {
            break;
        }
        o += (size_t)snprintf(out + o, out_len - o,
                              "%s{\"code\":\"%s\",\"label\":\"%s\"}",
                              n ? "," : "", DCS_CODES[i], DCS_CODES[i]);
        ++n;
    }
    snprintf(out + o, out_len - o, "]");
    return n;
}

int ctcss_list_json(char *out, size_t out_len)
{
    int n = 0;
    size_t o = 0;
    if (!out || out_len < 8) {
        return 0;
    }
    o += (size_t)snprintf(out + o, out_len - o, "[");
    for (int i = 0; i < TONE_COUNT; ++i) {
        if (o + 64 >= out_len) {
            break;
        }
        o += (size_t)snprintf(out + o, out_len - o,
                              "%s{\"code\":\"%04d\",\"hz\":%.1f,"
                              "\"label\":\"%.1f Hz\"}",
                              n ? "," : "", i + 1, (double)TONES[i],
                              (double)TONES[i]);
        ++n;
    }
    snprintf(out + o, out_len - o, "]");
    return n;
}
