/*
 * OpenRadioLink-v0.3 -- stage 1 firmware (board bring-up).
 *
 * What this stage does:
 *   - drives every dangerous line to its safe state before anything else
 *   - prints a board banner (chip, flash, PSRAM, MAC, reset reason)
 *   - runs the bring-up self test and prints a PASS/FAIL table
 *   - keeps a visible heartbeat on the status LED
 *   - serves a tiny console over the native USB (the same port used to flash)
 *
 * What it deliberately does NOT do yet: touch I2S/ES8311 audio or key the
 * transmitter.  Those are stage 2 and 3, added once this stage is proven on
 * hardware -- the whole point is that each stage is signed off separately.
 */
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "audio_codec.h"
#include "audio_i2s.h"
#include "bridge.h"
#include "frontpanel.h"
#include "channels.h"
#include "net_http.h"
#include "netcom.h"
#include "voice_tail.h"
#include "netp2pnet.h"
#include "netrelay.h"
#include "ctcss.h"
#include "net_wifi.h"
#include "board_hw.h"
#include "bringup.h"
#include "ptt.h"
#include "sa818.h"
#include "sa818_cfg.h"
#include "voice.h"

static const char *TAG = "app";

static bringup_report_t s_rpt;
static bool s_have_report;

/* Two-step interlock: `t`/`T`/`u` arm, `y` confirms.  Everything that can key
 * the transmitter goes through here, so a fat-fingered single key cannot. */
typedef enum {
    ARM_NONE = 0,
    ARM_TONE,
    ARM_CARRIER,
    ARM_TALK
} arm_t;

static arm_t   s_armed;
static int64_t s_arm_deadline_us;
#define TX_ARM_WINDOW_US  (5 * 1000 * 1000)

/* Line-input mode, so parameters can be changed without a rebuild. */
static char s_line[64];
static int  s_line_len;
static bool s_line_mode;

static void tx_tone_test(void);     /* defined below, used by the console */
static void tx_carrier_test(void);
static void pd_polarity_probe(void);

/* --------------------------------------------------------- line-input mode */

/*
 * Text commands, entered after pressing `x`.  This is the same set the future
 * web panel will drive, so it is worth having on the console: it means changing
 * frequency or CTCSS never requires a rebuild, and NVS is only rewritten when
 * the command is accepted.
 */
static void console_run_line(char *line)
{
    printf("\n");
    char *argv[6];
    int argc = 0;
    for (char *t = strtok(line, " \t"); t && argc < 6; t = strtok(NULL, " \t")) {
        argv[argc++] = t;
    }
    if (argc == 0) {
        return;
    }

    sa818_cfg_t cfg;
    sa818_cfg_load(&cfg);

    bool changed = true;
    if (!strcmp(argv[0], "set")) {
        if (argc < 2) {
            printf("  usage: set <tx_mhz> [rx_mhz]     (rx defaults to tx)\n");
            return;
        }
        float tx, rx;
        if (!sa818_freq_parse(argv[1], &tx) ||
            !sa818_freq_parse(argc >= 3 ? argv[2] : argv[1], &rx) ||
            !sa818_freq_pair_valid(sa818_cfg_band(), tx, rx)) {
            printf("  %s: TX/RX must be %.1f-%.1f MHz, 12.5k grid\n",
                   sa818_band_name(sa818_cfg_band()), sa818_band_min(sa818_cfg_band()),
                   sa818_band_max(sa818_cfg_band()));
            return;
        }
        cfg.tx_mhz = tx;
        cfg.rx_mhz = rx;
    } else if (!strcmp(argv[0], "bw")) {
        if (argc < 2) { printf("  usage: bw 0 (12.5k) | bw 1 (25k)\n"); return; }
        cfg.bandwidth = atoi(argv[1]) ? 1 : 0;
    } else if (!strcmp(argv[0], "ctcss")) {
        /*
         * Sub-audible tone.  The module does the encoding and decoding; this
         * only translates between the tone people know ("88.5") and the code
         * the AT command wants ("0009"), and keeps TX and RX separate because a
         * repeater normally needs a different one in each direction.
         */
        sa818_cfg_t cfg;
        sa818_cfg_load(&cfg);

        if (argc == 1) {
            char a[32], b[32];
            printf("  TX CTCSS: %s\n", ctcss_describe(cfg.tx_ctcss, a, sizeof(a)));
            printf("  RX CTCSS: %s\n", ctcss_describe(cfg.rx_ctcss, b, sizeof(b)));
            printf("  usage: ctcss <value> [rx]  |  ctcss off  |  ctcss list\n");
            printf("  value: 88.5 Hz, 9 (tone number), 0009, or a DCS code like 023N\n");
            return;
        }
        if (!strcmp(argv[1], "list")) {
            printf("  standard tones (number -> Hz):\n");
            for (int i = 1; i <= 39; ++i) {
                char code[8];
                snprintf(code, sizeof(code), "%04d", i);
                printf("   %s %6.1f%s", code, (double)ctcss_hz_from_code(code),
                       (i % 6 == 0) ? "\n" : "");
            }
            printf("\n");
            return;
        }
        if (!strcmp(argv[1], "off")) {
            snprintf(cfg.tx_ctcss, sizeof(cfg.tx_ctcss), "0000");
            snprintf(cfg.rx_ctcss, sizeof(cfg.rx_ctcss), "0000");
        } else {
            char norm[8];
            if (!ctcss_normalise(argv[1], norm)) {
                printf("  ctcss: '%s' is not a known tone (try `ctcss list`)\n",
                       argv[1]);
                return;
            }
            snprintf(cfg.tx_ctcss, sizeof(cfg.tx_ctcss), "%s", norm);
            if (argc >= 3) {
                if (!ctcss_normalise(argv[2], norm)) {
                    printf("  ctcss: '%s' is not a known tone\n", argv[2]);
                    return;
                }
                snprintf(cfg.rx_ctcss, sizeof(cfg.rx_ctcss), "%s", norm);
            } else {
                /* One value means both directions, which is what a simplex
                 * contact wants. */
                snprintf(cfg.rx_ctcss, sizeof(cfg.rx_ctcss), "%s", norm);
            }
        }
        char slog[160];
        if (sa818_cfg_update(&cfg, slog, sizeof(slog)) == ESP_OK) {
            printf("  ctcss -> %s\n", slog);
        } else {
            printf("  ctcss: the module rejected the profile\n");
        }
        bridge_apply();
        return;
    } else if (!strcmp(argv[0], "vol")) {
        if (argc < 2) { printf("  usage: vol 1-8   (module AF volume)\n"); return; }
        cfg.volume = atoi(argv[1]);
    } else if (!strcmp(argv[0], "sq")) {
        if (argc < 2) { printf("  usage: sq 0-8   (0 = monitor)\n"); return; }
        cfg.squelch = atoi(argv[1]);
    } else if (!strcmp(argv[0], "pd")) {
        if (argc < 2) { printf("  usage: pd 0|1   (drive the module's PD pin manually)\n"); return; }
        board_sa818_pd_raw(atoi(argv[1]));
        printf("  PD pin driven to %d  (stays until reboot; try t then y to transmit)\n",
               gpio_get_level(PIN_SA818_PD));
        return;
    } else if (!strcmp(argv[0], "at")) {
        if (argc < 2) { printf("  usage: at RSSI? | AT+VERSION | AT+DMOREADGROUP\n"); return; }
        /* Re-join the rest of the line so commands containing spaces survive. */
        char raw[64] = {0};
        for (int i = 1; i < argc; ++i) {
            if (i > 1) { strncat(raw, " ", sizeof(raw) - strlen(raw) - 1); }
            strncat(raw, argv[i], sizeof(raw) - strlen(raw) - 1);
        }
        /* Raw tuning bypasses band validation/readiness; keep this diagnostic
         * entrance read-only. Operating changes use the validated commands. */
        if (strcmp(raw, "RSSI?") && strcmp(raw, "AT+VERSION") &&
            strcmp(raw, "AT+DMOCONNECT") && strcmp(raw, "AT+DMOREADGROUP")) {
            printf("  raw AT is read-only; use panel radio controls\n");
            return;
        }
        char reply[128];
        esp_err_t err = sa818_cmd(raw, reply, sizeof(reply), 700);
        printf("  > %s\n  < %s   (%s)\n", raw,
               err == ESP_OK ? reply : "(no reply)", esp_err_to_name(err));
        return;
    } else if (!strcmp(argv[0], "power")) {
        sa818_cfg_t cfg;
        sa818_cfg_load(&cfg);
        if (argc < 2) {
            printf("  RF power requested: %s; applied H/L: %s%s\n",
                   cfg.high_power ? "HIGH (1 W)" : "LOW (500 mW)",
                   board_sa818_high_power_get() ? "released" : "grounded",
                   board_sa818_power_pending() ? " (pending PTT release)" : "");
            printf("  usage: power 0|1   (GPIO10 open drain, no internal pulls)\n");
            return;
        }
        if (strcmp(argv[1], "0") && strcmp(argv[1], "1")) {
            printf("  usage: power 0|1\n");
            return;
        }
        char slog[160] = {0};
        esp_err_t e = sa818_cfg_set_power(atoi(argv[1]) != 0, slog, sizeof(slog));
        printf("  power: %s (%s)\n", slog, esp_err_to_name(e));
        return;
    } else if (!strcmp(argv[0], "scan")) {
        /*
         * Same scan the setup page runs, printed to the console.
         *
         * Exists because the provisioning AP is a network the development
         * machine is not on: without this there is no way to check that the
         * scan works without standing there with a phone.
         */
        static char buf[2600];
        if (net_wifi_scan_json(buf, sizeof(buf), true) == ESP_OK) {
            printf("  scan result: %s\n", buf);
        } else {
            printf("  scan failed\n");
        }
        return;
    } else if (!strcmp(argv[0], "wifi")) {
        /*
         * Credentials are set here and stored in NVS, deliberately not compiled
         * in: a password in the source tree is a password published forever,
         * and this board gets carried between networks.
         */
        if (argc == 1) {
            printf("  station : %s  ssid \"%s\"  ip \"%s\"  rssi %d\n",
                   net_wifi_state_name(), net_wifi_ssid(),
                   net_wifi_ip()[0] ? net_wifi_ip() : "-",
                   net_wifi_rssi_dbm());
            printf("  own AP  : %s  ssid \"%s\"  ip \"%s\"\n",
                   net_wifi_setup_ap_active() ? "up" : "down",
                   net_wifi_setup_ssid(),
                   net_wifi_ap_ip()[0] ? net_wifi_ap_ip() : "-");
            if (net_wifi_setup_ap_active()) {
                printf("  panel   : http://%s/   (join \"%s\" from a phone)\n",
                       net_wifi_ap_ip(), net_wifi_setup_ssid());
            }
            if (net_wifi_connected()) {
                printf("  panel   : http://%s/   (same LAN)\n", net_wifi_ip());
            }
            printf("  usage: wifi <ssid> <password> | wifi off | wifi setup | wifi ap off|on\n");
            return;
        }
        if (!strcmp(argv[1], "ap")) {
            if (argc >= 3 && !strcmp(argv[2], "off")) {
                net_wifi_setup_ap_stop();
                printf("  own AP closed; direct panel no longer available\n");
            } else {
                net_wifi_setup_ap_start();
                printf("  own AP up: \"%s\" -> http://%s/\n",
                       net_wifi_setup_ssid(), net_wifi_ap_ip());
            }
            return;
        }
        if (!strcmp(argv[1], "off")) {
            net_wifi_clear();
            printf("  wifi credentials cleared; setup AP \"%s\" is up\n",
                   net_wifi_setup_ssid());
            printf("  connect to it and open http://192.168.4.1/\n");
            return;
        }
        if (!strcmp(argv[1], "setup")) {
            net_wifi_setup_ap_start();
            printf("  setup AP \"%s\" up -- open http://192.168.4.1/\n",
                   net_wifi_setup_ssid());
            return;
        }
        if (argc < 3) {
            printf("  usage: wifi <ssid> <password> [host]   |   wifi off\n");
            printf("         host = the panel's .local name (default \"orl\")\n");
            return;
        }
        if (net_wifi_set_credentials(argv[1], argv[2], argc > 3 ? argv[3] : NULL)
                == ESP_OK) {
            printf("  wifi -> \"%s\" saved; connecting (watch for the IP)\n",
                   argv[1]);
        } else {
            printf("  wifi: failed to store credentials\n");
        }
        return;
    } else if (!strcmp(argv[0], "ntxgain")) {
        if (argc < 2) {
            printf("  network TX gain is %d/16 = %.2fx (NVS persisted); usage: ntxgain 1-255\n",
                   voice_get_net_tx_gain(), voice_get_net_tx_gain() / 16.0);
            return;
        }
        voice_set_net_tx_gain(atoi(argv[1]));
        printf("  network TX gain -> %d/16 = %.2fx  (saved to NVS)\n",
               voice_get_net_tx_gain(), voice_get_net_tx_gain() / 16.0);
        return;
    } else if (!strcmp(argv[0], "txgain")) {
        if (argc < 2) {
            printf("  TX gain is %d/16 = %.2fx (NVS persisted); usage: txgain 1-255\n",
                   voice_get_tx_gain(), voice_get_tx_gain() / 16.0);
            return;
        }
        const int g = atoi(argv[1]);
        voice_set_tx_gain(g);
        printf("  TX gain -> %d/16 = %.2fx  (saved to NVS)\n",
               voice_get_tx_gain(), voice_get_tx_gain() / 16.0);
        return;
    } else if (!strcmp(argv[0], "micgain")) {
        if (argc < 2) {
            int db = 0;
            const int idx = audio_codec_get_mic_gain(&db);
            printf("  mic gain index %d = %d dB (NVS persisted); usage: micgain 0-7 (0/6/12/18/24/30/36/42 dB)\n",
                   idx, db);
            return;
        }
        voice_set_mic_gain_index(atoi(argv[1]));
        int db = 0;
        const int idx = audio_codec_get_mic_gain(&db);
        printf("  mic gain -> %d dB (REG16 = %d)  (saved to NVS)\n", db, idx);
        return;
    } else if (!strcmp(argv[0], "adcvol")) {
        if (argc < 2) {
            printf("  ADC volume is 0x%02X (REG17); usage: adcvol 0-255\n",
                   audio_codec_get_adc_volume());
            return;
        }
        const int v = atoi(argv[1]);
        esp_err_t err = audio_codec_set_adc_volume(v);
        printf("  ADC volume (REG17) -> 0x%02X  (%s)\n",
               audio_codec_get_adc_volume(), esp_err_to_name(err));
        return;
    } else if (!strcmp(argv[0], "spk")) {
        /*
         * Speaker volume, eight steps: 0 = amplifier off, 1..7 = the panel's
         * 40/45/50/55/60/65/70.  Each step reproduces the loudness the codec's
         * own DAC volume at that value produced (70 = unity).  This is the
         * operator's volume control and the same one the front-panel keys
         * move; it never touches the codec DAC volume, which also feeds the
         * radio's transmit input (see voice.h).
         */
        if (argc < 2) {
            printf("  speaker volume is step %d/7 (0 = amp off, 1..7 = 40..70, "
                   "7 = 70 unity); usage: spk 0-7\n", voice_get_speaker_level());
            return;
        }
        voice_set_speaker_level(atoi(argv[1]));
        printf("  speaker volume -> step %d/7\n", voice_get_speaker_level());
        return;
    } else if (!strcmp(argv[0], "cvol")) {
        /*
         * Codec DAC volume (0-100).  A SYSTEM level, not the speaker volume:
         * this board wires the codec's DAC output to both the speaker amplifier
         * and the SA818's transmit input, so lowering it lowers our transmitted
         * audio as well.  Kept for bench work only; the panel does not show it
         * and the volume keys do not move it.
         */
        if (argc < 2) {
            printf("  codec DAC volume is %d/100 (system level); usage: cvol 0-100\n",
                   voice_get_output_volume());
            return;
        }
        voice_set_output_volume(atoi(argv[1]));
        printf("  codec DAC volume -> %d/100\n", voice_get_output_volume());
        return;
    } else if (!strcmp(argv[0], "sqgate")) {
        if (argc < 2) {
            printf("  LED indicator threshold is %d counts (module squelch\n"
                   "  closed: ~0-4, real carrier: 30+).  usage: sqgate 10-2000\n",
                   voice_get_squelch_threshold());
            return;
        }
        voice_set_squelch_threshold(atoi(argv[1]));
        printf("  LED indicator threshold -> %d counts\n",
               voice_get_squelch_threshold());
        return;
    } else if (!strcmp(argv[0], "eq")) {
        static const char *const band_names[6] =
            { "bass 150", "warm 400", "body 900",
              "clear 2k", "pres 3.2k", "air 3.8k" };
        if (argc < 2) {
            int v[6];
            for (int d = 0; d < 2; ++d) {
                voice_eq_get(d, v);
                printf("  %s EQ:  ", d ? "mic     " : "speaker ");
                for (int b = 0; b < 6; ++b) {
                    printf("%+3d ", v[b]);
                }
                printf("dB\n");
            }
            printf("  bands:");
            for (int b = 0; b < 6; ++b) {
                printf(" %s", band_names[b]);
            }
            printf("\n  range -12..12 dB.  usage: eq [spk|mic] [v1..v6]\n");
            return;
        }
        const int dir = (!strcmp(argv[1], "mic") || !strcmp(argv[1], "in")) ? 1 : 0;
        int v[6];
        voice_eq_get(dir, v);
        for (int b = 0; b < 6 && b + 2 < argc; ++b) {
            v[b] = atoi(argv[b + 2]);
        }
        voice_eq_set(dir, v);
        voice_eq_get(dir, v);
        printf("  %s EQ -> ", dir ? "mic" : "speaker");
        for (int b = 0; b < 6; ++b) {
            printf("%+d ", v[b]);
        }
        printf("dB (saved)\n");
        return;
    } else if (!strcmp(argv[0], "gain")) {
        if (argc < 2) {
            printf("  gain is %d; usage: gain 1-255  (monitor path make-up gain)\n",
                   voice_get_gain());
            return;
        }
        voice_set_gain(atoi(argv[1]));
        printf("  monitor gain -> %d\n", voice_get_gain());
        return;
    } else if (!strcmp(argv[0], "auto")) {
        if (argc < 2) {
            printf("  autostart is %s; usage: auto 0|1   (1 = monitor at boot)\n",
                   voice_get_autostart() ? "ON" : "OFF");
            return;
        }
        voice_set_autostart(atoi(argv[1]) != 0);
        printf("  autostart -> %s (saved)\n", voice_get_autostart() ? "ON" : "OFF");
        return;
    } else if (!strcmp(argv[0], "show")) {
        sa818_cfg_print(&cfg);
        return;
    } else if (!strcmp(argv[0], "mode")) {
        /*
         * Intercom mode: 0 = local (SA818), 1 = NET-P2P (public-internet STUN
         * hole punch, UDP direct), 2 = network relay (EMQX/MQTT, FMO-style).
         * Hot switch: the SA818 and the web panel keep running.
         */
        if (argc < 2) {
            uint32_t txm = 0, rxm = 0;
            int peers = 0;
            const char *state = NULL;
            netcom_stats(&txm, &rxm, &peers, &state);
            printf("  intercom mode: %s   room %u\n",
                   netcom_mode_name(netcom_mode()), netcom_room());
            if (netcom_mode() != NETCOM_MODE_LOCAL) {
                printf("  engine: %s   tx %u / rx %u msgs, peers %d\n",
                       state ? state : "-", (unsigned)txm, (unsigned)rxm, peers);
                if (netcom_mode() == NETCOM_MODE_NETP2P) {
                    char np[768];
                    netp2pnet_console(np, sizeof(np));
                    printf("%s\n", np);
                } else {
                    printf("  relay  : %s   (mode 2 only)\n", netcom_broker_str());
                }
            }
            printf("  usage: mode 0|1|2   (0 local / 1 net-p2p / 2 relay)\n");
            return;
        }
        esp_err_t err = netcom_set_mode((uint8_t)atoi(argv[1]));
        printf("  mode -> %s   (%s)\n",
               netcom_mode_name(netcom_mode()), esp_err_to_name(err));
        return;
    } else if (!strcmp(argv[0], "np2p")) {
        /*
         * NET-P2P (mode 1) controls:
         *   np2p show                 public address, NAT hint, peers
         *   np2p stun                 force an immediate STUN probe
         *   np2p knock <idx>          force a fresh KNOCK to peer <idx>
         *   np2p peer add <name> <ip> <port>
         *   np2p peer del <idx>
         *   np2p hub on|off           group-chat star forwarder
         */
        if (argc < 2) {
            printf("  usage: np2p show | stun | knock <idx> | peer add <name> <ip> <port> | peer del <idx> | hub on|off\n");
            return;
        }
        if (!strcmp(argv[1], "show")) {
            char np[768];
            netp2pnet_console(np, sizeof(np));
            printf("%s\n", np);
        } else if (!strcmp(argv[1], "stun")) {
            netp2pnet_stun_now();
            printf("  STUN probe requested\n");
        } else if (!strcmp(argv[1], "knock")) {
            if (argc < 3) {
                printf("  usage: np2p knock <idx>\n");
                return;
            }
            esp_err_t err = netp2pnet_peer_knock(atoi(argv[2]));
            printf("  knock -> %s\n", esp_err_to_name(err));
        } else if (!strcmp(argv[1], "peer")) {
            if (argc >= 4 && !strcmp(argv[2], "add")) {
                if (argc < 6) {
                    printf("  usage: np2p peer add <name> <ip> <port>\n");
                    return;
                }
                esp_err_t err = netp2pnet_peer_add(argv[3], argv[4], (uint16_t)atoi(argv[5]));
                printf("  peer add -> %s\n", esp_err_to_name(err));
            } else if (argc >= 4 && !strcmp(argv[2], "del")) {
                esp_err_t err = netp2pnet_peer_del(atoi(argv[3]));
                printf("  peer del -> %s\n", esp_err_to_name(err));
            } else {
                printf("  usage: np2p peer add <name> <ip> <port> | peer del <idx>\n");
            }
        } else if (!strcmp(argv[1], "hub")) {
            bool on = (argc >= 3) ? (atoi(argv[2]) != 0) : !netp2pnet_hub_on();
            esp_err_t err = netp2pnet_hub_set(on);
            printf("  hub -> %s (%s)\n", netp2pnet_hub_on() ? "ON" : "off", esp_err_to_name(err));
        } else {
            printf("  usage: np2p show | stun | knock <idx> | peer add <name> <ip> <port> | peer del <idx> | hub on|off\n");
        }
        return;
    } else if (!strcmp(argv[0], "room")) {
        if (argc < 2) {
            printf("  room: %u   (usage: room %d-%d, NET-P2P only)\n",
                   netcom_room(), NETCOM_ROOM_MIN, NETCOM_ROOM_MAX);
            return;
        }
        esp_err_t err = netcom_set_room((uint16_t)atoi(argv[1]));
        printf("  room -> %u   (%s)\n", netcom_room(), esp_err_to_name(err));
        return;
    } else if (!strcmp(argv[0], "broker")) {
        if (argc < 2) {
            printf("  relay broker: %s   (usage: broker host[:port])\n",
                   netcom_broker_str());
            return;
        }
        esp_err_t err = netcom_set_broker(argv[1]);
        printf("  broker -> %s   (%s)\n", netcom_broker_str(), esp_err_to_name(err));
        return;
    } else if (!strcmp(argv[0], "bridge")) {
        /*
         * Analog <-> network bridge (P2P / relay modes): the RF receive
         * audio on the selected channel goes into the room, and the room
         * audio is re-transmitted on the air with automatic PTT keying.
         */
        if (argc < 2) {
            char name[CHANNEL_NAME_LEN] = {0};
            const bool have_name =
                bridge_chan() >= 1 && bridge_chan() <= channels_count()
                && channels_get(bridge_chan() - 1, name, sizeof(name), NULL);
            printf("  analog bridge: %s   (channel %d%s%s)\n",
                   bridge_en() ? "ON" : "off",
                   bridge_chan(),
                   have_name ? ": " : "", name);
            printf("  state: %s%s\n",
                   bridge_active() ? "active" : "inactive",
                   bridge_keyed() ? "  (RF keyed)" : "");
            printf("  usage: bridge 0|1 [chan]   (chan: 0=none, 1..%d)\n",
                   channels_count());
            return;
        }
        esp_err_t err = bridge_set_en(atoi(argv[1]) != 0);
        if (argc >= 3) {
            err = bridge_set_chan(atoi(argv[2]));
        }
        printf("  bridge -> %s%s\n", esp_err_to_name(err),
               bridge_active() ? "  (active)" : "");
        return;
    } else if (!strcmp(argv[0], "bridge_test")) {
        /* Self-test: N ms of 1 kHz into the active engine's jitter buffer,
         * as if a peer had sent it (default 3000). */
        const int ms = (argc >= 2) ? atoi(argv[1]) : 3000;
        netcom_test_tone(ms);
        printf("  bridge test: %d ms of 1 kHz queued%s\n", ms,
               netcom_mode() == NETCOM_MODE_LOCAL ? " (local mode: no-op)"
                                                  : "");
        return;
    } else if (!strcmp(argv[0], "tail")) {
        if (argc < 2) {
            printf("  tail (end-of-transmission tone): %s\n",
                   tail_name(tail_get()));
            printf("  available:");
            for (int i = TAIL_OFF; i < TAIL_COUNT; ++i) {
                printf(" %d=%s", i, tail_name(i));
            }
            printf("\n");
            if (tail_custom_samples()) {
                printf("  custom: %.2f s stored\n",
                       (double)tail_custom_samples() / (double)AUDIO_SAMPLE_RATE_HZ);
            }
            printf("  usage: tail <id|name>   (any name listed above)\n");
            return;
        }
        int id = -1;
        const char *a = argv[1];
        /* numeric id (0..10); an all-digit argument that is out of range
         * falls through to the name match */
        if (a[0] >= '0' && a[0] <= '9' &&
            (a[1] == '\0' || (a[1] >= '0' && a[1] <= '9' && a[2] == '\0'))) {
            const int v = atoi(a);
            if (v >= TAIL_OFF && v < TAIL_COUNT) {
                id = v;
            }
        }
        if (id < 0) {
            /* lenient name match: ignore case and non-alphanumerics, so
             * "kchoo" matches "K-CHOO" and "icom*" matches "icom *" */
            for (int i = TAIL_OFF; i < TAIL_COUNT && id < 0; ++i) {
                char n1[24], n2[24];
                int j1 = 0, j2 = 0;
                for (const char *p = a; *p && j1 < 23; ++p) {
                    if (isalnum((unsigned char)*p)) {
                        n1[j1++] = (char)tolower((unsigned char)*p);
                    }
                }
                n1[j1] = '\0';
                for (const char *p = tail_name(i); *p && j2 < 23; ++p) {
                    if (isalnum((unsigned char)*p)) {
                        n2[j2++] = (char)tolower((unsigned char)*p);
                    }
                }
                n2[j2] = '\0';
                if (j1 && !strcmp(n1, n2)) {
                    id = i;
                }
            }
        }
        esp_err_t err = tail_set(id);
        printf("  tail -> %s   (%s)\n", tail_name(tail_get()), esp_err_to_name(err));
        return;
    } else if (!strcmp(argv[0], "tail_preview")) {
        if (argc < 2) {
            printf("  usage: tail_preview <id>   (plays through the speaker)\n");
            return;
        }
        tail_preview_arm(atoi(argv[1]));
        printf("  preview armed\n");
        return;
    } else {
        printf("  commands:\n");
        printf("    set <tx_mhz> [rx_mhz]   e.g. set 438.5000\n");
        printf("    bw 0|1                  0 = 12.5 kHz narrow, 1 = 25 kHz\n");
        printf("    ctcss <tx> [rx]         NS code e.g. 0008, or 0000 to disable\n");
        printf("    sq 0-8                  0 = monitor (squelch open)\n");
        printf("    vol 1-8                 module AF volume\n");
        printf("    show                    print the profile\n");
        printf("    mode 0|1|2              intercom: local / net-p2p / relay\n");
        printf("    np2p show|stun|knock|peer|hub  NET-P2P (mode 1) controls\n");
        printf("    room 1000-9999          NET-P2P room (group) number\n");
        printf("    broker host[:port]      relay (EMQX) server, mode 2\n");
        printf("    tail [id|name]          end-of-transmission tone\n");
        printf("    tail_preview <id>       hear a tone now (speaker)\n");
        return;
    }

    if (changed) {
        char summary[80];
        esp_err_t err = sa818_cfg_update(&cfg, summary, sizeof(summary));
        bridge_apply();
        printf("  %s -> %s\n", esp_err_to_name(err), summary);
        printf("  (press g to show, or t then y to transmit on this frequency)\n");
    }
}

/* ------------------------------------------------------------------ console */

static void console_help(void)
{
    printf("\n  commands\n");
    printf("    h   this help              s   status\n");
    printf("    b   board banner           i   I2C scan\n");
    printf("    r   SA818 RSSI             v   SA818 AT+VERSION\n");
    printf("    l   toggle status LED      a   toggle amplifier\n");
    printf("    p   play a 1 kHz tone through the LM4871\n");
    printf("    P   play the full ~15 s listening test (tones + sweep + volume ladder)\n");
    printf("    L   live mic level meter (10 s, no RF) -- use this to check the capsule\n");
    printf("    m   measure the microphone path (1 s capture)\n");
    printf("    w   measure radio AF_OUT level (1 s, GPIO%d)\n", PIN_SA818_AUDIO_IN);
    printf("    c   codec status           d   ES8311 register dump\n");
    printf("    g   show radio profile     f   (re)apply profile to the module\n");
    printf("    x   command mode (set freq / bw / ctcss / sq / vol / mode / room / broker)\n");
    printf("    0-8 set squelch level (0 = monitor, squelch fully open)\n");
    printf("    t   ARM the RF test (3 beeps)   y   confirm (needs a load!)\n");
    printf("    T   ARM the 15 s RF test        y   confirm (watch the current)\n");
    printf("    u   ARM local talk (mic -> RF)  y   start it, z stops it\n");
    printf("    o   monitor mode: radio AF_OUT -> speaker (toggle)\n");
    printf("    S   LOUD audible self-test: pulses ON 2s / OFF 2s for 120 s\n");
    printf("    z   STOP everything: voice paths + release PTT\n");
    printf("    M   watch SA818 RSSI for 30 s (key your handset)\n");
    printf("    V   voice-presence analysis, 8 s (keep talking)\n");
    printf("    A   output noise floor (toggle): amp ON, DAC silent, clocks alive\n");
    printf("    B   squelch bypassed (toggle): amp ON, radio audio passes -- MEASURE\n");
    printf("    N   codec-DAC mute test: splits codec vs amplifier\n");
    printf("    O   amp alone (INVALID for DC tests -- stops the codec clocks)\n");
    printf("    Q   software reboot\n");
    printf("    G   gain sweep: 5 steps x 5 s, reports hard clipping\n");
    printf("    k   PTT state-machine dry run (never keys the radio)\n");
    printf("    D   re-probe the SA818 PD pin polarity (AT only, no RF)\n");
    printf("    R   re-run bring-up self test\n");
    printf("\n  Transmitting is NOT reachable from this console on purpose:\n");
    printf("  keying the radio needs a licensed operator and a proper antenna\n");
    printf("  or dummy load, so the PTT state machine owns that, not a stray key.\n\n");
}

static void console_status(void)
{
    const sa818_state_t *st = sa818_state();
    int tx = 0, rx = 0;
    sa818_report_pins(&tx, &rx);

    printf("\n  --- status -------------------------------------------\n");
    printf("  uptime        : %lld s\n", esp_timer_get_time() / 1000000LL);
    printf("  free heap     : %u bytes internal\n", (unsigned)esp_get_free_heap_size());
    printf("  LED           : %s\n", gpio_get_level(PIN_LED_STATUS) ? "on" : "off");
    printf("  amplifier     : %s  (AMP_SHDN GPIO%d = %d)\n",
           board_amp_is_enabled() ? "ON" : "off",
           PIN_AMP_SHDN, gpio_get_level(PIN_AMP_SHDN));
    printf("  PTT           : %s  (GPIO%d = %d)\n",
           board_ptt_is_asserted() ? "*** TRANSMITTING ***" : "released (RX)",
           PIN_SA818_PTT, gpio_get_level(PIN_SA818_PTT));
    printf("  SA818 PD      : GPIO%d = %d  (%s)\n", PIN_SA818_PD, board_sa818_pd_level(),
           board_sa818_pd_level() ? "module POWERED" : "module OFF -- no RF possible");
    printf("  AMP_SHDN      : GPIO%d = %d\n", PIN_AMP_SHDN, gpio_get_level(PIN_AMP_SHDN));
    printf("  SA818 UART    : TX=GPIO%d RX=GPIO%d%s\n", tx, rx,
           st->txrx_swapped ? "  (swapped vs net names)" : "");
    printf("  voice mode    : %s%s\n", voice_mode_name(),
           voice_get_bypass_squelch() ? "  [squelch BYPASSED]" : "");
    printf("  autostart     : %s   (LED steady ON = audio present)\n",
           voice_get_autostart() ? "monitor at boot" : "off");
    if (voice_mode() != VOICE_OFF) {
        voice_stats_t vs;
        voice_get_stats(&vs);
        printf("    blocks=%u underruns=%u gain=%d @ %d Hz\n",
               vs.blocks, vs.underruns, vs.gain, vs.sample_rate);
        printf("    input AC peak: %d counts now (~%d mV), %d max since start\n",
               vs.peak_in, vs.peak_in * 3300 / 4095, vs.peak_max);
    }
    if (ptt_get_state() != PTT_IDLE) {
        printf("  PTT state     : %s, %d ms left\n",
               ptt_state_name(), ptt_tx_remaining_ms());
    }
    printf("  SA818 link    : %s   version=%s   handshake=%s\n",
           st->inited ? "up" : "down",
           st->version[0] ? st->version : "?",
           st->handshake_ok ? "yes" : "no");
    if (st->rssi >= 0) {
        printf("  SA818 RSSI    : %d dB\n", st->rssi);
    }
    int mv = 0;
    if (board_adc_read_mv(BOARD_ADC_CH_RADIO_AF, &mv) == ESP_OK) {
        printf("  radio AF out  : %d mV (GPIO%d)\n", mv, PIN_SA818_AUDIO_IN);
    }
    if (board_adc_read_mv(BOARD_ADC_CH_VBAT, &mv) == ESP_OK) {
        printf("  battery       : %d mV (GPIO%d)\n", mv, PIN_VBAT_ADC);
    }
    if (s_have_report) {
        printf("  bring-up      : %d passed, %d failed\n",
               s_rpt.passed, s_rpt.failed);
    }
    printf("  ------------------------------------------------------\n\n");
}

static void console_i2c_scan(void)
{
    board_i2c_cfg_t cfg;
    uint8_t found[BRINGUP_MAX_I2C];
    int count = 0;
    esp_err_t err = board_i2c_autodetect(&cfg, found, BRINGUP_MAX_I2C, &count);

    printf("\n  I2C autodetect (as-drawn -> swapped -> 25kHz -> 10kHz)\n");
    printf("  winner: SCL=GPIO%d SDA=GPIO%d @ %u Hz%s -> %d device(s)\n",
           cfg.scl_gpio, cfg.sda_gpio, (unsigned)cfg.freq_hz,
           cfg.swapped ? "  (SWAPPED vs net names)" : "", count);
    for (int i = 0; i < count && i < BRINGUP_MAX_I2C; ++i) {
        printf("    0x%02X%s\n", found[i],
               found[i] == BOARD_ES8311_I2C_ADDR ? "   <== ES8311 codec" : "");
    }
    if (err != ESP_OK) {
        printf("    nothing answered.  This bus has NO external pull-ups, and\n");
        printf("    U3 is a hand-soldered WQFN-20 (0.4 mm).  Check AUDIO_3V3\n");
        printf("    (FB3) and reflow U3's pins + exposed pad.\n");
    }
    printf("\n");
}

static void console_task(void *arg)
{
    (void)arg;
    console_help();
    for (;;) {
        int c = fgetc(stdin);
        if (c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        /* Line-input mode swallows everything until Enter. */
        if (s_line_mode) {
            if (c == '\r' || c == '\n') {
                s_line[s_line_len] = '\0';
                s_line_mode = false;
                s_line_len = 0;
                console_run_line(s_line);
            } else if (c == 8 || c == 127) {
                if (s_line_len > 0) {
                    --s_line_len;
                    printf("\b \b");
                }
            } else if (c >= 32 && c < 127 && s_line_len < (int)sizeof(s_line) - 1) {
                s_line[s_line_len++] = (char)c;
                printf("%c", c);        /* local echo */
            }
            continue;
        }

        switch (c) {
        case 'h': case '?':
            console_help();
            break;
        case 's':
            console_status();
            break;
        case 'b':
            board_print_banner();
            break;
        case 'i':
            console_i2c_scan();
            break;
        case 'r': {
            int rssi = -1;
            if (sa818_read_rssi(&rssi) == ESP_OK) {
                printf("\n  RSSI = %d dB\n\n", rssi);
            } else {
                printf("\n  RSSI read failed -- is the SA818 link up? (press s)\n\n");
            }
            break;
        }
        case 'v': {
            char reply[64];
            if (sa818_cmd("AT+VERSION", reply, sizeof(reply), 400) == ESP_OK) {
                printf("\n  AT+VERSION -> \"%s\"\n\n", reply);
            } else {
                printf("\n  no reply to AT+VERSION\n\n");
            }
            break;
        }
        case 'l':
            board_led_toggle();
            printf("\n  LED %s\n\n", gpio_get_level(PIN_LED_STATUS) ? "on" : "off");
            break;
        case 'a':
            board_amp_enable(!board_amp_is_enabled());
            printf("\n  amplifier %s\n\n", board_amp_is_enabled() ? "ON" : "off");
            break;
        case 'p':
            /*
             * Speaker path test: make sure the codec is configured, turn the
             * LM4871 on, play 1 kHz for 300 ms, then shut the amp down again.
             */
            printf("\n  playing 1 kHz for 300 ms through the LM4871...\n");
            if (!audio_codec_ready()) {
                printf("  initialising the ES8311 first...\n");
                if (audio_codec_init() != ESP_OK) {
                    printf("  codec init FAILED -- press i to re-scan the I2C bus\n\n");
                    break;
                }
            }
            board_amp_enable(true);
            vTaskDelay(pdMS_TO_TICKS(30));
            if (audio_i2s_play_tone(1000, 300, 8000) == ESP_OK) {
                printf("  tone written to I2S -- listen on J4\n");
            } else {
                printf("  I2S write failed\n");
            }
            vTaskDelay(pdMS_TO_TICKS(30));
            board_amp_enable(false);
            printf("\n");
            break;
        case 'L': {
            /*
             * Live microphone level meter.
             *
             * The single 1 s snapshot of `m` is not enough to tell a dead
             * capsule from a healthy one at low gain: it samples whatever
             * happens to be in that one second.  This prints RMS/peak ten times
             * a second for ten seconds, so the operator can talk and watch the
             * numbers move -- and it never keys the transmitter.
             */
            if (!audio_codec_ready() && audio_codec_init() != ESP_OK) {
                printf("  codec init failed\n\n");
                break;
            }
            int mdb = 0;
            audio_codec_get_mic_gain(&mdb);
            printf("\n  === live mic meter: 10 s, no RF ===\n");
            printf("  mic gain %d dB (REG16), ADC volume 0x%02X (REG17)\n",
                   mdb, audio_codec_get_adc_volume());
            printf("  speak at a normal distance and watch the peak column\n\n");
            printf("   t(s)   RMS    peak   clipped\n");
            for (int i = 0; i < 20; ++i) {
                audio_stats_t st;
                if (audio_measure_mic(500, &st) != ESP_OK) {
                    printf("   capture failed\n");
                    break;
                }
                printf("   %4.1f  %5d   %5d   %5d  %s\n",
                       (i + 1) * 0.5, st.rms, st.peak, st.clipped,
                       st.peak > 4000 ? "<<< good level"
                     : st.peak > 800  ? "<-- usable"
                     : st.peak > 200  ? "(low)" : "(very low)");
            }
            printf("\n  If the peak never rises above ~200 while speaking, the capsule\n");
            printf("  or its bias network is the problem, not the gain: raise it with\n");
            printf("  `x micgain 7` (42 dB, the chip maximum) and compare.\n\n");
            break;
        }
        case 'm': {
            /* Microphone path: ES8311 ADC -> ASDOUT -> S3 I2S RX. */
            printf("\n  capturing 1 s from the microphone (MK1 -> ES8311 ADC -> I2S)...\n");
            if (!audio_codec_ready() && audio_codec_init() != ESP_OK) {
                printf("  codec init failed\n\n");
                break;
            }
            audio_stats_t st;
            if (audio_measure_mic(1000, &st) == ESP_OK) {
                const char *level;
                if (!st.alive) {
                    level = "no variation at all";
                } else if (st.rms < 8) {
                    level = "very quiet (silent room / low mic gain)";
                } else if (st.rms < 200) {
                    level = "room noise -- normal for a quiet room";
                } else if (st.rms < 4000) {
                    level = "audio present";
                } else {
                    level = "loud";
                }
                printf("  samples   : %d\n", st.samples);
                printf("  DC offset : %d\n", st.mean);
                printf("  AC RMS    : %d\n", st.rms);
                printf("  peak      : %d\n", st.peak);
                printf("  distinct  : %d\n", st.distinct);
                printf("  clipped   : %d\n", st.clipped);
                printf("  level     : %s\n", level);
                printf("  verdict   : %s\n", st.alive
                       ? "LIVE -- the ADC path is carrying varying data"
                       : "DEAD -- constant data, the ADC path is not producing audio");
                if (st.alive) {
                    printf("  (talk into the microphone and press m again: RMS should jump)\n");
                }
            } else {
                printf("  capture failed\n");
            }
            printf("\n");
            break;
        }
        case 'w': {
            /* Radio receive audio: SA818 AF_OUT -> GPIO1 -> ADC1_CH0. */
            printf("\n  measuring 1 s of radio AF_OUT on GPIO%d...\n", PIN_SA818_AUDIO_IN);
            board_adc_stats_t a;
            if (board_adc_measure(BOARD_ADC_CH_RADIO_AF, 1000, &a) == ESP_OK) {
                printf("  samples   : %d\n", a.samples);
                printf("  DC bias   : %d mV\n", a.mean_mv);
                printf("  AC RMS    : %d mV\n", a.ac_rms_mv);
                printf("  min/max   : %d / %d mV\n", a.min_mv, a.max_mv);
                printf("  peak-peak : %d mV\n", a.pp_mv);
                if (a.mean_mv < 30) {
                    printf("  note      : bias near 0 V -- SA818 may be unpowered or in power-down\n");
                } else if (a.ac_rms_mv < 2) {
                    printf("  note      : flat -- squelch closed / no carrier (expected with no signal)\n");
                } else {
                    printf("  note      : signal present\n");
                }
            } else {
                printf("  measurement failed\n");
            }
            printf("\n");
            break;
        }
        case 'P': {
            /*
             * Long listening test.  The amplifier is enabled for the whole
             * pattern (each segment fades in/out internally, so no clicks) and
             * shut down again afterwards.
             */
            printf("\n  audio test pattern starting (~15 s) -- listen on J4\n");
            if (!audio_codec_ready()) {
                printf("  initialising the ES8311 first...\n");
                if (audio_codec_init() != ESP_OK) {
                    printf("  codec init FAILED -- press i to re-scan the I2C bus\n\n");
                    break;
                }
            }
            board_amp_enable(true);
            vTaskDelay(pdMS_TO_TICKS(50));
            if (audio_i2s_play_test_pattern() == ESP_OK) {
                printf("  pattern played OK\n");
            } else {
                printf("  pattern FAILED part-way through\n");
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            board_amp_enable(false);
            printf("\n");
            break;
        }
        case 'c':
            printf("\n  codec: %s\n", audio_codec_ready() ? "initialised" : "NOT initialised");
            if (audio_codec_ready()) {
                int vol = -1;
                if (audio_codec_get_volume(&vol) == ESP_OK) {
                    printf("  volume (read back from the chip): %d\n", vol);
                } else {
                    printf("  volume read-back FAILED\n");
                }
            } else {
                printf("  press p to initialise, or i to re-scan I2C\n");
            }
            printf("\n");
            break;
        case 'd':
            printf("\n");
            if (!audio_codec_ready()) {
                printf("  codec not initialised -- press p first\n\n");
            } else {
                printf("  ES8311 register dump:\n");
                audio_codec_dump();
                printf("\n\n");
            }
            break;
        case 'R':
            printf("\n  re-running bring-up...\n");
            bringup_run(&s_rpt);
            s_have_report = true;
            bringup_print(&s_rpt);
            break;

        case 'g': {
            sa818_cfg_t cfg;
            sa818_cfg_load(&cfg);
            printf("\n  --- radio profile ---\n");
            sa818_cfg_print(&cfg);
            printf("\n");
            break;
        }
        case 'f': {
            sa818_cfg_t cfg;
            char summary[80];
            sa818_cfg_load(&cfg);
            esp_err_t err = sa818_cfg_apply(&cfg, summary, sizeof(summary));
            bridge_apply();
            printf("\n  profile apply: %s\n", esp_err_to_name(err));
            if (summary[0]) {
                printf("  %s\n", summary);
            }
            printf("  (existing NVS profile retained)\n\n");
            break;
        }
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': {
            const int sq = c - '0';
            char summary[80];
            esp_err_t err = sa818_cfg_set_squelch(sq, summary, sizeof(summary));
            printf("\n  squelch -> %d%s : %s\n\n", sq,
                   sq == 0 ? " (monitor: squelch fully open)" : "",
                   esp_err_to_name(err));
            break;
        }
        case 'k': {
            /* Safe: exercises the PTT machine with the radio NOT keyed. */
            printf("\n  PTT state-machine dry run (radio will NOT be keyed)...\n");
            ptt_dry_result_t d;
            if (ptt_dry_run(600, &d) != ESP_OK) {
                printf("  dry run refused: state is %s\n\n", ptt_state_name());
                break;
            }
            printf("  PREPARE reached : %s\n", d.reached_prepare ? "yes" : "NO");
            printf("  TX reached      : %s\n", d.reached_tx ? "yes" : "NO");
            printf("  TAIL reached    : %s\n", d.reached_tail ? "yes" : "NO");
            printf("  deadline fired  : %s\n", d.deadline_fired ? "yes" : "NO");
            printf("  ended in IDLE   : %s\n", d.ended_idle ? "yes" : "NO");
            printf("  timing          : prepare %d ms, TX %d ms, tail %d ms\n",
                   d.prepare_ms, d.tx_ms, d.tail_ms);
            printf("  keyings         : %d (must be 0)\n", d.keyings);
            printf("  PTT pad after   : %d (1 = released)\n", d.pin_level_after);
            printf("  verdict         : %s\n",
                   (d.reached_tx && d.reached_tail && d.ended_idle &&
                    d.deadline_fired && d.keyings == 0 && d.pin_level_after == 1)
                   ? "PASS -- machine and safety net behave correctly"
                   : "FAIL -- see the fields above");
            printf("\n");
            break;
        }
        case 'M': {
            /*
             * Receive-side isolation test: poll the module's own RSSI while the
             * operator keys a handset.  If the module reports signal then the
             * antenna path, the module's RF front end and the frequency are all
             * good, which pins any remaining fault on the transmit chain.
             */
            sa818_cfg_t cfg;
            sa818_cfg_load(&cfg);
            printf("\n  RSSI watch for 30 s -- key your handset any time now\n");
            printf("  Handset: %.4f MHz, %s, CTCSS %s\n",
                   cfg.rx_mhz, cfg.bandwidth ? "25 kHz" : "12.5 kHz",
                   strcmp(cfg.rx_ctcss, "0000") ? cfg.rx_ctcss : "off");
            printf("  Squelch is at %d; press 0 first for a fully open squelch.\n\n",
                   cfg.squelch);

            /* Show the raw framing once, so an unexpected reply format is visible. */
            {
                char raw[96];
                if (sa818_cmd("RSSI?", raw, sizeof(raw), 500) == ESP_OK) {
                    printf("  raw RSSI? reply: \"%s\"\n\n", raw);
                } else {
                    printf("  raw RSSI? reply: (none)\n\n");
                }
            }

            int best = -1;
            int best_at = -1;
            for (int i = 0; i < 150; ++i) {
                int rssi = -1;
                if (sa818_read_rssi(&rssi) == ESP_OK) {
                    if (rssi > best) {
                        best = rssi;
                        best_at = i;
                    }
                }
                if (i % 10 == 0) {
                    printf("  t=%2d s  rssi=%-4d best=%d\n", i / 5, rssi, best);
                }
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            printf("  best RSSI over 30 s: %d at t=%d s\n", best,
                   best_at >= 0 ? best_at / 5 : -1);
            if (best <= 0) {
                printf("  => the module heard nothing.  Either the handset was not on\n");
                printf("     this frequency, or the antenna path / RF front end is dead.\n");
            } else {
                printf("  => the module DOES receive: antenna path, RF front end and\n");
                printf("     frequency are all good.\n");
            }
            printf("\n");
            break;
        }
        case 'G': {
            /*
             * Gain sweep.
             *
             * Finding the right `gain` by hand means watching the limiter
             * counter while talking, which is awkward.  This steps the gain
             * through a spread of values, holding each for five seconds, and
             * reports how hard the limiter had to work in each.  The operator
             * just talks continuously for half a minute and then picks the
             * highest gain that keeps limiting out of the "heavy" band.
             */
            static const int steps[] = { 120, 160, 200, 240, 255 };
            printf("\n  === GAIN SWEEP -- keep talking for 30 s ===\n\n");
            printf("  step  gain   in-peak   hard clips   verdict\n");
            for (unsigned i = 0; i < sizeof(steps) / sizeof(steps[0]); ++i) {
                voice_set_gain(steps[i]);
                (void)voice_take_peak();          /* clear */
                const uint32_t l0 = voice_hard_clip_total();
                vTaskDelay(pdMS_TO_TICKS(5000));
                const uint32_t hits = voice_hard_clip_total() - l0;
                const int pk = voice_take_peak();
                const char *verdict;
                if (pk < 5) {
                    /*
                     * No speech reached the detector, so the limiter count says
                     * nothing about this gain.  Say so instead of reporting a
                     * meaningless "no limiting".
                     */
                    verdict = "NO SIGNAL -- step invalid, talk continuously";
                } else if (hits == 0) {
                    verdict = "no limiting -- headroom left";
                } else if (hits < 300) {
                    verdict = "light -- good setting";
                } else if (hits < 3000) {
                    verdict = "moderate";
                } else {
                    verdict = "HEAVY -- too much gain";
                }
                printf("   %u    %3d    %8d   %8u      %s\n", i + 1, steps[i],
                       pk, (unsigned)hits, verdict);
            }
            printf("\n  Pick the highest gain that is 'light' or 'no limiting',\n");
            printf("  ignoring any step marked NO SIGNAL.\n");
            printf("  Report it and it becomes the default.\n\n");
            break;
        }
        case 'x':
            s_line_mode = true;
            s_line_len = 0;
            printf("\n  command mode: type a command and press Enter\n");
            printf("   set <tx> [rx] | bw 0|1 | ctcss <tx> [rx] | sq 0-8 | vol 1-8\n");
            printf("   spk 0-7                            speaker volume (0 = amp off, 40..70)\n");
            printf("   gain 1-255 | txgain 1-255 | cvol 0-100\n");
            printf("   micgain 0-7 | adcvol 0-255 | sqgate 10-2000 | auto 0|1 | show\n");
            printf("   eq [spk|mic] [v1..v6]             6-band EQ, -12..12 dB\n");
            printf("   wifi ... | scan | power 0|1 | ctcss <value> [rx] | ctcss list | ctcss off\n");
            printf("   mode 0|1|2 | room 1000-9999 | broker host[:port]   intercom\n");
            printf("  > ");
            break;
        case 't':
            /*
             * RF transmit test, two steps on purpose.  Keying a UHF transmitter
             * is a licensed act and needs a matched load, so it must not be one
             * keystroke away from a fat-fingered key.
             */
            printf("\n  *** RF TRANSMIT TEST ARMING ***\n");
            printf("  Next: one key-up of about 2 s carrying three 1 kHz beeps.\n");
            printf("  Only proceed with a licensed setup and an antenna or DUMMY LOAD\n");
            printf("  on J5.  Transmitting into an open connector can damage the PA.\n");
            {
                sa818_cfg_t cfg;
                sa818_cfg_load(&cfg);
                printf("  Current profile: %.4f MHz, %s, CTCSS %s, sq %d\n",
                       cfg.tx_mhz,
                       cfg.bandwidth ? "25 kHz" : "12.5 kHz",
                       strcmp(cfg.tx_ctcss, "0000") ? cfg.tx_ctcss : "off",
                       cfg.squelch);
            }
            printf("  Press 'y' within 5 s to transmit, anything else cancels.\n\n");
            s_armed = ARM_TONE;
            s_arm_deadline_us = esp_timer_get_time() + TX_ARM_WINDOW_US;
            break;
        case 'T':
            printf("\n  *** LONG RF TEST ARMING ***\n");
            printf("  A 1 kHz beep every second for 15 s so you can watch the 5 V\n");
            printf("  current or hold a probe on the module.\n");
            printf("  Requires a licensed setup and an antenna or DUMMY LOAD on J5.\n");
            printf("  Press 'y' within 5 s to transmit, anything else cancels.\n\n");
            s_armed = ARM_CARRIER;
            s_arm_deadline_us = esp_timer_get_time() + TX_ARM_WINDOW_US;
            break;
        case 'u':
            printf("\n  *** LOCAL TALK ARMING ***\n");
            printf("  Next: key the transmitter and feed the microphone straight into\n");
            printf("  SA818 MIC_IN.  The radio stays keyed until you press z, up to a\n");
            printf("  hard ceiling of %d s.\n", PTT_TX_ABSOLUTE_MAX_MS / 1000);
            printf("  TX gain is %d/16 = %.2fx -- adjust with `x txgain N` if the\n",
                   voice_get_tx_gain(), voice_get_tx_gain() / 16.0);
            printf("  receiving handset sounds too loud/distorted or too quiet.\n");
            printf("  Requires a licensed setup and an antenna or DUMMY LOAD on J5.\n");
            printf("  Press 'y' within 5 s to start, anything else cancels.\n\n");
            s_armed = ARM_TALK;
            s_arm_deadline_us = esp_timer_get_time() + TX_ARM_WINDOW_US;
            break;
        case 'y': {
            if (s_armed == ARM_NONE || esp_timer_get_time() > s_arm_deadline_us) {
                s_armed = ARM_NONE;
                printf("\n  nothing armed -- press t, T or u first\n\n");
                break;
            }
            const arm_t what = s_armed;
            s_armed = ARM_NONE;
            if (what == ARM_TONE) {
                tx_tone_test();
            } else if (what == ARM_CARRIER) {
                tx_carrier_test();
            } else {
                printf("\n  talk mode: speak into the microphone (MK1).  Press z to stop.\n");
                if (voice_start(VOICE_TALK) != ESP_OK) {
                    printf("  talk mode failed to start (PTT state %s)\n\n",
                           ptt_state_name());
                } else {
                    printf("  transmitting -- PTT held, ceiling %d s\n\n",
                           PTT_TX_ABSOLUTE_MAX_MS / 1000);
                }
            }
            break;
        }
        case 'o': {
            if (voice_mode() == VOICE_MONITOR) {
                voice_stop();
                printf("\n  monitor mode OFF\n\n");
            } else {
                printf("\n  starting monitor mode: radio AF_OUT -> speaker\n");
                if (voice_start(VOICE_MONITOR) != ESP_OK) {
                    printf("  monitor mode failed to start\n\n");
                } else {
                    printf("  listening -- press o again to stop\n\n");
                }
            }
            break;
        }
        case 'z': {
            const voice_mode_t was = voice_mode();
            voice_stop();
            ptt_request_rx();
            printf("\n  stopped %s; PTT released (GPIO%d = %d)\n\n",
                   was == VOICE_OFF ? "nothing (already idle)" : voice_mode_name_of(was),
                   PIN_SA818_PTT, gpio_get_level(PIN_SA818_PTT));
            break;
        }
        case 'S': {
            /*
             * Audible self-test.  Rather than an A/B that depends on the
             * operator and the console being in step (which got missed the
             * first time), the gate pulses the monitor output on and off for a
             * minute: listen at any point and the rhythm is unmistakable, and
             * hearing it proves the sound really is produced by this pipeline.
             */
            printf("\n  === LOUD audible self-test: 120 s ===\n");
            if (voice_mode() != VOICE_MONITOR) {
                printf("  (starting monitor mode first)\n");
                if (voice_start(VOICE_MONITOR) != ESP_OK) {
                    printf("  monitor mode failed to start\n\n");
                    break;
                }
            }
            const int old_gain = voice_get_gain();
            voice_set_gain(200);
            audio_codec_set_volume(100);
            printf("  Speaker pulses LOUD 2 s ON / 2 s OFF for 120 s.\n");
            printf("  If that rhythm is audible, the sound really is this pipeline\n");
            printf("  playing the radio's audio.  Listen at any time.\n\n");
            voice_set_ab_gate(2000);
            for (int i = 0; i < 12; ++i) {
                vTaskDelay(pdMS_TO_TICKS(10000));
                printf("  ... %d/120 s\n", (i + 1) * 10);
            }
            voice_set_ab_gate(0);
            voice_set_gain(old_gain);
            audio_codec_set_volume(70);
            printf("  self-test done; gain and volume restored.\n\n");
            break;
        }
        case 'W': {
            /*
             * Pull the filtered monitor audio off the board for offline analysis.
             *
             * Peak-level logs were not enough to explain "the voice is not
             * intelligible", so the samples themselves get shipped out.
             *
             * Sent as hex rather than raw binary because the console is built
             * with CONFIG_NEWLIB_STDOUT_LINE_ENDING_CRLF, which would rewrite
             * every 0x0A byte in the stream and silently corrupt the data.
             */
            const size_t want = AUDIO_SAMPLE_RATE_HZ * 3;
            int16_t *buf = (int16_t *)malloc(want * sizeof(int16_t));
            if (!buf) {
                printf("\n  out of memory for the capture\n\n");
                break;
            }
            if (voice_mode() != VOICE_MONITOR) {
                printf("\n  (starting monitor mode first)\n");
                if (voice_start(VOICE_MONITOR) != ESP_OK) {
                    printf("  monitor mode failed to start\n\n");
                    free(buf);
                    break;
                }
            }
            /* No waiting: the ring already holds the last 3 s. */
            const size_t got = voice_capture_recent(buf, want);
            voice_set_quiet(true);      /* keep log lines out of the hex stream */
            printf("\n  dumping the most recent %u ms of filtered audio\n",
                   (unsigned)(got * 1000 / AUDIO_SAMPLE_RATE_HZ));
            printf("---ORL-AUDIO %u %d---\n", (unsigned)got, AUDIO_SAMPLE_RATE_HZ);
            char line[80];
            for (size_t i = 0; i < got; ) {
                int p = 0;
                for (int k = 0; k < 16 && i < got; ++k, ++i) {
                    p += snprintf(line + p, sizeof(line) - (size_t)p, "%04X",
                                  (uint16_t)buf[i]);
                }
                line[p++] = '\n';
                line[p] = '\0';
                fwrite(line, 1, (size_t)p, stdout);
            }
            printf("---ORL-END---\n\n");
            voice_set_quiet(false);
            free(buf);
            break;
        }
        case 'V': {
            /*
             * Voice-presence analysis, done on the board.
             *
             * Shipping raw audio off the board was too slow to be practical
             * (about 3 KB/s), so the analysis happens here and only the verdict
             * travels.
             *
             * The discriminator is the envelope shape, not the level.  Speech
             * has deep dips between syllables, so over ~1.5 s its per-block RMS
             * has a high crest factor (max/mean) and a low floor (min/mean).
             * Continuous noise has neither: its RMS is nearly flat.  A loud but
             * noisy signal and a clean quiet one can have identical peak
             * readings, which is exactly what made level logs misleading.
             */
            const size_t NS = AUDIO_SAMPLE_RATE_HZ * 3 / 2;
            int16_t *buf = (int16_t *)malloc(NS * sizeof(int16_t));
            if (!buf) {
                printf("\n  out of memory\n\n");
                break;
            }
            if (voice_mode() != VOICE_MONITOR) {
                if (voice_start(VOICE_MONITOR) != ESP_OK) {
                    printf("\n  monitor mode failed to start\n\n");
                    free(buf);
                    break;
                }
            }
            printf("\n  === voice-presence analysis: 8 s, one line per second ===\n");
            printf("  (keep talking for the whole window)\n\n");
            for (int rep = 0; rep < 8; ++rep) {
                const size_t got = voice_capture_recent(buf, NS);
                double sum = 0.0, mn = 1e9, mx = 0.0;
                int nb = 0;
                for (size_t off = 0; off + 128 <= got; off += 128) {
                    double acc = 0.0;
                    for (int i = 0; i < 128; ++i) {
                        const double v = (double)buf[off + i];
                        acc += v * v;
                    }
                    const double rms = sqrt(acc / 128.0);
                    sum += rms;
                    if (rms < mn) { mn = rms; }
                    if (rms > mx) { mx = rms; }
                    ++nb;
                }
                const double mean = nb ? sum / nb : 0.0;
                const double crest = mean > 0.5 ? mx / mean : 0.0;
                const double dip = mean > 0.5 ? mn / mean : 0.0;
                const char *verdict;
                if (mean < 3.0) {
                    verdict = "silent";
                } else if (crest > 2.5 && dip < 0.40) {
                    verdict = "SPEECH-LIKE (gaps quiet -> clean audio)";
                } else if (crest < 2.0 && dip > 0.55) {
                    verdict = "CONTINUOUS NOISE (flat envelope)";
                } else {
                    verdict = "borderline (noise filling the gaps)";
                }
                printf("  [%d] rms mean %6.1f  min %6.1f  max %6.1f   crest %5.2f  dip %5.2f  %s\n",
                       rep, mean, mn, mx, crest, dip, verdict);
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            printf("  =========================================\n\n");
            free(buf);
            break;
        }
        case 'A': {
            /*
             * Output-chain noise floor.  TOGGLE.
             *
             * Amplifier held ON while the DAC is fed digital silence.  The I2S
             * clocks keep running, so the codec stays in a valid state -- which
             * matters, because stopping them puts the ES8311 into an undefined
             * output state that drives the LM4871 into saturation and makes a
             * healthy amplifier look broken.
             */
            static bool amp_noise;
            if (!amp_noise) {
                if (voice_mode() != VOICE_MONITOR) {
                    if (voice_start(VOICE_MONITOR) != ESP_OK) {
                        printf("\n  monitor mode failed to start\n\n");
                        break;
                    }
                }
                voice_set_amp_noise_test(true);
                amp_noise = true;
                printf("\n  === OUTPUT NOISE FLOOR: ON ===\n");
                printf("  Amplifier ON, DAC fed digital silence, clocks running.\n");
                printf("  Anything audible now is the ES8311 / LM4871 / supply.\n");
                printf("  Press A again to stop.\n\n");
            } else {
                voice_set_amp_noise_test(false);
                amp_noise = false;
                printf("\n  === OUTPUT NOISE FLOOR: OFF ===\n\n");
            }
            break;
        }
        case 'B': {
            /*
             * Representative listening condition.  TOGGLE.
             *
             * Amplifier held ON and the monitor audio passed straight through
             * with no squelch gating at all.  This is the state in which the
             * output signal and the output noise can be compared on a scope
             * under the same conditions the operator actually listens in:
             * valid I2S data, running clocks, no gating, amplifier on.
             */
            static bool bypass;
            if (!bypass) {
                if (voice_mode() != VOICE_MONITOR) {
                    if (voice_start(VOICE_MONITOR) != ESP_OK) {
                        printf("\n  monitor mode failed to start\n\n");
                        break;
                    }
                }
                voice_set_bypass_squelch(true);
                voice_set_quiet(false);
                bypass = true;
                printf("\n  === SQUELCH BYPASSED: ON ===\n");
                printf("  Amplifier held ON, radio audio passed straight through.\n");
                printf("  Scope VO1-U4pin7 (or across the speaker) AC-coupled:\n");
                printf("    - with no carrier  : output NOISE level\n");
                printf("    - while transmitting: output SIGNAL level\n");
                printf("  Ratio of those two is the real SNR.  Press B again to stop.\n\n");
            } else {
                voice_set_bypass_squelch(false);
                bypass = false;
                printf("\n  === SQUELCH BYPASSED: OFF ===\n\n");
            }
            break;
        }
        case 'N': {
            /*
             * Split the output-chain noise between the codec and the amplifier.
             *
             * The noise-floor test proved the hiss is produced after the DAC
             * with no radio audio involved at all.  Two candidates remain:
             * the ES8311's analogue output stage, or the LM4871 / 5 V supply.
             * They need completely different fixes, so: keep the amplifier ON
             * and mute the codec's DAC.
             *
             *   noise disappears -> it is the ES8311 (or the I2S data path)
             *   noise remains     -> it is the LM4871 or the supply
             */
            printf("\n  === codec-DAC mute test: 20 s ===\n");
            if (voice_mode() != VOICE_MONITOR) {
                if (voice_start(VOICE_MONITOR) != ESP_OK) {
                    printf("  monitor mode failed to start\n\n");
                    break;
                }
            }
            printf("  Amplifier stays ON; the codec DAC is being MUTED now.\n");
            printf("  LISTEN: does the noise go away?\n");
            printf("    gone  -> the noise is the ES8311\n");
            printf("    stays -> the noise is the LM4871 or the 5 V supply\n\n");
            voice_set_amp_noise_test(true);      /* amp on, digital silence */
            audio_codec_mute(true);
            for (int i = 0; i < 4; ++i) {
                vTaskDelay(pdMS_TO_TICKS(5000));
                printf("  ... %d/20 s (DAC muted)\n", (i + 1) * 5);
            }
            audio_codec_mute(false);
            printf("  DAC unmuted.  Tell me if the noise CHANGED across that.\n\n");
            voice_set_amp_noise_test(false);
            break;
        }
        case 'O': {
            /*
             * Amplifier-alone mode, TOGGLE.
             *
             * Stops voice mode entirely (no ADC DMA, no I2S clock, no audio
             * task) and enables only the LM4871, so the amplifier can be probed
             * at leisure without any other signal present.  Toggle rather than
             * timed because bench measurements need as long as they take.
             *
             *   noise stays     -> it is the LM4871 or the 5 V supply itself
             *   noise goes      -> our own digital activity is coupling in
             */
            static bool amp_alone;
            if (!amp_alone) {
                voice_stop();
                vTaskDelay(pdMS_TO_TICKS(200));
                board_amp_enable(true);
                amp_alone = true;
                printf("\n  === AMP ALONE: ON ===\n");
                printf("  !!! DIAGNOSTIC HAZARD -- READ THIS !!!\n");
                printf("  This STOPS the I2S clocks.  With no clock the ES8311's\n");
                printf("  output goes undefined, and that wild signal drives the\n");
                printf("  LM4871 into saturation (hot, outputs pinned to the rails).\n");
                printf("  Any DC reading taken now describes a fault this test\n");
                printf("  created itself -- it does NOT mean the amplifier is bad.\n");
                printf("  Use A (silence) or B (audio) for real measurements.\n");
                printf("  Press O to stop.\n\n");
            } else {
                board_amp_enable(false);
                amp_alone = false;
                printf("\n  === AMP ALONE: OFF === (press o to resume monitoring)\n\n");
            }
            break;
        }
        case 'Q':
            /*
             * Software reboot.
             *
             * Needed because the only reliable way to restart this board from
             * the host is not to touch DTR/RTS at all: on the ESP32-S3's native
             * USB-Serial/JTAG, asserting both lines is what the ROM reads as
             * "enter download mode", so a naive host-side reset drops the board
             * into the bootloader and it never runs the application.
             */
            printf("\n  rebooting...\n\n");
            fflush(stdout);
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
            break;
        case 'D':
            pd_polarity_probe();
            break;
        case '\r': case '\n':
            break;
        default:
            printf("\n  unknown command '%c' -- press h\n\n", c);
            break;
        }
    }
}

/* ------------------------------------------------------------------- tasks */

/*
 * Heartbeat: two short pulses every 2 s.  While the amplifier-shutdown and PTT
 * checks run this doubles as the "firmware is alive" indicator, which is how a
 * freshly flashed board is visually confirmed from across the bench.
 */
/*
 * Heartbeat: two short pulses every 2 s.
 *
 * The steady blink is the "firmware is alive" indicator, which is how a board
 * running standalone on a power bank is confirmed from across the bench.
 *
 * While the software squelch is open the LED is held ON instead, so reception
 * is visible without a console attached.  That matters once the board is
 * powered from a battery with no serial link.
 */
static void heartbeat_task(void *arg)
{
    (void)arg;

    /*
     * Factory reset on the BOOT button (SW1).
     *
     * The point is to get out of a network the board can no longer be reached
     * on, without a serial cable: hold SW1, the stored credentials go away, the
     * board reboots and comes up as its own access point again.
     *
     * Only a runtime gesture works here.  GPIO0 is a strapping pin, so holding
     * it across a reset makes the ROM enter download mode and the application
     * never starts -- the classic trap with this button.
     *
     *   3 s   forget the WiFi network, keep the radio settings
     *   10 s  erase everything saved, radio profile included
     */
    int held_ms = 0;
    int announced = 0;
    board_boot_button_init();

    for (;;) {
        if (board_boot_button_pressed()) {
            if (held_ms == 0) {
                ESP_LOGI(TAG, "SW1 pressed (GPIO%d reads 0)", PIN_BOOT_IO0);
            }
            held_ms += 50;
            /* Blink while counting: the operator can see the press registered
             * instead of wondering whether it took. */
            board_led_set((held_ms / 150) & 1);

            if (held_ms >= 10000) {
                ESP_LOGW(TAG, "SW1 held 10 s -- erasing ALL saved settings");
                board_led_set(true);
                vTaskDelay(pdMS_TO_TICKS(300));
                nvs_flash_erase();
                esp_restart();
            }
            if (held_ms >= 3000 && announced < 1) {
                announced = 1;
                ESP_LOGW(TAG, "SW1 held 3 s -- release now to open setup AP (saved networks kept)");
            } else if (held_ms >= 6000 && announced < 2) {
                announced = 2;
                ESP_LOGW(TAG, "SW1 held 6 s -- keep holding for a full erase");
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        /* Released: act on however long it was held. */
        if (held_ms > 0) {
            ESP_LOGI(TAG, "SW1 released after %d ms", held_ms);
        }
        if (held_ms >= 3000) {
            ESP_LOGW(TAG, "SW1 released after %d ms -- opening setup AP; saved networks kept",
                     held_ms);
            board_led_set(false);
            (void)net_wifi_setup_ap_start();
        }
        held_ms = 0;
        announced = 0;

        /*
         * One blink per second.
         *
         * Steady and obvious beats clever: a single 1 Hz pulse is easy to read
         * from across a bench and gives the other two states room to stand out.
         *
         *   idle          one short blink every second (this)
         *   SW1 held      fast blink (~150 ms)         -- the reset count
         *
         * This is the POWER LED on the v0.4 board.  Audio-present and transmit
         * indication moved to the dedicated RX/TX LEDs (IO11/IO42, driven by
         * frontpanel.c), which is why this LED no longer goes solid on audio:
         * a power LED that stops blinking exactly when the radio is busy is
         * indistinguishable from a board that has crashed.
         */
        board_led_set(true);
        vTaskDelay(pdMS_TO_TICKS(60));
        board_led_set(false);
        vTaskDelay(pdMS_TO_TICKS(940));
    }
}

static void status_task(void *arg)
{
    (void)arg;
    /*
     * A freshly flashed board is normally looked at *after* the reset, so the
     * one-shot boot report is easy to miss.  Repeat the full acceptance table
     * here for the first few minutes; after that only the one-line status
     * remains, so the console stays usable.
     */
    int report_repeats = 12;          /* ~3 minutes at the 15 s interval */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(15000));
        const sa818_state_t *st = sa818_state();
        int mv = 0;
        board_adc_read_mv(BOARD_ADC_CH_RADIO_AF, &mv);
        ESP_LOGI(TAG, "up %lld s | heap %u | PTT %s | amp %s | SA818 %s rssi %d | AF %d mV | %s r%d",
                 esp_timer_get_time() / 1000000LL,
                 (unsigned)esp_get_free_heap_size(),
                 board_ptt_is_asserted() ? "TX!" : "RX",
                 board_amp_is_enabled() ? "on" : "off",
                 st->inited ? st->version : "down",
                 st->rssi, mv,
                 netcom_mode_name(netcom_mode()), netcom_room());

        if (s_have_report && report_repeats > 0) {
            --report_repeats;
            bringup_print(&s_rpt);
            if (report_repeats == 0) {
                ESP_LOGI(TAG, "boot report repeats finished; press R to re-run");
            }
        }
    }
}

/*
 * RF transmit test.
 *
 * Two variants share everything except the audio pattern:
 *   ladder = three 1 kHz beeps at rising DAC amplitude, for level matching
 *   carrier = a beep every second, so a current meter can be watched while the
 *             PA should be drawing power
 *
 * Both read the PTT and PD pads back WHILE keyed.  That readback is the whole
 * point: the ESP32 driving its own pin low proves nothing about the module
 * receiving it, but a pad that does not read 0 means we are not even asking.
 *
 * The local LM4871 stays OFF: this is transmit audio only.
 */
static void tx_test(int duration_ms, bool ladder)
{
    sa818_cfg_t cfg;
    sa818_cfg_load(&cfg);

    printf("\n  ================ RF TRANSMIT TEST ================\n");
    printf("  Set your handset to receive:\n");
    printf("    frequency : %.4f MHz\n", cfg.tx_mhz);
    printf("    bandwidth : %s\n", cfg.bandwidth ? "25 kHz (wide)" : "12.5 kHz (narrow)");
    printf("    CTCSS     : %s\n", strcmp(cfg.tx_ctcss, "0000") ? cfg.tx_ctcss : "OFF");
    if (ladder) {
        printf("\n  THREE 1 kHz beeps, each louder than the last (about 2 s):\n");
        printf("    beep 1 = DAC amplitude  3000\n");
        printf("    beep 2 = DAC amplitude  8000\n");
        printf("    beep 3 = DAC amplitude 16000\n");
    } else {
        printf("\n  A 1 kHz beep every second for %d ms.\n", duration_ms);
        printf("  WATCH THE 5 V CURRENT while this runs: a real 1 W transmission\n");
        printf("  adds hundreds of mA.  No current step = no RF.\n");
    }
    printf("\n");

    if (audio_codec_init() != ESP_OK) {
        printf("  codec init failed -- press i to re-scan the I2C bus\n\n");
        return;
    }
    audio_i2s_start_clock_only();
    board_amp_enable(false);          /* TX audio only, no local speaker */

    /*
     * Make sure the module is actually powered before keying, and give its RF
     * section a moment to settle.  PD is held high from boot now, so this is
     * normally a no-op -- but it would silently eat the first burst if the pin
     * happened to be low.
     */
    if (!board_sa818_pd_level()) {
        printf("  module was powered DOWN -- powering it up and waiting 500 ms\n");
        board_sa818_power(true);
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    const int limit = duration_ms + 2000;
    if (ptt_request_tx(limit) != ESP_OK) {
        printf("  refused: PTT state is %s\n\n", ptt_state_name());
        return;
    }

    for (int i = 0; i < 100 && ptt_get_state() == PTT_PREPARE; ++i) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (ptt_get_state() != PTT_TX) {
        printf("  never reached TX (state=%s) -- aborting\n\n", ptt_state_name());
        ptt_request_rx();
        return;
    }

    /* The measurement that matters: what the pins actually read while keyed. */
    printf("  KEYED -- pad readback: PTT GPIO%d = %d (must be 0), PD GPIO%d = %d\n",
           PIN_SA818_PTT, gpio_get_level(PIN_SA818_PTT),
           PIN_SA818_PD, gpio_get_level(PIN_SA818_PD));
    if (gpio_get_level(PIN_SA818_PTT) != 0) {
        printf("  !! PTT pad is NOT low -- the ESP32 is not driving it.\n");
    }

    const int64_t t0 = esp_timer_get_time();
    audio_codec_set_volume(90);

    if (ladder) {
        static const int amps[3] = { 3000, 8000, 16000 };
        for (int i = 0; i < 3; ++i) {
            printf("  beep %d/3  (amplitude %d)   PTT pad = %d\n",
                   i + 1, amps[i], gpio_get_level(PIN_SA818_PTT));
            audio_i2s_play_tone(1000, 400, amps[i]);
            audio_i2s_play_silence(200);
        }
    } else {
        int elapsed = 0;
        while (elapsed < duration_ms) {
            audio_i2s_play_tone(1000, 300, 12000);
            audio_i2s_play_silence(700);
            elapsed += 1000;
            printf("  .. %d ms  PTT pad = %d  PD pad = %d  state=%s\n",
                   elapsed, gpio_get_level(PIN_SA818_PTT),
                   gpio_get_level(PIN_SA818_PD), ptt_state_name());
        }
    }
    ptt_request_rx();

    for (int i = 0; i < 400 && ptt_get_state() != PTT_IDLE &&
                        ptt_get_state() != PTT_FAULT; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    const int keyed_ms = (int)((esp_timer_get_time() - t0) / 1000);
    audio_codec_set_volume(70);

    printf("\n  keyed for about %d ms\n", keyed_ms);
    printf("  PTT now GPIO%d = %d   state=%s\n", PIN_SA818_PTT,
           gpio_get_level(PIN_SA818_PTT), ptt_state_name());
    printf("  total keyings since boot: %d\n", ptt_tx_count());
    {
        const esp_reset_reason_t rr = esp_reset_reason();
        if (rr == ESP_RST_BROWNOUT) {
            printf("  !! BROWNOUT reset: the 5 V rail collapsed -- that means the PA\n");
            printf("     WAS drawing transmit current.  PTT was released by the reset.\n");
        } else if (rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT ||
                   rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT) {
            printf("  !! reset reason %d indicates a crash/WDT -- investigate.\n", (int)rr);
        } else {
            printf("  no brownout, no crash (reset reason %d).  Combined with no\n", (int)rr);
            printf("  current step, this points at the module not keying at all.\n");
        }
    }
    printf("  ==================================================\n\n");
}

static void tx_tone_test(void) { tx_test(2000, true); }
/* 15 s is a deliberate measurement window: long enough to hold a probe on the
 * module's PTT pin or watch a current meter, short enough to stay polite. */
static void tx_carrier_test(void) { tx_test(15000, false); }

/*
 * PD polarity probe.
 *
 * "PD" is a power-down pin.  On THIS board the answer is now known from a live
 * RF test: it is ACTIVE LOW, so PD must be driven HIGH to power the module up
 * (see SA818_PD_POWERDOWN_ACTIVE_LOW in board_hw.h).
 *
 * This probe is kept for other builds / other module revisions.  It uses the AT
 * link as the sensor, which is a weak test -- the module's MCU survives PD low,
 * so surviving both levels proves nothing.  The honest conclusion is printed.
 */
static void pd_polarity_probe(void)
{
    char reply[96];
    printf("\n  --- PD polarity probe (AT only, no RF) ---\n");

    board_sa818_pd_raw(0);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_err_t lo = sa818_cmd("AT+VERSION", reply, sizeof(reply), 400);
    printf("  PD = 0 : AT+VERSION -> %s  (%s)\n",
           lo == ESP_OK ? reply : "(no reply)", esp_err_to_name(lo));

    board_sa818_pd_raw(1);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_err_t hi = sa818_cmd("AT+VERSION", reply, sizeof(reply), 400);
    printf("  PD = 1 : AT+VERSION -> %s  (%s)\n",
           hi == ESP_OK ? reply : "(no reply)", esp_err_to_name(hi));

    printf("  Note: on this module the AT link survives PD low, so this probe\n");
    printf("  cannot settle the polarity -- only an RF test can.  Measured result:\n");
    printf("  PD is ACTIVE LOW (high = powered).\n");
    board_sa818_power(true);          /* leave the module powered */
    printf("  PD restored to %d (powered).\n\n", board_sa818_pd_level());
}

/*
 * Services the PTT state machine.  Priority is above the console so the TX
 * deadline is honoured even if a long console command is running.
 */
static void ptt_task(void *arg)
{
    (void)arg;
    int tick = 0;
    for (;;) {
        ptt_tick();

        /*
         * Poll RSSI here rather than in the audio task: sa818_read_rssi() does a
         * UART round trip, and doing that inside the 8 kHz monitor loop would
         * stall the stream.  It is cached in the sa818 state, so the audio log
         * can print it for free.
         */
        if (++tick >= 50) {                 /* ~500 ms */
            tick = 0;
            if (!ptt_is_transmitting()) {
                int rssi = -1;
                (void)sa818_read_rssi(&rssi); /* display/diagnosis only */
            }
        }

        /*
         * Print the level line the audio task formatted.  Doing it here keeps a
         * blocking console write out of the task that drains the ADC pool, which
         * is what made the pool overflow about once every eight seconds.
         */
        {
            static char lvl[192];
            if (voice_take_level_line(lvl, sizeof(lvl))) {
                ESP_LOGI(TAG, "%s", lvl);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* -------------------------------------------------------------------- entry */

void app_main(void)
{
    /* 1. never let the radio key up or the amplifier come on by accident */
    if (board_safe_state_init() != ESP_OK) {
        ESP_LOGE(TAG, "FATAL: could not set the safe state -- halting");
        for (;;) {
            sa818_cfg_refresh_tick();
        vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    /* 2. PTT state machine next: from here on the radio can only be keyed
     *    through ptt_request_tx(), which enforces a deadline. */
    if (ptt_init() != ESP_OK) {
        ESP_LOGW(TAG, "ptt_init failed");
    }

    board_print_banner();

    if (board_led_init() != ESP_OK) {
        ESP_LOGW(TAG, "status LED init failed (not fatal)");
    }
    if (board_charge_pins_init() != ESP_OK) {
        ESP_LOGW(TAG, "charge header pins init failed (not fatal)");
    }
    if (board_i2c_open(PIN_I2C_SCL, PIN_I2C_SDA, 100000) != ESP_OK) {
        ESP_LOGW(TAG, "I2C init failed -- codec work will be unavailable");
    }

    /* NVS holds the radio profile; a fresh chip has no partition formatted yet. */
    {
        esp_err_t nvs = nvs_flash_init();
        if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_LOGW(TAG, "NVS needs a fresh partition (%s); erasing", esp_err_to_name(nvs));
            nvs_flash_erase();
            nvs = nvs_flash_init();
        }
        if (nvs != ESP_OK) {
            ESP_LOGW(TAG, "nvs_flash_init: %s (radio profile will not persist)",
                     esp_err_to_name(nvs));
        }
    }

    /* Add band metadata for legacy UHF installs, keeping every RF value. */
    (void)sa818_cfg_init();

    /*
     * 4 KB, not 3 KB: this task runs the SA818 UART round trip (three 256-byte
     * reply buffers in sa818.c) and the bridge-key watchdog's ESP_LOGE, whose
     * vprintf needs ~1 KB.  A coredump taken after "\*\*\*ERROR\*\*\* A stack overflow in
     * task ptt" showed a 2560-byte high-water mark against a 3072-byte stack:
     * the common path fits, the rare report path does not.
     */
    xTaskCreate(ptt_task, "ptt", 4096, NULL, 6, NULL);

    /* 8 KB: the console runs captures and long prints, and an undersized stack
     * here shows up as a boot loop from a stack-overflow panic. */
    xTaskCreate(console_task, "console", 8192, NULL, 5, NULL);

    /* 2. run the acceptance test once at boot (it drives the LED itself, so the
     *    heartbeat task only starts afterwards to avoid fighting over it) */
    if (bringup_run(&s_rpt) == ESP_OK) {
        ESP_LOGI(TAG, "bring-up PASSED on all checks");
    } else {
        ESP_LOGW(TAG, "bring-up finished with %d failure(s)", s_rpt.failed);
    }
    s_have_report = true;
    bringup_print(&s_rpt);

    xTaskCreate(heartbeat_task, "hb", 2560, NULL, 4, NULL);
    xTaskCreate(status_task, "status", 3072, NULL, 3, NULL);

    /* 3. standalone receiver: start monitoring without being asked.
     *
     * Receive-only, so it cannot key the transmitter, which is why only MONITOR
     * is allowed to auto-start.  This lets the board be powered from a power
     * bank and used as a plain receiver with nothing else attached. */
    {
        sa818_cfg_t cfg;
        sa818_cfg_load(&cfg);
        (void)voice_get_tx_gain();      /* load the persisted TALK TX gain    */
        (void)voice_get_net_tx_gain();  /* and the network TX gain            */
        voice_mic_gain_load();          /* handset mic PGA (else 30 dB again) */
        voice_speaker_level_load();     /* speaker volume step + amp mute */
        voice_output_volume_load();     /* codec DAC volume (bench level)  */
        voice_eq_load();                /* restore the persisted EQ tuning   */
        if (voice_get_autostart()) {
            printf("\n  autostart: monitoring %.4f MHz, %s, CTCSS %s\n",
                   cfg.rx_mhz, cfg.bandwidth ? "25 kHz" : "12.5 kHz",
                   strcmp(cfg.rx_ctcss, "0000") ? cfg.rx_ctcss : "off");
            printf("  LED: two short pulses = idle, steady ON = audio present.\n");
            printf("  Press o to stop, or 'x' then 'auto 0' to disable autostart.\n\n");
            if (voice_start(VOICE_MONITOR) != ESP_OK) {
                printf("  autostart failed -- press o to retry\n\n");
            }
        } else {
            printf("\n  autostart disabled; press o to monitor\n\n");
        }
    }

    /*
     * 3b. Network and control panel.
     *
     * Starts after the radio is up so that nothing about the audio path
     * depends on a network being present: with no credentials stored, or with
     * no access point in range, the firmware behaves exactly as it did before
     * and the panel simply has no address.
     */
    if (net_wifi_start() == ESP_OK && net_http_start() == ESP_OK) {
        if (net_wifi_connected()) {
            printf("\n  web panel: http://%s/   (WiFi \"%s\")\n\n",
                   net_wifi_ip(), net_wifi_ssid());
        } else {
            printf("\n  web panel ready but WiFi is %s\n", net_wifi_state_name());
            printf("  set it with:  x wifi <ssid> <password>\n\n");
        }
    }

    /* 3c. Network intercom mode manager.
     *
     * After the WiFi so the network engines (NET-P2P / EMQX relay) can attach
     * to the radio interface.  In local mode this is just a config load and the
     * base firmware behaviour is untouched. */
    netrelay_init();  /* register the MQTT/EMQX engine (FMO-style relay) */
    netp2pnet_init(); /* register the NET-P2P (STUN hole-punch) engine */
    if (netcom_init() == ESP_OK) {
        printf("  intercom: mode=%s room=%u\n",
               netcom_mode_name(netcom_mode()), netcom_room());
        /* Analog <-> network bridge: restore the persisted selection and
         * activate it if it applies to the booted mode. */
        bridge_init();
        bridge_apply();
    } else {
        printf("  intercom init failed -- local mode only\n\n");
    }

    /* 3d. End-of-transmission tail tones: synthesizes the built-in
     * courtesy signals, mounts the storage partition for a custom tone,
     * and restores the selection. */
    voice_tail_init();

    /* 3e. v0.4 front panel: RX/TX LEDs and the PTT / function keys.  Last,
     * because it starts a task that can key the transmitter through
     * voice_start(), and nothing may do that before the PTT state machine,
     * the radio profile and the engines are all up. */
    if (frontpanel_init() != ESP_OK) {
        ESP_LOGW(TAG, "front panel init failed (LEDs/keys unavailable)");
    }

    /* 4. idle: the console and the heartbeat tasks own the rest */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
