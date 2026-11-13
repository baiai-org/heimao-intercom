/*
 * netp2pnet -- NET-P2P engine (netcom mode 1): public-internet UDP direct via
 * STUN hole punching.  See netp2pnet.h for the API and
 * docs/NETP2P-PUBNET-PLAN.md (v2) for the design.
 *
 * Architecture
 * ------------
 *  - One long-lived UDP socket bound to UDP_PORT_MAIN (54321).  It carries the
 *    KNOCK/PING/BYE control frames AND the G.711 audio.  A second socket bound
 *    to UDP_PORT_PROBE (54322) is used once per STUN cycle purely to learn
 *    whether our NAT preserves the mapped port (full-cone / addr-restricted
 *    => yes, symmetric / port-restricted => no), which the panel reports so
 *    the operator can pick the NAT-friendly end as the "target".
 *  - The STUN task (core 0) does DNS (getaddrinfo) + builds RFC 5389 binding
 *    requests and sendto()s them.  It keeps the NAT mapping warm every ~15 s.
 *    Responses are received by the engine task (the single public-info
 *    writer), which parses XOR-MAPPED-ADDRESS and updates {pub_ip, pub_port}.
 *  - The engine task (core 1) runs select() over both sockets (20 ms timeout),
 *    receives peer frames, validates room + PSK, drives the per-peer state
 *    machine (KNOCK -> ACK -> LINKED; 90 s of silence -> EXPIRED), decodes
 *    audio into per-peer PSRAM jitter buffers, forwards hub-mode member audio
 *    verbatim to the other linked peers, and answers PING with PONG.
 *  - The playback task (core 1) walks the most-active peer's jitter buffer at
 *    the 20 ms packet cadence, owns the DAC in normal mode (or pushes into
 *    the monitor in VOICE_MONITOR), and gates the LM4871 amplifier + analog
 *    bridge key with the incoming audio level.
 *
 * Frame format (14-byte header, same layout the relay engine uses; the `reserved`
 * byte is repurposed as the frame type so the audio path is unchanged):
 *
 *   off  len  field
 *   0    2    magic 0x4F52 ("OR")
 *   2    2    room   1000..9999, must match netcom_room()
 *   4    2    seq    per-sender audio counter (0 for control frames)
 *   6    6    sender MAC
 *   12   1    flags  bit0 = PTT active (audio only)
 *   13   1    type   0=audio 1=KNOCK 2=KNOCK_ACK 3=PING 4=PONG 5=BYE
 *   14   8    psk_check  HMAC-SHA256(key=PSK, room_be16||mac||"ORL-NP2P")[:8]
 *   (control) 2 nat_hint(1: 0 unknown / 1 port-preserving / 2 not) + ver(1=1)
 *   (audio) 2 optional big-endian talk epoch when flags bit5 is set; otherwise
 *             legacy nat_hint/ver. Header/audio lengths are unchanged.
 *   (KNOCK / KNOCK_ACK) +6: sender pub_ip(4, net order) + pub_port(2)
 *   (audio) +80: fixed-size Opus frame, 16 kHz, 320 samples = 20 ms
 *
 *   audio = 104 B, KNOCK/ACK = 30 B, PING/PONG/BYE = 24 B.
 *
 * PSK: the 13-byte netcom PSK (NVS ncpsk) when set; otherwise a built-in
 * default so freshly-factory boards intercom out of the box.  A frame from a
 * different room or with a bad psk_check is dropped.
 */
#include "netp2pnet.h"
#include "np_access.h"
#include "net_rx_order.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"   /* xTaskCreatePinnedToCoreWithCaps */
#include "freertos/semphr.h"
#include "freertos/portmacro.h"
#include "nvs.h"

#include "lwip/err.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/ip_addr.h"

#include "mbedtls/md.h"

#include "audio_i2s.h"
#include "board_hw.h"
#include "bridge.h"
#include "netcom.h"
#include "radio_led.h"
#include "opus_link.h"
#include "net_wifi.h"
#include "ptt.h"
#include "sdr.h"
#include "voice.h"
#include "voice_tail.h"

static const char *TAG = "np2p";

/* Exactly one courtesy tone per over, and never hold the carrier forever. */
static bool s_tail_played;
static int64_t s_tail_start_ms;

/* ------------------------------------------------------------------ consts */

#define UDP_PORT_MAIN        54321   /* control + audio, long-lived */
#define UDP_PORT_PROBE       54322   /* port-preservation probe only */

#define NP_MAGIC             0x4F52
#define NP_FLAG_HUB          4u  /* this direct sender acts as a hub */
#define NP_FLAG_FORWARDED    8u  /* one hub hop already consumed */
#define NP_FLAG_ROLE         16u /* hub role bit is authoritative */
/* 40 ms at 12 kHz: Opus needs a standard frame duration, and 40 ms keeps
 * the packet rate at 25/s. */
#define NP_PKT_SAMPLES       OPUS_LINK_SAMPLES   /* one Opus frame */
#define NP_HDR_LEN           14
#define NP_CHECK_LEN         8
#define NP_CTR_BASE          (NP_HDR_LEN + NP_CHECK_LEN)            /* 22 */
/* 16-bit linear PCM in the audio frames (was G.711 u-law, 8 bit/sample).
 * 232 samples stay one packet (29 ms); 488 bytes fit any MTU.  Receivers sniff
 * the payload length so a legacy peer's u-law frame is still decodable. */
#define NP_AUDIO_OPUS        (OPUS_LINK_BITRATE / 8 * OPUS_LINK_FRAME_MS / 1000) /* 120 */
#define NP_AUDIO_LEN         (NP_CTR_BASE + 2 + NP_AUDIO_OPUS)       /* 144 */
#define NP_CTR_KNOCK_LEN     (NP_CTR_BASE + 2 + 4 + 2)              /* 30 */
#define NP_CTR_PING_LEN      (NP_CTR_BASE + 2)                      /* 24 */

#define JBUF_SLOTS           128     /* ~3.7 s per peer (PSRAM) */

/* Carrier hang after the far end's PTT flag clears, and how long a missing
 * frame may go before we assume the sender is gone (same rule as netrelay.c). */
#define NP_RX_HANG_MS        250   /* after the last PLAYED block, not after
                                      * the last received ptt flag: the cushion
                                      * means those are ~0.5 s apart */
#define NP_RX_PTT_TIMEOUT_MS 2000
/*
 * Playout cushion, in 29 ms packets.  3 packets (87 ms) is not enough on WiFi:
 * a late packet makes peer_jbuf_next() return NULL, the play task injects a
 * hole, and the audio is heard as chopped.  12 packets (~350 ms) absorbs the
 * burst jitter the same way the SDR path's 600 ms cushion does.  The extra
 * delay is only on the network -> radio direction.
 */
#define NP_STREAM_IDLE_MS_LOCAL  1000
#define PLAY_LAG             6       /* 120 ms at 20 ms frames: the operator asked for
                                      * 120 ms on both engines (was 240 ms = 12 frames). */
#define STREAM_IDLE_MS       500
/* Must match the packet: 480 samples at 12 kHz = 40 ms.  While this said 29
 * (the old 348-sample framing) the monitor loop injected 40 ms of audio every
 * 29 ms of wall time -- 38 % too fast -- and the bridge stuttered even on a
 * LAN.  Whatever changes NP_PKT_SAMPLES must change this with it. */
#define NP_PKT_MS            (NP_PKT_SAMPLES * 1000 / AUDIO_SAMPLE_RATE_HZ)
#define PEER_MAX             NP_PEER_LIMIT
#define SLOT_MAX             (PEER_MAX + 1) /* +1 self-test slot (not persisted) */
#define KNOCK_PERIOD_MS      1000
#define PING_PERIOD_MS       10000
#define PEER_EXPIRE_MS       90000   /* no packet from a linked peer        */

#define STUN_PERIOD_MS       15000
#define STUN_FAIL_COOLDOWN_MS 60000
#define STUN_MAX_SRV         8

/* RFC 5389 */
#define STUN_MAGIC           0x2112A442u
#define STUN_FP_CONST        0x5354554Eu
#define STUN_BINDING_REQ     0x0001
#define STUN_BINDING_RSP     0x0101
#define STUN_ATTR_XORMAPPED  0x0020
#define STUN_ATTR_MAPPED     0x0001
#define STUN_ATTR_FINGERPRINT 0x8028
#define STUN_TXID_LEN        12

/* Built-in default PSK (13 bytes) used when no operator PSK is set, so two
 * factory boards punch straight through.  Change it by setting the netcom PSK. */
static const uint8_t s_psk_default[NETCOM_PSK_LEN] = {
    'O', 'R', 'L', '-', 'N', 'P', '2', 'P', 0x31, 0x30, 0x31, 0x36, 0x21,
};
static const char s_psk_salt[] = "ORL-NP2P";

/* ------------------------------------------------------------------ header */

#pragma pack(push, 1)
typedef struct {
    uint16_t magic;
    uint16_t room;
    uint16_t seq;
    uint8_t  mac[6];
    uint8_t  flags;
    uint8_t  type;
} np_header_t;
#pragma pack(pop)
_Static_assert(sizeof(np_header_t) == NP_HDR_LEN, "header must be 14 bytes");
_Static_assert(sizeof(np_header_t) == NP_HDR_LEN, "header must be 14 bytes");
/* A bound, not an exact number: the audio frame length follows the codec
 * settings (it was 144 bytes at 24 kbit/s, and 184 at 32), so pinning it here
 * only turned a deliberate rate change into a build error.  What matters is that
 * an audio datagram stays far below any MTU and inside the packet buffers. */
_Static_assert(NP_AUDIO_LEN <= 512 && NP_CTR_KNOCK_LEN == 30, "frame sizes");

/* -------------------------------------------------------------------- utils */

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

/* Standard CRC32 (polynomial 0xEDB88320 reversed, zlib-compatible) -- lwIP 2.2
 * has no crc.h, so we roll our own for the STUN Fingerprint attribute. */
static uint32_t s_crc_tab[256];
static void crc32_init(void)
{
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        s_crc_tab[i] = c;
    }
}
static uint32_t crc32(const uint8_t *data, size_t len)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        c = s_crc_tab[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

static uint32_t be32(const uint8_t *p)
{ return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static void put_be32(uint8_t *p, uint32_t v)
{ p[0] = (v >> 24) & 0xFF; p[1] = (v >> 16) & 0xFF; p[2] = (v >> 8) & 0xFF; p[3] = v & 0xFF; }
static void put_be16(uint8_t *p, uint16_t v) { p[0] = (v >> 8) & 0xFF; p[1] = v & 0xFF; }

/* A STUN message keeps the magic cookie at byte offset 4: the first four bytes
 * are the message type and the message length.  Testing the start of the
 * datagram against the cookie therefore never matches a real reply (a Binding
 * Success Response starts 0x0101 <len>), which is exactly how replies got
 * dropped as if they were peer frames. */
static bool stun_is_message(const uint8_t *p, int len)
{
    return len >= 20 && be32(p + 4) == STUN_MAGIC;
}

static void json_escape_str_n(const char *in, char *out, size_t out_len)
{
    size_t o = 0;
    for (size_t i = 0; in && in[i] && o + 5 < out_len; ++i) {
        char c = in[i];
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = c; }
        else if ((unsigned char)c < 0x20) {
            o += (size_t)snprintf(out + o, out_len - o, "\\u%04x", (unsigned)c);
        }
        else { out[o++] = c; }
    }
    if (out_len) { out[o] = '\0'; }
}

/* --------------------------------------------------------------- PSK check */

/* psk_check = HMAC-SHA256(key=PSK, room_be16 || sender_mac || "ORL-NP2P")[:8]. */
static void psk_get(uint8_t out[NETCOM_PSK_LEN])
{
    bool set = false;
    netcom_psk(out, &set);
    if (!set) {
        memcpy(out, s_psk_default, NETCOM_PSK_LEN);
    }
}

static void psk_compute(uint16_t room, const uint8_t mac[6], uint8_t out[NP_CHECK_LEN])
{
    uint8_t key[NETCOM_PSK_LEN];
    psk_get(key);
    uint8_t msg[2 + 6 + sizeof(s_psk_salt) - 1];
    msg[0] = (room >> 8) & 0xFF;
    msg[1] = room & 0xFF;
    memcpy(&msg[2], mac, 6);
    memcpy(&msg[8], s_psk_salt, sizeof(s_psk_salt) - 1);
    uint8_t full[32];
    (void)mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                          key, NETCOM_PSK_LEN, msg, sizeof(msg), full);
    memcpy(out, full, NP_CHECK_LEN);
}

/* ----------------------------------------------------------------- sockets */

static int      s_sock_main = -1;
static int      s_sock_probe = -1;
static int      s_sock6 = -1;      /* dual-stack partner for IPv6 peers */

static esp_err_t socket_up(void)
{
    s_sock_main = lwip_socket(AF_INET, SOCK_DGRAM, 0);
    if (s_sock_main < 0) {
        ESP_LOGE(TAG, "main socket failed");
        return ESP_FAIL;
    }
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = 0;              /* INADDR_ANY */
    put_be16((uint8_t *)&a.sin_port, UDP_PORT_MAIN);
    if (lwip_bind(s_sock_main, (struct sockaddr *)&a, sizeof(a)) != 0) {
        ESP_LOGE(TAG, "bind main:%u failed", (unsigned)UDP_PORT_MAIN);
        lwip_close(s_sock_main); s_sock_main = -1;
        return ESP_FAIL;
    }
    int rcvbuf = 16384;
    (void)lwip_setsockopt(s_sock_main, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    s_sock_probe = lwip_socket(AF_INET, SOCK_DGRAM, 0);
    if (s_sock_probe >= 0) {
        struct sockaddr_in p;
        memset(&p, 0, sizeof(p));
        p.sin_family = AF_INET;
        p.sin_addr.s_addr = 0;
        put_be16((uint8_t *)&p.sin_port, UDP_PORT_PROBE);
        if (lwip_bind(s_sock_probe, (struct sockaddr *)&p, sizeof(p)) != 0) {
            lwip_close(s_sock_probe); s_sock_probe = -1;
        }
    }
    /* IPv6 partner socket: peers with a v6 address are reached directly (no
     * NAT, no hole punching), so a plain dual socket is enough. */
    s_sock6 = lwip_socket(AF_INET6, SOCK_DGRAM, 0);
    if (s_sock6 >= 0) {
        int v6only = 1;               /* keep the v4 socket authoritative */
        (void)lwip_setsockopt(s_sock6, IPPROTO_IPV6, IPV6_V6ONLY, &v6only,
                              sizeof(v6only));
        struct sockaddr_in6 a6;
        memset(&a6, 0, sizeof(a6));
        a6.sin6_family = AF_INET6;
        a6.sin6_addr = in6addr_any;
        a6.sin6_port = htons(UDP_PORT_MAIN);
        if (lwip_bind(s_sock6, (struct sockaddr *)&a6, sizeof(a6)) != 0) {
            lwip_close(s_sock6);
            s_sock6 = -1;
        }
    }
    ESP_LOGI(TAG, "sockets up: main:%u probe:%s ipv6:%s", (unsigned)UDP_PORT_MAIN,
             s_sock_probe >= 0 ? "54322" : "(no probe)",
             s_sock6 >= 0 ? "yes" : "no");
    return ESP_OK;
}

static void socket_down(void)
{
    if (s_sock_main >= 0) { lwip_close(s_sock_main); s_sock_main = -1; }
    if (s_sock6 >= 0) { lwip_close(s_sock6); s_sock6 = -1; }
    if (s_sock_probe >= 0) { lwip_close(s_sock_probe); s_sock_probe = -1; }
}

/* --------------------------------------------------------------- STUN state */

typedef struct {
    char         host[64];
    uint16_t     port;
    bool         have_ip;
    struct in_addr ip4;
    int64_t      last_fail_ms;
} stun_srv_t;

static stun_srv_t s_srv[STUN_MAX_SRV];
static int        s_srv_n;
static bool       s_have_override;
static char       s_ovr_host[64];
static uint16_t   s_ovr_port;
static char       s_stun_str[128];   /* "" = built-in list; else "host:port" */

static void stun_default_list(void)
{
    struct { const char *h; uint16_t p; } def[5] = {
        { "stun.chat.bilibili.com", 3478 },
        { "stun.miwifi.com",        3478 },
        { "stun.cloudflare.com",    3478 },
        { "stun.metered.ca",        3478 },
        { "stun.twilio.com",        3478 },
    };
    s_srv_n = 0;
    for (int i = 0; i < 5; ++i) {
        snprintf(s_srv[s_srv_n].host, sizeof(s_srv[s_srv_n].host), "%s", def[i].h);
        s_srv[s_srv_n].port = def[i].p;
        s_srv[s_srv_n].have_ip = false;
        s_srv[s_srv_n].last_fail_ms = 0;
        s_srv_n++;
    }
    s_have_override = false;
    s_ovr_host[0] = '\0';
    s_stun_str[0] = '\0';
}

typedef struct {
    bool     pending;
    uint8_t  txid[STUN_TXID_LEN];
    int64_t  sent_ms;
    int      server_idx;      /* -1 = probe socket, -2 = manual override */
} stun_pending_t;

/* Public-internet info: written by the engine task only. */
static uint32_t s_pub_ip;           /* network byte order, 0 = unknown */
static uint16_t s_pub_port;
static int64_t  s_last_stun_ms;
static int      s_last_rtt_ms;
static int      s_port_preserving;  /* -1 unknown, 1 yes, 2 no          */
static volatile bool s_main_port_preserved;   /* main socket mapped==local */
static volatile bool s_probe_port_preserved;  /* probe socket mapped==local */
static volatile bool s_stun_now_req;

/* A NAT preserves the source port iff the STUN-mapped port equals the local
 * port we sent from.  We probe from two local ports (54321 + 54322); a
 * full-cone / addr-restricted NAT preserves both, a symmetric (port
 * restricted) one changes them.  The verdict is "yes" only when both preserve
 * (the common, punchable cases), "no" when both change. */
static void nat_preserving_update(void)
{
    if (s_main_port_preserved && s_probe_port_preserved) {
        s_port_preserving = 1;
    } else if (!s_main_port_preserved && !s_probe_port_preserved) {
        s_port_preserving = 2;
    } else {
        s_port_preserving = -1;
    }
}

static stun_pending_t s_pend_main, s_pend_probe;

/* --------------------------------------------------------------- peer state */

/* One packet source / peer endpoint, IPv4 or IPv6.  The engine used to be
 * IPv4-only (sockaddr_in everywhere); a dual-stack socket plus this type is
 * enough to accept IPv6 peers without touching the wire format: an IPv6 peer
 * needs no NAT hole punching, it is reached directly. */
typedef struct {
    uint8_t  fam;        /* 4 or 6 */
    uint32_t ip;         /* fam 4, network byte order */
    uint8_t  ip6[16];    /* fam 6 */
    uint16_t port;
} np_addr_t;

typedef struct {
    bool     in_use;
    bool     selftest;
    uint8_t  mac[6];
    char     name[8];
    uint32_t ip;                 /* network byte order (IPv4 peers) */
    uint8_t  fam;                /* 4 (ip) or 6 (ip6) */
    uint8_t  ip6[16];            /* IPv6 peers */
    uint16_t port;
    bool     linked;
    bool     manual;             /* entered by the operator (not auto-learned) */
    bool     hub;                /* advertised/inferred role of a direct peer */
    bool     via_hub;            /* audio origin, not a directly reachable peer */
    uint8_t  hub_mac[6];          /* upstream transport identity for via_hub */
    np_addr_t rx_src;            /* address the last authenticated frame came from */
    int64_t  rx_src_ms;
    int64_t  last_rx_ms;         /* last valid AUDIO block (stream freshness) */
    int64_t  last_frame_ms;      /* last valid frame of any type (link liveness) */
    int64_t  last_linked_ms;     /* when the peer was last LINKED (0 = never) */
    int64_t  last_ping_ms;       /* cadence: when the last PING went out */
    int64_t  last_knock_ms;      /* cadence: when the last KNOCK went out */
    int64_t  ping_tx_ms;         /* outstanding PING (RTT sample), 0 = none */
    int64_t  knock_tx_ms;        /* outstanding KNOCK (RTT sample), 0 = none */
    int      rtt_ms;
    uint16_t latest;
    bool     have_latest;
    bool     head_valid;
    uint16_t head;
    uint8_t (*jbuf)[NP_AUDIO_OPUS];  /* PSRAM, compressed frames */
    opus_link_dec_t *opus_dec;       /* PSRAM, created on first audio frame */
    uint16_t *jbuf_seq;           /* same lazy PSRAM allocation as jbuf */
    bool     *jbuf_valid;
    uint32_t rx_msgs, tx_msgs, rx_drop;
    /* The sequence counter never resets between transmissions, so a new
     * stream's first packet is contiguous with the previous one's last.  The
     * play head may only be primed from packets of the CURRENT stream, or the
     * far end replays the previous call's tail (see peer_jbuf_next). */
    uint16_t stream_first;
    bool     have_first;
    uint32_t rx_late;   /* arrived after the play head had passed: dropped */
    uint32_t rx_plc;    /* lost frames Opus concealed */
    net_rx_order_t order; /* independent of playback-buffer lifetime */
} rt_peer_t;

static rt_peer_t s_peer[SLOT_MAX];
static SemaphoreHandle_t s_peer_mux;
static void peer_lock(void) { if (s_peer_mux) xSemaphoreTake(s_peer_mux, portMAX_DELAY); }
static void peer_unlock(void) { if (s_peer_mux) xSemaphoreGive(s_peer_mux); }
static volatile bool s_tx_stop;

/*
 * NVS must not be written from a task whose stack lives in PSRAM: a flash write
 * disables the cache, the stack becomes unreachable and the task faults (this
 * is what made the far end reboot when it received a BYE).  The engine task --
 * PSRAM stack, which is what stops ESP_ERR_NO_MEM when the engine is restarted
 * on a mode switch -- therefore only raises this flag; the httpd task, whose
 * stack is internal, performs the save on its next status poll.
 */
static volatile bool s_peers_dirty;

enum { F_AUDIO = 0, F_KNOCK = 1, F_KNOCK_ACK = 2, F_PING = 3, F_PONG = 4, F_BYE = 5 };

static bool frame_send_to(rt_peer_t *p, uint8_t type, uint16_t seq,
                          bool ptt, const uint8_t *audio);
static uint16_t s_tx_epoch;
static bool s_tx_epoch_open;

/* Persistent, local admission decisions. Remote KNOCK flags never grant consent. */
static np_access_record_t s_denied[NP_DENY_LIMIT];
static size_t s_denied_count;
static bool s_access_error;
static uint32_t s_drop_short, s_drop_full, s_drop_room, s_drop_self, s_drop_tomb, s_drop_psk;
static bool peer_addr_set(const rt_peer_t *p);
static void peer_record(const rt_peer_t *p, np_access_record_t *r)
{
    memset(r, 0, sizeof(*r));
    memcpy(r->peer.name, p->name, 8);
    if (!r->peer.name[0]) { snprintf(r->peer.name, 8, "peer"); }
    memcpy(r->mac, p->mac, 6);
    if (!p->via_hub) {
        r->peer.family = p->fam; r->peer.port = p->port;
        if (p->fam == 6) { memcpy(r->peer.address, p->ip6, 16); }
        else { put_be32(r->peer.address, p->ip); }
    }
}
static int denied_find(const uint8_t mac[6], const np_addr_t *src)
{
    uint8_t address[16] = {0};
    if (src) {
        if (src->fam == 6) { memcpy(address, src->ip6, 16); }
        else { put_be32(address, src->ip); }
    }
    for (size_t i = 0; i < s_denied_count; ++i) {
        if (np_access_match(&s_denied[i], mac, src ? src->fam : 0,
                            src ? address : NULL, src ? src->port : 0)) { return (int)i; }
    }
    return -1;
}
/* Only a local, confirmed address add may use address matching to restore a known MAC. */
static int denied_address(const np_addr_t *a)
{
    for (size_t i = 0; i < s_denied_count; ++i) {
        np_access_record_t r = s_denied[i];
        memset(r.mac, 0, 6);
        uint8_t address[16] = {0};
        if (a->fam == 6) { memcpy(address, a->ip6, 16); }
        else { put_be32(address, a->ip); }
        if (np_access_match(&r, NULL, a->fam, address, a->port)) { return (int)i; }
    }
    return -1;
}
static void denied_remove(size_t i)
{
    if (i < s_denied_count) {
        memmove(&s_denied[i], &s_denied[i + 1], (s_denied_count - i - 1) * sizeof(s_denied[0]));
        memset(&s_denied[--s_denied_count], 0, sizeof(s_denied[0]));
    }
}

static int64_t s_last_tx_ms;
/* Carrier state from the far end's PTT flag (see netrelay.c). */
static volatile bool    s_rx_ptt;
static volatile int64_t s_rx_ptt_ms;
static volatile int64_t s_rx_hang_ms;      /* unkey pending since (0 = none) */
static uint32_t s_tx_msgs, s_tx_errs;
static int      s_last_send_rc = -999;  /* lwip_sendto of the last frame */
static char     s_last_del[72] = "";    /* last delete: peer, fam, result */
static uint32_t s_bye_rx;              /* BYE frames received */

static volatile bool        s_started;
static volatile bool s_play_stop;
static TaskHandle_t s_play_task;
static TaskHandle_t s_eng_task;
static TaskHandle_t s_stun_task;
static bool        s_amp_on;
static bool        s_crc_init;
static uint8_t     s_my_mac[6];
static bool        s_hub_on;

/* --------------------------------------------------------------- NVS config */

#define NVS_NS     "orl"
#define KEY_PEERS  "np_peers"
#define KEY_HUB    "nphub"
#define KEY_STUN   "np_stun"

#define PEER_BLOB_ENTRY  15   /* v1: name(8) + ip(4) + port(2)              */
#define PEER_BLOB_LEN    (PEER_MAX * PEER_BLOB_ENTRY)
/* v2 adds the family and a 16-byte address so IPv6 peers survive a reboot.
 * It lives under its own key; the v1 blob is migrated on first load. */
#define KEY_PEERS2       "np_peer2"
#define KEY_PEERS3       "np_peer3"
#define KEY_ACCESS       "np_state1"
#define PEER_BLOB2_ENTRY 27   /* name(8) + fam(1) + addr(16) + port(2)      */
#define PEER_BLOB2_LEN   (PEER_MAX * PEER_BLOB2_ENTRY)

static bool stun_set_inner(const char *host_port);
static void peer_clear(rt_peer_t *p);

static void peers_load(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) { s_access_error = err != ESP_ERR_NVS_NOT_FOUND; return; }
    /* Keep policy snapshots and encoded blobs off the HTTP/engine task stacks. */
    np_access_state_t *state = heap_caps_malloc(sizeof(*state), MALLOC_CAP_SPIRAM);
    uint8_t *blob = heap_caps_malloc(NP_ACCESS_BYTES, MALLOC_CAP_SPIRAM);
    if (!state || !blob) { s_access_error = true; goto done; }
    size_t len = NP_ACCESS_BYTES;
    err = nvs_get_blob(h, KEY_ACCESS, blob, &len);
    if (err == ESP_OK) {
        if (!np_access_decode(blob, len, state)) { s_access_error = true; goto done; }
        s_denied_count = state->denied_count;
        memcpy(s_denied, state->denied, sizeof(s_denied));
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* One-time migration. Never fall back past an unreadable new policy. */
        memset(state, 0, sizeof(*state));
        np_peer_record_t records[PEER_MAX];
        const char *keys[] = {KEY_PEERS3, KEY_PEERS2, KEY_PEERS};
        const unsigned versions[] = {3, 2, 1};
        for (unsigned f = 0; f < 3; ++f) {
            len = NP_ACCESS_BYTES; size_t count = 0;
            if (nvs_get_blob(h, keys[f], blob, &len) != ESP_OK ||
                !np_peer_store_decode(blob, len, versions[f], records, &count)) { continue; }
            state->peer_count = count;
            for (size_t i = 0; i < count; ++i) { state->peers[i].peer = records[i]; }
            break;
        }
        s_peers_dirty = true;
    } else { s_access_error = true; goto done; }
    for (size_t i = 0; i < state->peer_count; ++i) {
        rt_peer_t *p = &s_peer[i]; const np_access_record_t *r = &state->peers[i];
        peer_clear(p); p->in_use = p->manual = true;
        memcpy(p->name, r->peer.name, 8); memcpy(p->mac, r->mac, 6);
        p->fam = r->peer.family; p->port = r->peer.port;
        if (p->fam == 6) { memcpy(p->ip6, r->peer.address, 16); }
        else { p->ip = be32(r->peer.address); }
        if (!peer_addr_set(p)) { peer_clear(p); s_peers_dirty = true; }
    }
done:
    heap_caps_free(state); heap_caps_free(blob);
    uint8_t hub = 0;
    if (nvs_get_u8(h, KEY_HUB, &hub) == ESP_OK) { s_hub_on = (hub != 0); }
    char stun[128] = {0}; size_t slen = sizeof(stun);
    if (nvs_get_str(h, KEY_STUN, stun, &slen) == ESP_OK) { (void)stun_set_inner(stun); }
    nvs_close(h);
}
/* A single NVS blob contains BOTH admitted peers and denied identities. */
static esp_err_t peers_save_ex(const rt_peer_t *exclude, const rt_peer_t *prefer)
{
    if (s_access_error) { return ESP_ERR_INVALID_STATE; }
    np_access_state_t *state = heap_caps_malloc(sizeof(*state), MALLOC_CAP_SPIRAM);
    uint8_t *blob = heap_caps_malloc(NP_ACCESS_BYTES, MALLOC_CAP_SPIRAM);
    esp_err_t err = ESP_ERR_NO_MEM;
    if (!state || !blob) { goto done; }
    memset(state, 0, sizeof(*state));
    for (int i = 0; i < PEER_MAX; ++i) {
        const rt_peer_t *p = &s_peer[i];
        if (p == exclude || !p->in_use || p->selftest || p->via_hub || !peer_addr_set(p)) { continue; }
        if (prefer && p != prefer && p->fam == prefer->fam && p->port == prefer->port &&
            (p->fam == 6 ? !memcmp(p->ip6, prefer->ip6, 16) : p->ip == prefer->ip)) { continue; }
        peer_record(p, &state->peers[state->peer_count++]);
    }
    state->denied_count = s_denied_count;
    memcpy(state->denied, s_denied, sizeof(s_denied));
    size_t size = np_access_encode(state, blob, NP_ACCESS_BYTES);
    nvs_handle_t h;
    err = size ? nvs_open(NVS_NS, NVS_READWRITE, &h) : ESP_ERR_INVALID_ARG;
    if (err == ESP_OK) {
        /* Legacy keys remain intact for deliberate rollback, never read over the new key. */
        err = nvs_set_blob(h, KEY_ACCESS, blob, size);
        if (err == ESP_OK) { err = nvs_set_u8(h, KEY_HUB, s_hub_on ? 1 : 0); }
        if (err == ESP_OK) { err = nvs_set_str(h, KEY_STUN, s_stun_str); }
        if (err == ESP_OK) { err = nvs_commit(h); }
        nvs_close(h);
    }
done:
    heap_caps_free(state); heap_caps_free(blob);
    s_peers_dirty = err != ESP_OK;
    if (err != ESP_OK) { ESP_LOGW(TAG, "peer/policy save pending: %s", esp_err_to_name(err)); }
    return err;
}
static void peers_save(void) { (void)peers_save_ex(NULL, NULL); }

/* ------------------------------------------------------------ address utils */

static bool np_addr_set(const np_addr_t *a)
{
    if (a->fam == 6) {
        for (int i = 0; i < 16; ++i) {
            if (a->ip6[i]) { return true; }
        }
        return false;
    }
    return a->ip != 0;
}

static bool peer_addr_set(const rt_peer_t *p)
{
    if (p->fam == 6) {
        for (int i = 0; i < 16; ++i) {
            if (p->ip6[i]) { return true; }
        }
        return false;
    }
    return p->ip != 0;
}

static void peer_take_addr(rt_peer_t *p, const np_addr_t *a)
{
    p->fam = a->fam;
    p->ip = a->ip;
    memcpy(p->ip6, a->ip6, sizeof(p->ip6));
    p->port = a->port;
}

/* "1.2.3.4" or "2001:db8::1" */
static void peer_addr_text(const rt_peer_t *p, char *buf, size_t n)
{
    if (p->fam == 6) {
        struct in6_addr a;
        memcpy(&a, p->ip6, sizeof(a));
        if (!inet_ntop(AF_INET6, &a, buf, n)) {
            snprintf(buf, n, "?");
        }
    } else if (p->ip) {
        struct in_addr a;
        a.s_addr = p->ip;
        inet_ntoa_r(a, buf, n);
    } else {
        snprintf(buf, n, "0.0.0.0");
    }
}

/* Send `len` bytes to a peer on the socket that matches its family. */
static int peer_send_raw(const rt_peer_t *p, const uint8_t *pkt, int len)
{
    if (!peer_addr_set(p)) {
        return -1;
    }
    /*
     * A port-rewriting NAT gives every flow its own external port: the address
     * the peer advertises (STUN) is not the address its packets come from, and
     * replies sent to the advertised one are dropped by the peer's NAT.  Reply
     * to the 5-tuple the last authenticated frame arrived on while it is fresh
     * (NAT mappings live ~30-120 s), otherwise fall back to the configured
     * address for the next knock.
     */
    const bool use_rx = (p->rx_src_ms > 0 && now_ms() - p->rx_src_ms < 45000 &&
                         np_addr_set(&p->rx_src));
    if (use_rx) {
        if (p->rx_src.fam == 6) {
            if (s_sock6 < 0) {
                return -1;
            }
            struct sockaddr_in6 dst;
            memset(&dst, 0, sizeof(dst));
            dst.sin6_family = AF_INET6;
            memcpy(&dst.sin6_addr, p->rx_src.ip6, sizeof(dst.sin6_addr));
            dst.sin6_port = htons(p->rx_src.port);
            return lwip_sendto(s_sock6, pkt, len, 0,
                               (struct sockaddr *)&dst, sizeof(dst));
        }
        if (s_sock_main < 0) {
            return -1;
        }
        struct sockaddr_in dst;
        memset(&dst, 0, sizeof(dst));
        dst.sin_family = AF_INET;
        dst.sin_addr.s_addr = p->rx_src.ip;
        dst.sin_port = htons(p->rx_src.port);
        return lwip_sendto(s_sock_main, pkt, len, 0,
                           (struct sockaddr *)&dst, sizeof(dst));
    }
    if (p->fam == 6) {
        if (s_sock6 < 0) {
            return -1;
        }
        struct sockaddr_in6 dst;
        memset(&dst, 0, sizeof(dst));
        dst.sin6_family = AF_INET6;
        memcpy(&dst.sin6_addr, p->ip6, sizeof(dst.sin6_addr));
        dst.sin6_port = htons(p->port);
        return lwip_sendto(s_sock6, pkt, len, 0,
                           (struct sockaddr *)&dst, sizeof(dst));
    }
    if (s_sock_main < 0) {
        return -1;
    }
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = p->ip;
    dst.sin_port = htons(p->port);
    return lwip_sendto(s_sock_main, pkt, len, 0,
                       (struct sockaddr *)&dst, sizeof(dst));
}

/* -------------------------------------------------------------- peer lookup */

static rt_peer_t *peer_by_mac(const uint8_t mac[6])
{
    for (int i = 0; i < SLOT_MAX; ++i) {
        if (s_peer[i].in_use && memcmp(s_peer[i].mac, mac, 6) == 0) {
            return &s_peer[i];
        }
    }
    return NULL;
}

static rt_peer_t *peer_free_slot(void)
{
    for (int i = 0; i < PEER_MAX; ++i) {   /* the last slot is the self-test slot */
        if (!s_peer[i].in_use) { return &s_peer[i]; }
    }
    return NULL;
}

/* An authenticated datagram's transport can be the hub rather than its origin.
 * Never bind a forwarded origin MAC to a configured hub's address. */
static rt_peer_t *peer_by_source(const np_addr_t *src)
{
    for (int i = 0; i < PEER_MAX; ++i) {
        rt_peer_t *p = &s_peer[i];
        if (!p->in_use || p->selftest || p->via_hub) { continue; }
        if (p->rx_src_ms > 0 && now_ms() - p->rx_src_ms < 45000
            && p->rx_src.fam == src->fam && p->rx_src.port == src->port
            && (src->fam == 6 ? !memcmp(p->rx_src.ip6, src->ip6, 16) : p->rx_src.ip == src->ip)) {
            return p;
        }
        if (p->fam == src->fam && p->port == src->port
            && (src->fam == 6 ? !memcmp(p->ip6, src->ip6, 16) : p->ip == src->ip)) {
            return p;
        }
    }
    return NULL;
}

/* Caller holds the peer mutex. Audio storage is created on first audio and
 * released with the slot, so empty capacity consumes no jitter-buffer RAM. */
static void peer_audio_free(rt_peer_t *p)
{
    opus_link_dec_free(p->opus_dec);
    p->opus_dec = NULL;
    heap_caps_free(p->jbuf);
    p->jbuf = NULL; p->jbuf_seq = NULL; p->jbuf_valid = NULL;
}
static void peer_clear(rt_peer_t *p)
{
    peer_audio_free(p);
    memset(p, 0, sizeof(*p));
}
/* Allocated on first audio, never for idle configured peers. Caller holds the
 * peer mutex; the playout task uses the same mutex before reading/freeing it. */
static bool peer_jbuf_alloc(rt_peer_t *p)
{
    if (!p->jbuf) {
        const size_t audio = JBUF_SLOTS * NP_AUDIO_OPUS;
        uint8_t *data = heap_caps_malloc(audio + JBUF_SLOTS * (sizeof(uint16_t) + sizeof(bool)), MALLOC_CAP_SPIRAM);
        if (!data) { return false; }
        p->jbuf = (uint8_t (*)[NP_AUDIO_OPUS])data;
        p->jbuf_seq = (uint16_t *)(data + audio);
        p->jbuf_valid = (bool *)(data + audio + JBUF_SLOTS * sizeof(uint16_t));
        memset(p->jbuf_seq, 0, JBUF_SLOTS * sizeof(uint16_t));
        memset(p->jbuf_valid, 0, JBUF_SLOTS * sizeof(bool));
    }
    return p->jbuf != NULL;
}

/* Find or create a runtime slot for a sender MAC.  A configured peer (known
 * by ip:port from NVS) has no MAC until the first frame: when the source
 * address matches such a slot we bind the MAC to it instead of creating a
 * duplicate. New senders are refused when the table is full. */
static bool ip4_is_private(uint32_t be_addr)
{
    const uint32_t a = ntohl(be_addr);
    if ((a & 0xFF000000u) == 0x0A000000u) { return true; }        /* 10/8 */
    if ((a & 0xFFF00000u) == 0xAC100000u) { return true; }        /* 172.16/12 */
    if ((a & 0xFFFF0000u) == 0xC0A80000u) { return true; }        /* 192.168/16 */
    return false;
}

/*
 * A peer's stored address can go stale: a manually added public address (or an
 * address learned earlier over the internet) stays in the table for ever, so
 * every reply -- including the BYE that tells it we deleted it -- is sent to an
 * address that no longer reaches it.  On an authenticated frame, adopt the
 * source address when it is clearly better: always for IPv6, and for IPv4 when
 * the stored address is public but the frame actually came from the LAN.
 */

static rt_peer_t *peer_ensure(const uint8_t mac[6], const np_addr_t *src)
{
    rt_peer_t *p = peer_by_mac(mac);
    if (p) { return p; }
    /*
     * A peer that was entered by hand has no MAC yet.  Its packets can arrive
     * from a different port than configured (NAT rewrites the source port), so
     * match on the address alone and bind the MAC here -- otherwise every
     * manual peer produced a second, auto-learned twin and deleting one half
     * left the other connected.
     */
    if (src && np_addr_set(src)) {
        for (int i = 0; i < SLOT_MAX; ++i) {
            rt_peer_t *c = &s_peer[i];
            if (!c->in_use || c->selftest) { continue; }
            if (c->mac[0] || c->mac[1] || c->mac[2] || c->mac[3] || c->mac[4] ||
                c->mac[5]) {
                continue;                       /* already bound to a MAC */
            }
            if (!peer_addr_set(c)) { continue; }
            if (c->fam == src->fam) {
                if (src->fam == 6) {
                    if (memcmp(c->ip6, src->ip6, 16) != 0) { continue; }
                } else if (c->ip != src->ip) {
                    continue;
                }
            } else {
                continue;
            }
            memcpy(c->mac, mac, 6);
            c->port = src->port;                /* adopt the NAT port */
            s_peers_dirty = true; /* Persist the newly learned identity. */
            ESP_LOGI(TAG, "peer '%s' bound to incoming mac (%u)",
                     c->name, (unsigned)src->port);
            return c;
        }
    }
    if (src && np_addr_set(src) && src->port) {
        for (int i = 0; i < SLOT_MAX; ++i) {
            rt_peer_t *c = &s_peer[i];
            if (!c->in_use || c->selftest) { continue; }
            if (c->port != src->port) { continue; }
            if (c->fam == 6 || src->fam == 6) {
                if (c->fam != src->fam || memcmp(c->ip6, src->ip6, 16) != 0) { continue; }
            } else if (c->ip != src->ip) {
                continue;
            }
            /* Same address as a slot we already hold: keep that slot instead of
             * growing a second one.  A slot with no MAC yet, or a not-yet-linked
             * slot whose MAC changed (peer restarted), simply takes the new MAC;
             * otherwise the frame belongs to this slot as it is. */
            if (!c->mac[0] || !c->linked || memcmp(c->mac, mac, 6) == 0) {
                memcpy(c->mac, mac, 6);
                s_peers_dirty = true;
                return c;
            }
        }
    }
    p = peer_free_slot();
    if (!p) { return NULL; }
    peer_clear(p);
    p->in_use = true;
    memcpy(p->mac, mac, 6);
    /*
     * Name a slot learned from traffic after its address instead of leaving it
     * blank.  A blank name is not just ugly in the panel: peers_save() writes
     * the table out and peers_load() SKIPS entries whose name is empty, so a
     * peer learned this way vanished on the next reboot and had to be learned
     * again -- which is how a peer table ends up full of "(空)" rows.
     */
    if (src && np_addr_set(src)) {
        /* The name is 8 bytes, so a whole address does not fit.  The panel shows
         * the address anyway; the useful part as a label is the last field. */
        if (src->fam == 6) {
            char t[46] = {0};
            struct in6_addr a6;
            memcpy(&a6, src->ip6, sizeof(a6));
            if (inet_ntop(AF_INET6, &a6, t, sizeof(t))) {
                const char *colon = strrchr(t, ':');
                snprintf(p->name, sizeof(p->name), "%.7s",
                         colon ? colon + 1 : t);
            }
        } else {
            const uint32_t ip_h = ntohl(src->ip);
            snprintf(p->name, sizeof(p->name), "%u", (unsigned)(ip_h & 0xFFu));
        }
        p->manual = false;
        s_peers_dirty = true;
    }
    return p;
}

static rt_peer_t *peer_for_frame(const uint8_t mac[6], const np_addr_t *src,
                                uint8_t type, uint8_t flags)
{
    rt_peer_t *upstream = type == F_AUDIO ? peer_by_source(src) : NULL;
    rt_peer_t *origin = peer_by_mac(mac);
    const bool own_transport = origin && !origin->via_hub && origin->fam == src->fam
        && origin->port == src->port && (src->fam == 6
            ? !memcmp(origin->ip6, src->ip6, 16) : origin->ip == src->ip);
    const bool relayed = type == F_AUDIO && ((flags & NP_FLAG_FORWARDED)
        || (!own_transport && upstream && upstream->linked && memcmp(upstream->mac, mac, 6)));
    rt_peer_t *p;
    if (relayed) {
        if (!upstream || !upstream->linked || !memcmp(upstream->mac, mac, 6)) { return NULL; }
        upstream->hub = true; /* Also recognises an older hub without role flags. */
        p = peer_by_mac(mac);
        /* A configured direct route wins; don't mix direct/relayed copies. */
        if (p && !p->via_hub && peer_addr_set(p)) { return NULL; }
        if (!p) {
            p = peer_free_slot();
            if (!p) { ++s_drop_full; return NULL; }
            peer_clear(p);
            p->in_use = p->via_hub = true;
            p->rtt_ms = -1; /* No end-to-end RTT was measured for this origin. */
            memcpy(p->mac, mac, 6);
            snprintf(p->name, sizeof(p->name), "%02x%02x%02x", mac[3], mac[4], mac[5]);
        }
        memcpy(p->hub_mac, upstream->mac, 6);
        p->linked = true;
        p->last_linked_ms = now_ms();
        return p;
    }
    p = peer_ensure(mac, src);
    if (!p) { ++s_drop_full; }
    if (p) {
        if (p->via_hub) {
            p->via_hub = false;
            memset(p->hub_mac, 0, sizeof(p->hub_mac));
            peer_take_addr(p, src);
            s_peers_dirty = true;
        }
        if (flags & (NP_FLAG_ROLE | NP_FLAG_HUB)) { p->hub = (flags & NP_FLAG_HUB) != 0; }
    }
    return p;
}
static void peer_reset_stream(rt_peer_t *p)
{
    if (p->jbuf_seq) { memset(p->jbuf_seq, 0, JBUF_SLOTS * sizeof(uint16_t)); }
    if (p->jbuf_valid) { memset(p->jbuf_valid, 0, JBUF_SLOTS * sizeof(bool)); }
    p->have_latest = false;
    p->head_valid = false;
    p->head = 0;
    p->stream_first = 0;
    p->have_first = false;
    opus_link_dec_free(p->opus_dec);
    p->opus_dec = NULL;
}

/* ------------------------------------------------------------ frame builder */

static bool frame_send_to(rt_peer_t *p, uint8_t type, uint16_t seq,
                          bool ptt, const uint8_t *audio)
{
    if (!peer_addr_set(p)) {
        return true;   /* nothing to send to yet; not an error */
    }
    uint8_t pkt[NP_AUDIO_LEN];
    np_header_t *h = (np_header_t *)pkt;
    h->magic = NP_MAGIC;
    h->room = netcom_room();
    h->seq = seq;
    memcpy(h->mac, s_my_mac, 6);
    /* Keep bit 1 for old peers' wire compatibility. It is never consent to
     * override a local persistent refusal on this firmware. */
    h->flags = (uint8_t)((ptt ? 1 : 0) | ((type == F_KNOCK && p->manual) ? 2 : 0)
        | NP_FLAG_ROLE | (s_hub_on ? NP_FLAG_HUB : 0));
    h->type = type;
    psk_compute(h->room, s_my_mac, &pkt[NP_HDR_LEN]);
    uint8_t *pl = &pkt[NP_CTR_BASE];
    /* nat_hint: 0 unknown, 1 port-preserving, 2 NOT port-preserving.
     * s_port_preserving is -1 unknown / 1 yes / 2 no: the old `> 0` test
     * collapsed "2 = not preserving" onto the "1 = preserving" wire value. */
    pl[0] = (s_port_preserving == 1) ? 1 : (s_port_preserving == 2 ? 2 : 0);
    pl[1] = 1;                      /* ver */
    int len;
    if (type == F_AUDIO) {
        h->flags |= NET_RX_FLAG_EPOCH;
        put_be16(pl, s_tx_epoch); /* audio does not consume NAT hints */
        memcpy(pl + 2, audio, NP_AUDIO_OPUS);
        len = NP_AUDIO_LEN;
    } else if (type == F_KNOCK || type == F_KNOCK_ACK) {
        /* s_pub_ip is kept in network byte order (like every other address in
         * this file); the wire wants the plain numeric value. */
        put_be32(pl + 2, ntohl(s_pub_ip));
        put_be16(pl + 6, s_pub_port);
        len = NP_CTR_KNOCK_LEN;
    } else {
        len = NP_CTR_PING_LEN;
    }
    const int sent = peer_send_raw(p, pkt, len);
    s_last_send_rc = sent;
    if (sent >= 0) {
        p->tx_msgs++;
    }
    if (sent == len && type == F_AUDIO && ptt) {
        radio_led_note_net_tx(NETCOM_MODE_NETP2P);
    }
    return sent >= 0;
}

/* --------------------------------------------------------------- receive side */
static bool jbuf_audio(rt_peer_t *p, uint16_t seq, const uint8_t *audio,
                       size_t audio_len, bool ptt, uint16_t epoch)
{
    if (audio_len != NP_AUDIO_OPUS || !peer_jbuf_alloc(p)) { p->rx_drop++; return false; }
    net_rx_order_t next = p->order;
    const int accept = net_rx_order_accept(&next, epoch, seq, ptt, now_ms());
    if (accept == NET_RX_DROP) {
        /* Keep a legacy restart probe, but never its tentative PTT. */
        p->order.probe_count = next.probe_count; p->order.probe_seq = next.probe_seq;
        p->order.probe_ms = next.probe_ms;
        ++p->rx_late; return false;
    }
    const bool continuing = p->order.seen && p->order.epoch == epoch && p->order.ptt;
    const bool pending = p->have_latest && (!p->head_valid || (int16_t)(p->head - p->latest) <= 0);
    if (continuing) { next.audio |= p->order.audio; }
    if ((accept == NET_RX_NEW || (p->have_latest && now_ms() - p->last_rx_ms > STREAM_IDLE_MS)) &&
        !(continuing && pending)) { peer_reset_stream(p); }
    if (p->have_latest) {
        const int16_t fwd = (int16_t)(seq - p->latest);
        if (fwd < 0 && -fwd >= JBUF_SLOTS / 2) { p->rx_drop++; return false; }
        if (fwd > 0 && fwd >= JBUF_SLOTS / 2) { peer_reset_stream(p); }
    }
    if (p->head_valid && (int16_t)(seq - p->head) < 0) {
        ++p->rx_late;
        return false;
    }
    const uint16_t slot = seq % JBUF_SLOTS;
    if (p->jbuf_valid[slot] && p->jbuf_seq[slot] == seq) { ++p->rx_late; return false; }
    p->order = next;
    if (!p->have_latest) { p->stream_first = seq; p->have_first = true; }
    if (!p->have_latest || (int16_t)(seq - p->latest) > 0) { p->latest = seq; }
    p->have_latest = true;
    p->last_rx_ms = now_ms();
    p->rx_msgs++;
    memcpy(p->jbuf[slot], audio, NP_AUDIO_OPUS);
    p->jbuf_seq[slot] = seq;
    p->jbuf_valid[slot] = true;
    if (ptt && !p->selftest) { radio_led_note_net_rx(NETCOM_MODE_NETP2P); }
    return true;
}

/* A peer becomes LINKED once its PSK is validated (we heard an acceptable
 * frame from it).  For a configured (NAT) peer the stored public address
 * already is the one to reply to; for a freshly-encountered peer (a hub
 * member who knocked us) we learn the reply address from the source. */
static void link_peer(rt_peer_t *p, const np_addr_t *src)
{
    if (!peer_addr_set(p) && src && np_addr_set(src)) {
        peer_take_addr(p, src);
    }
    if (!p->linked) {
        p->linked = true;
        p->last_linked_ms = now_ms();
        p->last_frame_ms = now_ms();
        /* KNOCK -> KNOCK_ACK round trip is the first real RTT sample.  Use the
         * dedicated ping/knock sample timestamps, never the cadence timers:
         * resetting a cadence timer here made the periodic sender fire again on
         * the very next loop pass, which turned the heartbeat into a storm. */
        if (p->knock_tx_ms > 0) {
            const int64_t rtt = now_ms() - p->knock_tx_ms;
            if (rtt > 0 && rtt < 60000) {
                p->rtt_ms = (int)rtt;
            }
            p->knock_tx_ms = 0;
        }
        char ipb[48] = "0.0.0.0";
        peer_addr_text(p, ipb, sizeof(ipb));
        ESP_LOGI(TAG, "LINKED peer '%s' %s:%u (rtt %d ms)", p->name, ipb,
                 (unsigned)p->port, p->rtt_ms);
    }
}

static void handle_control(rt_peer_t *p, const uint8_t *pkt, int len,
                           const np_addr_t *src)
{
    const uint8_t type = pkt[13];
    switch (type) {
    case F_KNOCK:    /* we are the target: open the hole and answer */
        link_peer(p, src);
        (void)frame_send_to(p, F_KNOCK_ACK, 0, false, NULL);
        if (len >= NP_CTR_KNOCK_LEN) {
            const uint32_t pub_ip = htonl(be32(pkt + NP_CTR_BASE + 2));
            const uint16_t pub_port = be16(pkt + NP_CTR_BASE + 6);
            if (pub_ip && pub_port) {
                struct in_addr a;
                a.s_addr = pub_ip;
                char ipb[20];
                inet_ntoa_r(a, ipb, sizeof(ipb));
                /*
                 * The advert is the sender's current NAT mapping, which is what
                 * we must aim at when *we* want to reach it.  Without adopting
                 * it, a mapping that changed while the engine was stopped (a
                 * mode switch restarts it) leaves every later knock going to a
                 * dead address -- "IPv4 works once, then never again".
                 *
                 * It must never replace a LAN address, though.  Two boards
                 * behind one router share a public address, so the advert is
                 * that board's own mapping on the router we are both behind,
                 * not an address we can reach it on.  Adopting it turned
                 * 192.0.2.198 into 222.x.x.x:12576; peer_send_raw() replies to
                 * the last source for 45 s before falling back to the stored
                 * address, so the link worked for a minute or two and then died
                 * -- the "connected, then 已过期 a few minutes later" report on a
                 * LAN with no NAT to traverse at all.  A peer heard on the LAN
                 * keeps the LAN address it is speaking from.
                 */
                const bool lan_src = (src->fam == 4) && ip4_is_private(src->ip);
                const bool lan_stored = (p->fam == 4) && p->ip && ip4_is_private(p->ip);
                if (lan_src) {
                    if (p->fam != 6 && (p->ip != src->ip || p->port != src->port)) {
                        struct in_addr la;
                        la.s_addr = src->ip;
                        char lanb[20];
                        inet_ntoa_r(la, lanb, sizeof(lanb));
                        ESP_LOGI(TAG, "peer '%s' re-anchored to LAN %s:%u "
                                 "(ignoring public advert %s:%u)", p->name, lanb,
                                 (unsigned)src->port, ipb, (unsigned)pub_port);
                        p->fam = 4;
                        p->ip = src->ip;
                        p->port = src->port;
                    }
                } else if (lan_stored) {
                    /* Hairpinned through the router: the stored address already
                     * works, so a public advert must not replace it. */
                    ESP_LOGI(TAG, "peer '%s' keeps LAN address, advert %s:%u ignored",
                             p->name, ipb, (unsigned)pub_port);
                } else if (p->fam != 6 && (p->ip != pub_ip || p->port != pub_port) &&
                           !ip4_is_private(pub_ip)) {
                    ESP_LOGI(TAG, "peer '%s' public mapping -> %s:%u (was %u)",
                             p->name, ipb, (unsigned)pub_port, (unsigned)p->port);
                    p->ip = pub_ip;
                    p->port = pub_port;
                } else {
                    ESP_LOGI(TAG, "knock from '%s' advertises public %s:%u",
                             p->name, ipb, (unsigned)pub_port);
                }
            }
        }
        break;
    case F_KNOCK_ACK:   /* our knock was accepted, the hole is open */
        link_peer(p, src);
        break;
    case F_PING:
        (void)frame_send_to(p, F_PONG, 0, false, NULL);
        break;
    case F_PONG:
        if (p->ping_tx_ms > 0) {
            const int64_t rtt = now_ms() - p->ping_tx_ms;
            if (rtt > 0 && rtt < 60000) {
                p->rtt_ms = (int)rtt;
            }
            p->ping_tx_ms = 0;
        }
        break;
    case F_BYE:
        /*
         * The other end deleted this peer.  Merely unlinking it left our entry
         * alive, so we kept knocking and re-established the link -- which made
         * the peer impossible to remove.  BYE only means "goodbye for good"
         * (it is sent on delete and on factory reset), so drop the entry here
         * too; that is what makes the deletion synchronous in both directions.
         */
        ++s_bye_rx;
        char ipb[48] = "?";
        peer_addr_text(p, ipb, sizeof(ipb));
        ESP_LOGW(TAG, "BYE from '%s' (%s:%u) -- peer removed on this side too",
                 p->name, ipb, (unsigned)p->port);
        peer_clear(p);
        s_peers_dirty = true;      /* saved by the httpd task, see the flag doc */
        break;
    default:
        break;
    }
}
/* Caller holds the peer mutex; preserve the original sender and packet. */
static void hub_forward_audio(const uint8_t sender[6], const uint8_t *pkt, size_t len, bool keyed)
{
    if (s_access_error || denied_find(sender, NULL) >= 0 || !s_hub_on || len < NP_HDR_LEN || len > NP_AUDIO_LEN
        || (pkt[12] & NP_FLAG_FORWARDED)) { return; }
    uint8_t forwarded[NP_AUDIO_LEN];
    memcpy(forwarded, pkt, len);
    forwarded[12] |= NP_FLAG_FORWARDED;
    for (int i = 0; i < PEER_MAX; ++i) {
        rt_peer_t *p = &s_peer[i];
        if (!p->in_use || p->selftest || p->via_hub || p->hub || !p->linked || !peer_addr_set(p) ||
            memcmp(p->mac, sender, 6) == 0) { continue; }
        if (denied_find(p->mac, NULL) >= 0) { continue; }
        const int sent = peer_send_raw(p, forwarded, (int)len);
        if (sent >= 0) { ++p->tx_msgs; }
        if (sent == (int)len && keyed) { radio_led_note_net_tx(NETCOM_MODE_NETP2P); }
    }
}

static bool frame_admitted(const uint8_t mac[6], const np_addr_t *src, uint8_t type, uint8_t flags)
{
    rt_peer_t *transport = peer_by_source(src);
    const bool relayed = type == F_AUDIO && ((flags & NP_FLAG_FORWARDED) ||
        (transport && transport->linked && memcmp(transport->mac, mac, 6)));
    return !s_access_error && denied_find(mac, relayed ? NULL : src) < 0 &&
        !(relayed && transport && denied_find(transport->mac, src) >= 0);
}
static void handle_frame(const uint8_t *pkt, int len, const np_addr_t *src)
{
    if (len < NP_HDR_LEN + NP_CHECK_LEN + 2) { ++s_drop_short; return; }
    np_header_t h;
    memcpy(&h, pkt, sizeof(h));
    if (h.magic != NP_MAGIC) { ++s_drop_short; return; }
    if (h.room != netcom_room()) { ++s_drop_room; return; }
    if (memcmp(h.mac, s_my_mac, 6) == 0) { ++s_drop_self; return; }
    const int expected = h.type == F_AUDIO ? NP_AUDIO_LEN
        : (h.type == F_KNOCK || h.type == F_KNOCK_ACK) ? NP_CTR_KNOCK_LEN
        : (h.type == F_PING || h.type == F_PONG || h.type == F_BYE) ? NP_CTR_PING_LEN : 0;
    if (!expected || len != expected) { ++s_drop_short; return; }
    uint8_t want[NP_CHECK_LEN];
    psk_compute(h.room, h.mac, want);
    if (memcmp(want, pkt + NP_HDR_LEN, NP_CHECK_LEN) != 0) {
        ++s_drop_psk;
        return;
    }
    /* Check the original sender, including Hub-forwarded audio, before allocation/PTT. */
    if (!frame_admitted(h.mac, src, h.type, h.flags)) {
        ++s_drop_tomb;
        /* Converge old peers after an offline delete without relying on BYE.
         * Reply only to direct KNOCK, at its current source, never a relayed origin. */
        static int64_t last_refusal_ms;
        if (!s_access_error && h.type == F_KNOCK && now_ms() - last_refusal_ms >= 400) {
            last_refusal_ms = now_ms();
            rt_peer_t reply = {0}; reply.in_use = true; reply.fam = src->fam;
            reply.ip = src->ip; memcpy(reply.ip6, src->ip6, 16); reply.port = src->port;
            (void)frame_send_to(&reply, F_BYE, 0, false, NULL);
        }
        return;
    }
    rt_peer_t *p = peer_for_frame(h.mac, src, h.type, h.flags);
    if (!p) { return; }
    /* Allocation failure must not refresh RX PTT and key a silent transmitter. */
    if (h.type == F_AUDIO && !peer_jbuf_alloc(p)) { ++p->rx_drop; return; }
    p->last_frame_ms = now_ms();
    if (h.type == F_AUDIO && !p->via_hub) { link_peer(p, src); }
    const bool changed = p->rx_src.fam != src->fam ||
        (src->fam == 4 ? p->rx_src.ip != src->ip
                        : memcmp(p->rx_src.ip6, src->ip6, 16) != 0) ||
        p->rx_src.port != src->port;
    if (changed) {
        ESP_LOGI(TAG, "peer '%s' speaks from %s:%u (configured port %u)",
                 p->name, src->fam == 6 ? "ipv6" : "ipv4",
                 (unsigned)src->port, (unsigned)p->port);
    }
    p->rx_src = *src;
    p->rx_src_ms = now_ms();
    if (h.type == F_AUDIO) {
        if (p->last_rx_ms == 0 || now_ms() - p->last_rx_ms > STREAM_IDLE_MS) {
            ESP_LOGI(TAG, "audio from '%s' (seq %u, ptt %u)", p->name,
                     (unsigned)h.seq, (unsigned)(h.flags & 1));
        }
        const uint16_t epoch = (h.flags & NET_RX_FLAG_EPOCH) ? be16(pkt + NP_CTR_BASE) : 0;
        if (jbuf_audio(p, h.seq, pkt + NP_CTR_BASE + 2, NP_AUDIO_OPUS, (h.flags & 1) != 0, epoch)
            && !p->via_hub) { hub_forward_audio(h.mac, pkt, NP_AUDIO_LEN, (h.flags & 1) != 0); }
    } else {
        handle_control(p, pkt, len, src);
    }
}

/* ------------------------------------------------------------ STUN handling */

/* The fingerprint check zeros (and restores) the FP field in place, so the
 * caller passes a scratch buffer, not a const frame. */
static bool stun_response(uint8_t *pkt, int len)
{
    if (len < 20) { return false; }
    if (!stun_is_message(pkt, len)) { return false; }
    const uint16_t mtype = be16(pkt + 0);
    if (mtype != STUN_BINDING_RSP) { return false; }

    stun_pending_t *pd = NULL;
    if (s_pend_main.pending && memcmp(pkt + 8, s_pend_main.txid, STUN_TXID_LEN) == 0) {
        pd = &s_pend_main;
    } else if (s_pend_probe.pending && memcmp(pkt + 8, s_pend_probe.txid, STUN_TXID_LEN) == 0) {
        pd = &s_pend_probe;
    }
    if (!pd) { return true; }   /* a STUN reply we did not send: consumed */

    pd->pending = false;
    const uint16_t mlen = be16(pkt + 2);
    if (len < (int)(20 + mlen)) { return true; }

    /* Parse the attribute list; verify the Fingerprint (RFC 5389: CRC32 of
     * the message with the fingerprint value zeroed, ^ 0x5354554E) and read
     * the mapped address (XOR-MAPPED-ADDRESS 0x0020, fallback 0x0001). */
    uint32_t ip = 0;
    uint16_t port = 0;
    int off = 20;
    const int end = 20 + mlen;
    while (off + 4 <= end) {
        const uint16_t atype = be16(pkt + off);
        const uint16_t alen = be16(pkt + off + 2);
        if (off + 4 + (int)alen > end) { break; }
        uint8_t *val = pkt + off + 4;
        if (atype == STUN_ATTR_FINGERPRINT && alen == 4) {
            uint8_t saved[4];
            memcpy(saved, val, 4);
            memset(val, 0, 4);
            const uint32_t fp = crc32(pkt, end) ^ STUN_FP_CONST;
            memcpy(val, saved, 4);
            if (be32(val) != fp) {
                ESP_LOGW(TAG, "stun: fingerprint mismatch");
            }
        } else if ((atype == STUN_ATTR_XORMAPPED || atype == STUN_ATTR_MAPPED) &&
                   alen >= 8 && val[1] == 0x01) {      /* RFC 5389: byte 0 is
                                                        * reserved, byte 1 is
                                                        * the address family */
            if (atype == STUN_ATTR_XORMAPPED) {
                port = (uint16_t)(be16(val + 2) ^ 0x2112);
                ip = be32(val + 4) ^ STUN_MAGIC;
            } else {
                port = be16(val + 2);
                ip = be32(val + 4);
            }
        }
        off += 4 + alen;
    }
    if (!ip) { return true; }

    /* be32() above returned a big-endian number, i.e. the plain numeric value
     * of the address (222.182.13.242 is 0xDEB60DF2).  struct in_addr holds the
     * address in NETWORK byte order, so swap on this little-endian target --
     * storing the number directly prints the octets reversed. */
    ip = htonl(ip);

    const int64_t rtt = now_ms() - pd->sent_ms;
    if (pd->server_idx == -1) {        /* probe socket (local 54322) */
        s_probe_port_preserved = (port == UDP_PORT_PROBE);
        nat_preserving_update();
    } else {
        s_pub_ip = ip;
        s_pub_port = port;
        s_last_stun_ms = now_ms();
        s_last_rtt_ms = (rtt > 0 && rtt < 60000) ? (int)rtt : 0;
        s_main_port_preserved = (port == UDP_PORT_MAIN);
        nat_preserving_update();
        struct in_addr a;
        a.s_addr = ip;
        char ipb[20];
        inet_ntoa_r(a, ipb, sizeof(ipb));
        ESP_LOGI(TAG, "stun ok: public %s:%u rtt %lld ms (port-preserving=%d)",
                 ipb, (unsigned)port, (long long)rtt, s_port_preserving);
    }
    return true;
}

/* --------------------------------------------------------------- engine task */

static void engine_tick_timers(void)
{
    const int64_t now = now_ms();
    for (int i = 0; i < SLOT_MAX; ++i) {
        rt_peer_t *p = &s_peer[i];
        if (!p->in_use || p->selftest) { continue; }
        if (p->via_hub) {
            const rt_peer_t *upstream = peer_by_mac(p->hub_mac);
            if (p->linked && (!upstream || !upstream->linked
                || now - upstream->last_frame_ms > PEER_EXPIRE_MS
                || now - p->last_frame_ms > PEER_EXPIRE_MS)) {
                p->linked = false;
                peer_reset_stream(p);
            }
            continue; /* No direct KNOCK/PING to an indirect audio origin. */
        }
        const bool expired = p->linked && now - p->last_frame_ms > PEER_EXPIRE_MS;
        if (expired) {
            /*
             * A NAT mapping can be reclaimed while we are idle.  Demote the
             * peer back to KNOCKING instead of leaving a stale "linked" badge:
             * the UI must show 地址疑似过期 rather than a link that no longer
             * carries audio.  hub_quiet_ms() starts counting from the last
             * moment a peer was genuinely linked, which is what turns into the
             * "hub offline" hint for a member whose hub vanished.
             */
            ESP_LOGW(TAG, "peer '%s' quiet for %lld s -- back to KNOCKING; "
                     "drops: short=%u room=%u self=%u tomb=%u psk=%u "
                     "(peer rx=%d tx=%d drop=%d)",
                     p->name, (long long)((now - p->last_frame_ms) / 1000),
                     (unsigned)s_drop_short, (unsigned)s_drop_room,
                     (unsigned)s_drop_self, (unsigned)s_drop_tomb,
                     (unsigned)s_drop_psk, (int)p->rx_msgs, (int)p->tx_msgs,
                     (int)p->rx_drop);
            p->linked = false;
            peer_reset_stream(p);
        }
        if (!p->linked) {
            /* not linked (or the link went quiet for 90 s): re-knock. */
            if (now - p->last_knock_ms >= KNOCK_PERIOD_MS) {
                (void)frame_send_to(p, F_KNOCK, 0, false, NULL);
                p->last_knock_ms = now;
                p->knock_tx_ms = now;
            }
        } else {
            if (now - p->last_ping_ms >= PING_PERIOD_MS) {
                (void)frame_send_to(p, F_PING, 0, false, NULL);
                p->last_ping_ms = now;
                p->ping_tx_ms = now;
            }
        }
    }
}

static void engine_task(void *arg)
{
    (void)arg;
    /* Must hold a whole audio frame: the buffer was sized for the 8 kHz
     * frame (488 B) and silently truncated every 720 B frame at 12 kHz,
     * which showed up as "control frames arrive, audio never does". */
    uint8_t buf[NP_AUDIO_LEN];
    while (s_started) {
        fd_set rfds;
        FD_ZERO(&rfds);
        int maxfd = -1;
        if (s_sock_main >= 0) { FD_SET(s_sock_main, &rfds); maxfd = s_sock_main; }
        if (s_sock_probe >= 0 && s_sock_probe > maxfd) {
            FD_SET(s_sock_probe, &rfds); maxfd = s_sock_probe;
        } else if (s_sock_probe >= 0) {
            FD_SET(s_sock_probe, &rfds);
        }
        if (s_sock6 >= 0) {
            FD_SET(s_sock6, &rfds);
            if (s_sock6 > maxfd) { maxfd = s_sock6; }
        }
        struct timeval tv = { .tv_sec = 0, .tv_usec = 20000 };
        int n = (maxfd >= 0) ? select(maxfd + 1, &rfds, NULL, NULL, &tv) : 0;
        if (n > 0) {
            for (int sd = 0; sd <= maxfd && n > 0; ++sd) {
                if (sd != s_sock_main && sd != s_sock_probe && sd != s_sock6) { continue; }
                if (!FD_ISSET(sd, &rfds)) { continue; }
                struct sockaddr_storage ss;
                memset(&ss, 0, sizeof(ss));
                socklen_t slen = sizeof(ss);
                int r = lwip_recvfrom(sd, buf, sizeof(buf), 0,
                                      (struct sockaddr *)&ss, &slen);
                if (r <= 0) { n--; continue; }
                np_addr_t src;
                memset(&src, 0, sizeof(src));
                if (ss.ss_family == AF_INET6) {
                    const struct sockaddr_in6 *s6 = (const struct sockaddr_in6 *)&ss;
                    src.fam = 6;
                    memcpy(src.ip6, &s6->sin6_addr, sizeof(src.ip6));
                    src.port = ntohs(s6->sin6_port);
                } else if (ss.ss_family == AF_INET) {
                    const struct sockaddr_in *s4 = (const struct sockaddr_in *)&ss;
                    src.fam = 4;
                    src.ip = s4->sin_addr.s_addr;
                    src.port = ntohs(s4->sin_port);
                } else {
                    n--;
                    continue;
                }
                if (stun_is_message(buf, r)) {
                    (void)stun_response(buf, r);
                } else {
                    peer_lock();
                    handle_frame(buf, r, &src);
                    peer_unlock();
                }
                n--;
            }
        }
        peer_lock(); engine_tick_timers(); peer_unlock();
    }
    s_eng_task = NULL;
    vTaskDeleteWithCaps(NULL);
}

/* --------------------------------------------------------------- STUN task */

static bool stun_resolve(stun_srv_t *s)
{
    if (s->have_ip) { return true; }
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    struct addrinfo *res = NULL;
    if (lwip_getaddrinfo(s->host, NULL, &hints, &res) != 0 || !res) {
        return false;
    }
    bool ok = false;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        if (ai->ai_family == AF_INET &&
            ai->ai_addrlen >= (int)sizeof(struct sockaddr_in)) {
            s->ip4 = ((struct sockaddr_in *)ai->ai_addr)->sin_addr;
            s->have_ip = true;
            ok = true;
            break;
        }
    }
    lwip_freeaddrinfo(res);
    return ok;
}

static void stun_send(stun_pending_t *pd, int sock, const stun_srv_t *s,
                      bool is_probe, int server_idx)
{
    if (sock < 0) { return; }
    uint8_t req[28];
    memset(req, 0, sizeof(req));
    put_be16(req + 0, STUN_BINDING_REQ);
    put_be16(req + 2, 8);                 /* one attribute: fingerprint */
    put_be32(req + 4, STUN_MAGIC);
    esp_fill_random(pd->txid, STUN_TXID_LEN);
    memcpy(req + 8, pd->txid, STUN_TXID_LEN);
    put_be16(req + 20, STUN_ATTR_FINGERPRINT);
    put_be16(req + 22, 4);
    put_be32(req + 24, crc32(req, 28) ^ STUN_FP_CONST);

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    if (s->have_ip) {
        dst.sin_addr = s->ip4;
    }
    put_be16((uint8_t *)&dst.sin_port, s->port);
    if (lwip_sendto(sock, req, sizeof(req), 0, (struct sockaddr *)&dst,
                    sizeof(dst)) < 0) {
        ESP_LOGW(TAG, "stun: sendto %s:%u failed (sock %d)",
                 s->host, (unsigned)s->port, sock);
        return;
    }
    pd->pending = true;
    pd->sent_ms = now_ms();
    pd->server_idx = is_probe ? -1 : server_idx;
}

static void stun_task(void *arg)
{
    (void)arg;
    int64_t next_due = now_ms() + 1000;   /* probe right after boot */
    while (s_started) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (!s_started) { break; }
        const int64_t now = now_ms();
        if (now < next_due && !s_stun_now_req) { continue; }

        /* The engine is registered before Wi-Fi is associated (netcom_init()
         * runs earlier), and lwIP's getaddrinfo() blocks for a minute or more
         * when the station has no DNS server yet -- that wedged this loop and
         * the panel showed no public address at all.  Wait for a station
         * address before touching the resolver; stun_resolve() caches the
         * result, so this cost is paid at most once per server. */
        if (!net_wifi_ip() || !net_wifi_ip()[0]) {
            next_due = now + 1000;        /* a forced request stays armed */
            continue;
        }
        s_stun_now_req = false;

        if (s_have_override) {
            stun_srv_t ovr;
            memset(&ovr, 0, sizeof(ovr));
            strncpy(ovr.host, s_ovr_host, sizeof(ovr.host) - 1);
            ovr.port = s_ovr_port;
            if (stun_resolve(&ovr)) {
                stun_send(&s_pend_main, s_sock_main, &ovr, false, -2);
                stun_send(&s_pend_probe, s_sock_probe, &ovr, true, -1);
            }
            next_due = now + STUN_PERIOD_MS;
            continue;
        }

        int idx = -1;
        for (int i = 0; i < s_srv_n; ++i) {
            if (now - s_srv[i].last_fail_ms >= STUN_FAIL_COOLDOWN_MS &&
                (idx < 0 || s_srv[i].last_fail_ms < s_srv[idx].last_fail_ms)) {
                idx = i;
            }
        }
        if (idx < 0) {
            next_due = now + 5000;
            continue;
        }
        stun_srv_t *s = &s_srv[idx];
        if (!stun_resolve(s)) {
            ESP_LOGW(TAG, "stun: cannot resolve %s (dns or routing down)", s->host);
            s->last_fail_ms = now;
            next_due = now + STUN_PERIOD_MS;
            continue;
        }
        if (s_pend_main.pending) {
            ESP_LOGW(TAG, "stun: no reply from %s:%u within %d ms",
                     s->host, (unsigned)s->port, STUN_PERIOD_MS);
        }
        stun_send(&s_pend_main, s_sock_main, s, false, idx);
        stun_send(&s_pend_probe, s_sock_probe, s, true, -1);
        ESP_LOGI(TAG, "stun: query %s:%u via sock main=%d probe=%d",
                 s->host, (unsigned)s->port, s_sock_main, s_sock_probe);
        next_due = now + STUN_PERIOD_MS;
    }
    s_stun_task = NULL;
    vTaskDeleteWithCaps(NULL);
}

/* ----------------------------------------------------------- playback (DAC) */

/* Advance one peer's playback head and return its next block (or NULL).
 * Re-anchors on loss; keeps the stream "alive" while the buffer has audio. */
static bool peer_jbuf_next(rt_peer_t *p, int16_t *out, int *peak_out)
{
    *peak_out = 0;
    const int64_t now = now_ms();
    if (p->have_latest && now - p->last_rx_ms > STREAM_IDLE_MS &&
        p->head_valid && (int16_t)(p->head - p->latest) > 0) {
        p->head_valid = false; p->have_latest = false; p->have_first = false;
        memset(p->jbuf_valid, 0, JBUF_SLOTS * sizeof(bool));
        opus_link_dec_free(p->opus_dec); p->opus_dec = NULL;
    }
    if (!p->have_latest) { return false; }
    if (!p->head_valid) {
        if (!p->have_first || (net_rx_order_keyed(&p->order, now, NP_RX_PTT_TIMEOUT_MS) &&
            (int16_t)(p->latest - p->stream_first) < PLAY_LAG)) return false;
        p->head = p->stream_first;
        p->head_valid = true;
    } else {
        const int32_t behind = (int32_t)(int16_t)(p->latest - p->head);
        if (behind > (JBUF_SLOTS - PLAY_LAG)) p->head = (uint16_t)(p->latest - PLAY_LAG);
    }
    if ((int16_t)(p->head - p->latest) > 0) return false;
    const uint16_t slot = p->head % JBUF_SLOTS;
    int decoded = -1;
    if (p->jbuf_valid[slot] && p->jbuf_seq[slot] == p->head) {
        if (!p->opus_dec) p->opus_dec = opus_link_dec_new();
        if (p->opus_dec) decoded = opus_link_decode(p->opus_dec, p->jbuf[slot], NP_AUDIO_OPUS, out);
        p->jbuf_valid[slot] = false;
    } else if (p->opus_dec) {
        decoded = opus_link_decode(p->opus_dec, NULL, 0, out);
        if (decoded > 0) p->rx_plc++;
    }
    p->head++;
    if (decoded <= 0) return false;
    for (int i = 0; i < NP_PKT_SAMPLES; ++i) {
        const int a = out[i] < 0 ? -out[i] : out[i];
        if (a > *peak_out) *peak_out = a;
    }
    return true;
}

/* The peer whose stream is freshest is the one we play (PTT semantics: the
 * last active sender wins; re-anchoring happens in the jitter buffer). */
static rt_peer_t *select_active_peer(void)
{
    static rt_peer_t *playing;
    rt_peer_t *best = NULL;
    for (int i = 0; i < SLOT_MAX; ++i) {
        rt_peer_t *p = &s_peer[i];
        if (!p->in_use || !p->jbuf || !p->order.audio) { continue; }
        const bool pending = p->have_latest && (!p->head_valid || (int16_t)(p->head - p->latest) <= 0);
        if (!net_rx_order_keyed(&p->order, now_ms(), NP_RX_PTT_TIMEOUT_MS) &&
            (!pending || (p != playing && now_ms() - p->last_rx_ms > STREAM_IDLE_MS))) { continue; }
        if (!best || p->last_rx_ms > best->last_rx_ms) { best = p; }
    }
    playing = best;
    return best;
}

/* PTT and buffered playback control the carrier; PCM amplitude never does. */
static int64_t s_last_play_ms;
static void bridge_key_update(bool played)
{
    peer_lock();
    const int64_t now = now_ms();
    rt_peer_t *active = select_active_peer();
    const bool incoming = active && net_rx_order_keyed(&active->order, now, NP_RX_PTT_TIMEOUT_MS);
    static uint16_t traced_epoch, traced_latest;
    static int64_t traced_last_ms;
    static bool traced_ptt;
    if (active) {
        traced_latest = active->order.latest;
        traced_last_ms = active->order.last_ms;
        traced_ptt = active->order.ptt;
    }
    const uint16_t epoch = active ? active->order.epoch : traced_epoch;
    const bool timed_out = !incoming && s_rx_ptt && traced_ptt && now - traced_last_ms > NP_RX_PTT_TIMEOUT_MS;
    if (incoming != s_rx_ptt || (incoming && epoch != traced_epoch)) {
        ptt_trace_note(incoming ? PTT_TRACE_NET_ON : timed_out ? PTT_TRACE_NET_TIMEOUT : PTT_TRACE_NET_OFF,
                       1, epoch, traced_latest);
        traced_epoch = incoming ? epoch : 0;
    }
    if (incoming != s_rx_ptt) { netcom_note_rx_ptt(incoming, timed_out); }
    s_rx_ptt = incoming;
    s_rx_ptt_ms = incoming ? active->order.last_ms : 0;
    const bool feeding = incoming || (active && active->have_latest &&
        (!active->head_valid || (int16_t)(active->head - active->latest) <= 0));
    peer_unlock();
    voice_bridge_network_state(feeding, bridge_active());
    const bool pcm_pending = voice_bridge_network_pending();
    if (feeding || played || pcm_pending) { s_last_play_ms = now; }
    const bool keyed = feeding ||
        (s_last_play_ms != 0 && now - s_last_play_ms < NP_RX_HANG_MS);
    netcom_note_rx_carrier(keyed);
    bool tailing = voice_net_tail_active();
    if (played) {
        s_tail_played = false;            /* a new over may have its own tone */
        if (tailing) { voice_net_tail_stop(); tailing = false; }
    }
    if (!keyed && !tailing && !s_tail_played && bridge_active()) {
        tailing = voice_net_tail_start();  /* request; never fails on timing */
        if (tailing) { s_tail_played = true; }
    }
    if (bridge_active()) {
        ptt_bridge_key(keyed || tailing);
    } else if (ptt_owned(PTT_OWNER_NETWORK, 1)) {
        ptt_bridge_key(false);
    }
    voice_bridge_network_keyed(ptt_owned(PTT_OWNER_NETWORK, 1) && ptt_bridge_keyed());
}

static void netp2pnet_play_task(void *arg)
{
    (void)arg;
    int16_t block[NP_PKT_SAMPLES];
    int16_t silence[NP_PKT_SAMPLES] = {0};
    int64_t next_ms = 0;
    while (!s_play_stop) {
        /* Establish RF/lead before making the first PCM visible to monitor. */
        bridge_key_update(false);
        if (voice_mode() == VOICE_MONITOR) {
            const int64_t now = now_ms();
            if (!next_ms || now - next_ms > 4 * NP_PKT_MS) { next_ms = now; }
            bool played = false;
            for (int due = 0; due < 4 && now >= next_ms; ++due) {
                if (voice_bridge_room() < NP_PKT_SAMPLES) { next_ms = now; break; }
                int peak = 0;
                peer_lock();
                const bool have = select_active_peer() ? peer_jbuf_next(select_active_peer(), block, &peak) : false;
                peer_unlock();
                if (have) {
                    sdr_rx_network_push(NETCOM_MODE_NETP2P, block, NP_PKT_SAMPLES);
                    voice_bridge_inject(block, NP_PKT_SAMPLES); played = true;
                }
                next_ms += NP_PKT_MS;
            }
            bridge_key_update(played);
            /* The monitor owns the amplifier in this mode. */
            if (s_amp_on) { board_amp_enable(false); s_amp_on = false; }
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        next_ms = 0;
        if (!audio_i2s_clock_running()) { (void)audio_i2s_start_clock_only(); }
        const bool dac_owned = !voice_dac_held_by_talk();
        if (dac_owned != s_amp_on) { board_amp_enable(dac_owned); s_amp_on = dac_owned; }
        const size_t preview = dac_owned ? tail_preview_fetch(block, NP_PKT_SAMPLES) : 0;
        if (preview) {
            voice_speaker_apply(block, preview);
            (void)audio_i2s_write(block, preview);
            continue;
        }
        int peak = 0;
        const bool ready = voice_bridge_network_ready();
        peer_lock();
        const bool have = ready && select_active_peer()
            ? peer_jbuf_next(select_active_peer(), block, &peak) : false;
        peer_unlock();
        bridge_key_update(have);
        if (have) { sdr_rx_network_push(NETCOM_MODE_NETP2P, block, NP_PKT_SAMPLES); }
        int16_t *pcm = have ? block : silence;
        if (!ptt_bridge_keyed()) { voice_speaker_apply(pcm, NP_PKT_SAMPLES); }
        if (voice_dac_held_by_talk()) {
            s_amp_on = false;
            vTaskDelay(pdMS_TO_TICKS(NP_PKT_MS));
        } else {
            (void)audio_i2s_write(pcm, NP_PKT_SAMPLES);
        }
    }
    if (ptt_owned(PTT_OWNER_NETWORK, 1)) { ptt_bridge_key(false); }
    voice_bridge_network_cancel();
    s_play_task = NULL;
    vTaskDeleteWithCaps(NULL);
}

/* --------------------------------------------------------------- TX path */

static int16_t  s_tx_acc[NP_PKT_SAMPLES];
static int      s_tx_acc_n;
static uint16_t s_tx_seq;

/*
 * The accumulator is fed from more than one task: the TALK task (PTT and tail
 * tone), the monitor task (analog bridge) and the TX self-test tone that the
 * web API may inject while the radio is already keyed.  Each frame is therefore
 * assembled *and sent* under one mutex: no two producers can interleave inside
 * a block, tear s_tx_acc_n, push the write index past the end of the array, or
 * hand two finished blocks to the socket in an order that contradicts the
 * sequence numbers the far end uses to spot loss.
 *
 * A mutex and not a spinlock is deliberate: lwip's sendto may block, and
 * calling a blocking API with interrupts disabled is not allowed.
 */
static SemaphoreHandle_t s_tx_mux;

static void tx_lock(void)   { if (s_tx_mux) { xSemaphoreTake(s_tx_mux, portMAX_DELAY); } }
static void tx_unlock(void) { if (s_tx_mux) { xSemaphoreGive(s_tx_mux); } }

/* Caller holds s_tx_mux. */
static opus_link_enc_t *s_opus_enc;
static uint8_t          s_enc_buf[NP_AUDIO_OPUS];

static bool send_audio_to_all(const int16_t *pcm, bool ptt)
{
    (void)net_tx_epoch(&s_tx_epoch, &s_tx_epoch_open, ptt);
    /* Encode once for every peer: the bytes are identical, only the address
     * differs.  This runs on the transmit task, not the audio task. */
    if (!s_opus_enc) {
        s_opus_enc = opus_link_enc_new();
    }
    const int enc = s_opus_enc
                        ? opus_link_encode(s_opus_enc, pcm, s_enc_buf, sizeof(s_enc_buf))
                        : -1;
    if (enc != NP_AUDIO_OPUS) { ++s_tx_errs; return false; }
    bool sent = false;
    bool failed = false;
    peer_lock();
    const uint16_t seq = s_tx_seq++;
    s_last_tx_ms = now_ms();
    for (int i = 0; i < SLOT_MAX; ++i) {
        rt_peer_t *p = &s_peer[i];
        if (!p->in_use || p->selftest || !peer_addr_set(p)) { continue; }
        if (!frame_send_to(p, F_AUDIO, seq, ptt, s_enc_buf)) {
            s_tx_errs++;
            failed = true;
        } else {
            s_tx_msgs++;
            sent = true;
        }
    }
    peer_unlock();
    return sent && !failed;
}

typedef struct {
    int16_t pcm[NP_PKT_SAMPLES];
    uint8_t ptt;
    int64_t end_ms;             /* diagnostic only; 0 for ordinary audio */
} np_tx_item_t;

static QueueHandle_t s_txq_pcm;
static TaskHandle_t  s_tx_task;

/*
 * The audio task only fills the queue; this task pays the Opus cost.
 * Encoding 40 ms of audio takes ~12 ms on this chip, which would stall the
 * sample clock if it happened in the monitor/talk task.
 */
static void np_tx_task(void *arg)
{
    (void)arg;
    while (!s_tx_stop) {
        np_tx_item_t item;
        if (!s_txq_pcm || xQueueReceive(s_txq_pcm, &item, pdMS_TO_TICKS(100)) != pdTRUE) continue;
        const bool sent = send_audio_to_all(item.pcm, item.ptt != 0);
        if (!item.ptt) { netcom_note_end_sent(item.end_ms, sent); }
    }
    opus_link_enc_free(s_opus_enc); s_opus_enc = NULL;
    s_tx_task = NULL;
    vTaskDeleteWithCaps(NULL);
}

static void netp2pnet_tx_sample(const int16_t *pcm, size_t n, bool ptt)
{
    tx_lock();
    if (!s_started || !ptt) {
        s_tx_acc_n = 0;
        tx_unlock();
        return;
    }
    for (size_t i = 0; i < n; ++i) {
        s_tx_acc[s_tx_acc_n++] = pcm[i];
        if (s_tx_acc_n == NP_PKT_SAMPLES) {
            s_tx_acc_n = 0;
            if (s_txq_pcm) {
                np_tx_item_t item;
                memcpy(item.pcm, s_tx_acc, sizeof(item.pcm));
                item.ptt = 1;
                item.end_ms = 0;
                if (xQueueSend(s_txq_pcm, &item, 0) != pdTRUE) {
                    np_tx_item_t dump;
                    (void)xQueueReceive(s_txq_pcm, &dump, 0);
                    (void)xQueueSend(s_txq_pcm, &item, 0);
                }
            }
        }
    }
    tx_unlock();
}

static void netp2pnet_tx_end(void)
{
    tx_lock();
    if (!s_started) {
        tx_unlock();
        return;
    }
    if (s_tx_acc_n > 0) {
        while (s_tx_acc_n < NP_PKT_SAMPLES) s_tx_acc[s_tx_acc_n++] = 0;
    } else {
        memset(s_tx_acc, 0, sizeof(s_tx_acc));
    }
    s_tx_acc_n = 0;
    if (s_txq_pcm) {
        np_tx_item_t item, dump;
        memcpy(item.pcm, s_tx_acc, sizeof(item.pcm));
        item.ptt = 0;
        item.end_ms = now_ms();
        if (xQueueSend(s_txq_pcm, &item, 0) != pdTRUE) {
            (void)xQueueReceive(s_txq_pcm, &dump, 0);
            (void)xQueueSend(s_txq_pcm, &item, 0);
        }
    }
    tx_unlock();
}

/* -------------------------------------------------------------- test tone */
static TaskHandle_t s_test_tone_task;
static void test_tone_task(void *arg)
{
    const int ms = (int)(intptr_t)arg;
    rt_peer_t *p = &s_peer[SLOT_MAX - 1];
    opus_link_enc_t *enc = opus_link_enc_new();
    peer_lock();
    const bool ready = enc && peer_jbuf_alloc(p);
    peer_unlock();
    if (!ready) { opus_link_enc_free(enc); s_test_tone_task = NULL; vTaskDeleteWithCaps(NULL); return; }
    int16_t pcm[NP_PKT_SAMPLES];
    uint8_t packet[NP_AUDIO_OPUS];
    const int frames = (ms + NP_PKT_MS - 1) / NP_PKT_MS;
    for (int f = 0; f < frames && s_started; ++f) {
        for (int i = 0; i < NP_PKT_SAMPLES; ++i) {
            const double t = (double)(f * NP_PKT_SAMPLES + i) / AUDIO_SAMPLE_RATE_HZ;
            pcm[i] = (int16_t)(6000.0 * sin(2.0 * M_PI * 1000.0 * t));
        }
        const int n = opus_link_encode(enc, pcm, packet, sizeof(packet));
        if (n != NP_AUDIO_OPUS) { ESP_LOGE(TAG, "test tone encode failed (%d)", n); break; }
        peer_lock();
        if (!p->in_use) {
            p->in_use = true; p->selftest = true; memset(p->mac, 0xEE, 6);
            snprintf(p->name, sizeof(p->name), "test"); peer_reset_stream(p);
        }
        uint16_t seq = p->have_latest ? (uint16_t)(p->latest + 1) : 1000;
        if (!p->have_latest) { p->stream_first = seq; p->have_first = true; p->head_valid = false; }
        const uint16_t slot = seq % JBUF_SLOTS;
        memcpy(p->jbuf[slot], packet, NP_AUDIO_OPUS);
        p->jbuf_seq[slot] = seq; p->jbuf_valid[slot] = true;
        p->latest = seq; p->have_latest = true; p->last_rx_ms = now_ms(); p->rx_msgs++;
        peer_unlock();
        vTaskDelay(pdMS_TO_TICKS(NP_PKT_MS));
    }
    opus_link_enc_free(enc);
    peer_lock(); peer_clear(p); peer_unlock();
    s_test_tone_task = NULL;
    ESP_LOGW(TAG, "test tone complete: %d ms", ms);
    vTaskDeleteWithCaps(NULL);
}

static void netp2pnet_test_tone(int ms)
{
    if (!s_started || ms <= 0 || s_test_tone_task) return;
    if (ms > 30000) ms = 30000;
    if (xTaskCreateWithCaps(test_tone_task, "np_tone", 16384,
                            (void *)(intptr_t)ms, 5, &s_test_tone_task,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        s_test_tone_task = NULL;
        ESP_LOGE(TAG, "test tone task failed to start");
    }
}

/* -------------------------------------------------------------- engine seams */

static esp_err_t netp2pnet_stop(void);

static esp_err_t netp2pnet_start(const netcom_cfg_t *cfg)
{
    (void)cfg;
    if (s_started) { return ESP_OK; }
    if (s_eng_task || s_stun_task || s_play_task || s_tx_task || s_test_tone_task) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_crc_init) {
        crc32_init();
        s_crc_init = true;
    }
    if (esp_wifi_get_mac(WIFI_IF_STA, s_my_mac) != ESP_OK) {
        ESP_LOGE(TAG, "start: could not read base MAC");
        return ESP_FAIL;
    }

    if (!s_peer_mux) { s_peer_mux = xSemaphoreCreateMutex(); }
    if (!s_tx_mux) { s_tx_mux = xSemaphoreCreateMutex(); }
    if (!s_peer_mux || !s_tx_mux) { return ESP_ERR_NO_MEM; }
    if (socket_up() != ESP_OK) {
        return ESP_FAIL;
    }

    /*
     * Transmit path: the audio task queues PCM, this task encodes with Opus.
     * 24 items = ~1 s of slack; the queue and the 16 KB stack both live in
     * PSRAM so internal RAM stays free for WiFi and lwIP.
     */
    if (!s_txq_pcm) {
        s_txq_pcm = xQueueCreateWithCaps(24, sizeof(np_tx_item_t),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_txq_pcm) {
            s_txq_pcm = xQueueCreateWithCaps(24, sizeof(np_tx_item_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
    }
    if (!s_txq_pcm) {
        ESP_LOGE(TAG, "start: tx queue alloc failed");
        (void)netp2pnet_stop();
        return ESP_ERR_NO_MEM;
    }
    s_tx_stop = false;
    if (!s_tx_task) {
        if (xTaskCreateWithCaps(np_tx_task, "np_tx", 16384, NULL, 5,
                                &s_tx_task, MALLOC_CAP_SPIRAM) != pdPASS) {
            s_tx_task = NULL;
            ESP_LOGE(TAG, "start: tx task failed");
            (void)netp2pnet_stop();
            return ESP_ERR_NO_MEM;
        }
    }

    peer_lock();
    for (int i = 0; i < SLOT_MAX; ++i) {
        rt_peer_t *p = &s_peer[i];
        if (p->in_use) {
            p->linked = false;
            peer_reset_stream(p);
            memset(&p->order, 0, sizeof(p->order));
            p->last_rx_ms = 0;
            p->last_frame_ms = 0;
            p->last_linked_ms = 0;
            p->last_ping_ms = 0;
            p->last_knock_ms = 0;
            p->ping_tx_ms = 0;
            p->knock_tx_ms = 0;
        }
    }
    peer_unlock();
    s_last_tx_ms = 0;
    s_tx_msgs = 0;
    s_tx_errs = 0;
    /* One TX serialisation mutex for the whole engine lifetime; producers only
     * run while s_started, so creating it here (before s_started flips) is
     * enough to guarantee every tx_sample/tx_end sees a valid handle. */
    tx_lock();
    s_tx_acc_n = 0;
    s_tx_seq = 0;
    s_tx_epoch = (uint16_t)esp_random(); if (!s_tx_epoch) { s_tx_epoch = 1; }
    s_tx_epoch_open = false;
    tx_unlock();
    s_amp_on = false;
    s_last_play_ms = 0;
    s_rx_ptt = false;

    s_pend_main.pending = false;
    s_pend_probe.pending = false;
    s_main_port_preserved = false;
    s_probe_port_preserved = false;
    s_port_preserving = -1;
    s_pub_ip = 0;
    s_pub_port = 0;
    s_last_stun_ms = 0;
    s_last_rtt_ms = 0;
    if (ptt_owned(PTT_OWNER_NETWORK, 1)) {
        ptt_bridge_key(false);
    }
    board_amp_enable(false);

    s_tx_stop = false;
    s_started = true;
    s_play_stop = false;
    /* Stacks in PSRAM: internal RAM is already tight (the WiFi driver logs
     * "mem fail"), and a restart of the engine after a mode switch used to
     * fail with ESP_ERR_NO_MEM because 13 KB of internal stack could not be
     * found a second time. */
    if (xTaskCreatePinnedToCoreWithCaps(engine_task, "np2p_eng", 6144, NULL, 8,
                                        &s_eng_task, 1,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        s_started = false;
        (void)netp2pnet_stop();
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCoreWithCaps(netp2pnet_play_task, "np2p_play", 16384, NULL, 8,
                                        &s_play_task, 1,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        s_play_stop = true;
        s_started = false;
        (void)netp2pnet_stop();
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCoreWithCaps(stun_task, "np2p_stun", 3072, NULL, 3,
                                        &s_stun_task, 0,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        s_play_stop = true;
        s_started = false;
        (void)netp2pnet_stop();
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "engine up: room %u, udp %u, hub %s, stun list %s",
             netcom_room(), (unsigned)UDP_PORT_MAIN,
             s_hub_on ? "ON" : "off",
             s_stun_str[0] ? s_stun_str : "built-in");
    return ESP_OK;
}
static esp_err_t netp2pnet_stop(void)
{
    tx_lock();
    s_started = false;
    s_tx_stop = true;
    tx_unlock();
    s_play_stop = true;
    if (ptt_owned(PTT_OWNER_NETWORK, 1)) { ptt_bridge_key(false); }
    for (int i = 0; i < 1000 && (s_eng_task || s_stun_task || s_play_task || s_tx_task || s_test_tone_task); ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_eng_task || s_stun_task || s_play_task || s_tx_task || s_test_tone_task) {
        ESP_LOGE(TAG, "workers did not stop; keeping sockets and peer buffers alive");
        return ESP_ERR_TIMEOUT;
    }
    if (ptt_owned(PTT_OWNER_NETWORK, 1)) ptt_bridge_key(false);
    board_amp_enable(false);
    socket_down();
    s_pend_main.pending = false;
    s_pend_probe.pending = false;
    peer_lock();
    for (int i = 0; i < SLOT_MAX; ++i) {
        rt_peer_t *p = &s_peer[i];
        if (p->in_use) { p->linked = false; peer_reset_stream(p); }
        peer_audio_free(p);
    }
    peer_unlock();
    tx_lock();
    if (s_txq_pcm) { vQueueDeleteWithCaps(s_txq_pcm); s_txq_pcm = NULL; }
    tx_unlock();
    ESP_LOGI(TAG, "engine down (sent %u / err %u)",
             (unsigned)s_tx_msgs, (unsigned)s_tx_errs);
    return ESP_OK;
}

static void netp2pnet_stats(uint32_t *tx_msgs, uint32_t *rx_msgs,
                            int *peers, const char **state)
{
    peer_lock();
    uint32_t rx = 0;
    int linked = 0;
    for (int i = 0; i < SLOT_MAX; ++i) {
        rt_peer_t *p = &s_peer[i];
        if (p->in_use) {
            rx += p->rx_msgs;
            if (p->linked && !p->selftest) {
                linked++;
            }
        }
    }
    *tx_msgs = s_tx_msgs;
    *rx_msgs = rx;
    *peers = linked;
    if (!s_started) {
        *state = "stopped";
        peer_unlock();
        return;
    }
    if (!net_wifi_connected()) {
        *state = "no-net";
        peer_unlock();
        return;
    }
    const int64_t now = now_ms();
    if (s_last_tx_ms && now - s_last_tx_ms < 500) {
        *state = "tx";
        peer_unlock();
        return;
    }
    bool any_knock = false, any_rx = false;
    for (int i = 0; i < SLOT_MAX; ++i) {
        rt_peer_t *p = &s_peer[i];
        if (!p->in_use || p->selftest) {
            continue;
        }
        if (!p->linked) {
            any_knock = true;
        }
        if (p->last_rx_ms && now - p->last_rx_ms < 500) {
            any_rx = true;
        }
    }
    if (any_rx) {
        *state = "rx";
    } else if (any_knock) {
        *state = "knocking";
    } else {
        *state = "idle";
    }
    peer_unlock();
}

/* Only a known hub can be reported offline. An ordinary expired peer must not
 * create a fictitious hub-offline warning. Any live hub keeps the route up. */
static int64_t hub_quiet_ms(void)
{
    int64_t quiet = 0;
    const int64_t now = now_ms();
    for (int i = 0; i < SLOT_MAX; ++i) {
        const rt_peer_t *p = &s_peer[i];
        if (!p->in_use || p->selftest || p->via_hub || !p->hub || p->last_frame_ms == 0) {
            continue;
        }
        const int64_t q = now - p->last_frame_ms;
        if (p->linked && q <= PEER_EXPIRE_MS) { return 0; }
        if (q > quiet) {
            quiet = q;
        }
    }
    return quiet;
}

static void netp2pnet_diag(char *out, size_t n)
{
    char pub[20] = "0.0.0.0";
    if (s_pub_ip) {
        struct in_addr a;
        a.s_addr = s_pub_ip;
        inet_ntoa_r(a, pub, sizeof(pub));
    }
    const char *pp = (s_port_preserving == 1) ? "port-preserving"
                     : (s_port_preserving == 2) ? "NOT port-preserving" : "nat?";
    int linked = 0;
    for (int i = 0; i < SLOT_MAX; ++i) {
        if (s_peer[i].in_use && s_peer[i].linked && !s_peer[i].selftest) {
            linked++;
        }
    }
    const int64_t age = s_last_stun_ms ? (now_ms() - s_last_stun_ms) / 1000 : -1;
    const int64_t quiet = hub_quiet_ms();
    snprintf(out, n, "pub %s:%u (%s)%s stun %llds rtt %d ms | links %d/%d hub:%s",
             pub, (unsigned)s_pub_port, pp,
             (age >= 0 && age < 30) ? "" : " STALE",
             (long long)age, s_last_rtt_ms, linked, PEER_MAX, s_hub_on ? "on" : "off");
    if (!s_hub_on && quiet > PEER_EXPIRE_MS) {
        const size_t used = strlen(out);
        snprintf(out + used, n - used, " | hub offline %llds",
                 (long long)(quiet / 1000));
    }
}

static void netp2pnet_on_room_change(uint16_t room)
{
    (void)room;
    peer_lock();
    for (int i = 0; i < SLOT_MAX; ++i) {
        if (s_peer[i].in_use) {
            peer_reset_stream(&s_peer[i]);
        }
    }
    s_rx_ptt = false;
    peer_unlock();
}

static const netcom_engine_t s_engine = {
    .name         = "net-p2p",
    .start        = netp2pnet_start,
    .stop         = netp2pnet_stop,
    .tx_sample    = netp2pnet_tx_sample,
    .tx_end       = netp2pnet_tx_end,
    .rx_sample    = NULL,
    .stats        = netp2pnet_stats,
    .on_room_change = netp2pnet_on_room_change,
    .diag         = netp2pnet_diag,
    .test_tone    = netp2pnet_test_tone,
};

void netp2pnet_init(void)
{
    if (!s_peer_mux) { s_peer_mux = xSemaphoreCreateMutex(); }
    stun_default_list();
    peers_load();               /* NVS: peer table, hub flag, stun override */
    netcom_register_engine(NETCOM_MODE_NETP2P, &s_engine);
}

/* --------------------------------------------------------------- public API */

esp_err_t netp2pnet_peer_add_confirmed(const char *name, const char *ip, uint16_t port, bool restore)
{
    if (!name || !name[0] || !ip || !port || strlen(name) >= 8) {
        return ESP_ERR_INVALID_ARG;
    }
    peer_lock();
    np_addr_t a;
    memset(&a, 0, sizeof(a));
    a.port = port;
    struct in6_addr a6;
    struct in_addr a4;
    if (inet_pton(AF_INET6, ip, &a6) == 1) {
        a.fam = 6;
        memcpy(a.ip6, &a6, sizeof(a.ip6));
    } else if (inet_aton(ip, &a4) != 0) {
        a.fam = 4;
        a.ip = a4.s_addr;
    } else {
        peer_unlock();
        return ESP_ERR_INVALID_ARG;
    }
    if (!np_addr_set(&a)) { peer_unlock(); return ESP_ERR_INVALID_ARG; }
    if (s_access_error) { peer_unlock(); return ESP_ERR_INVALID_STATE; }
    const int denied = denied_address(&a);
    if (denied >= 0 && !restore) { peer_unlock(); return ESP_ERR_INVALID_STATE; }
    /* Replace an existing entry for the same (address, port). */
    rt_peer_t *p = NULL;
    for (int i = 0; i < SLOT_MAX; ++i) {
        rt_peer_t *c = &s_peer[i];
        if (!c->in_use || c->selftest || c->port != port) { continue; }
        if (c->fam == 6 || a.fam == 6) {
            if (c->fam == a.fam && memcmp(c->ip6, a.ip6, 16) == 0) { p = c; break; }
        } else if (c->ip == a.ip) {
            p = c;
            break;
        }
    }
    /* No entry for that address, but a frame may already have opened an
     * anonymous slot for this peer: audio arriving before the operator typed
     * the address creates one with a MAC and ip == 0, and peer_by_mac() would
     * hand every later frame to that slot for ever - the configured entry would
     * sit at "knocking" while the real traffic flows through the other one.
     * Adopt that slot (it has no address and no name yet, so nothing is lost)
     * and keep its MAC, so the first KNOCK_ACK after this call links the
     * configured slot. */
    bool adopted = false;
    if (!p) {
        for (int i = 0; i < SLOT_MAX; ++i) {
            rt_peer_t *c = &s_peer[i];
            if (c->in_use && !c->selftest && !peer_addr_set(c) && c->name[0] == '\0') {
                p = c;
                adopted = true;
                ESP_LOGW(TAG, "peer add: adopting unbound slot %d for %s:%u",
                         i, ip, (unsigned)port);
                break;
            }
        }
    }
    uint8_t keep_mac[6];
    memset(keep_mac, 0, sizeof(keep_mac));
    if (p) {
        memcpy(keep_mac, p->mac, sizeof(keep_mac));
    }
    if (!p) {
        p = peer_free_slot();
        if (!p) { peer_unlock(); return ESP_ERR_NOT_FOUND; }
    }
    rt_peer_t previous = *p;
    np_access_record_t restored = {0};
    if (denied >= 0) { restored = s_denied[denied]; denied_remove((size_t)denied); }
    /* Retain buffers until the new policy has been durably accepted. */
    memset(p, 0, sizeof(*p));
    p->in_use = true;
    snprintf(p->name, sizeof(p->name), "%s", name);
    p->manual = true;
    peer_take_addr(p, &a);
    p->port = port;
    p->linked = false;
    if (adopted || np_access_has_mac(keep_mac)) { memcpy(p->mac, keep_mac, 6); }
    if (denied >= 0 && np_access_has_mac(restored.mac)) { memcpy(p->mac, restored.mac, 6); }
    esp_err_t saved = peers_save_ex(NULL, p);
    if (saved != ESP_OK) {
        *p = previous;
        if (denied >= 0) { s_denied[s_denied_count++] = restored; }
        peer_unlock(); return saved;
    }
    peer_audio_free(&previous);
    /* Collapse duplicates for the same address.  A frame that arrives while the
     * configured slot is being replaced (or an older firmware that saved two
     * slots for one peer) can leave a second slot with the same ip:port, and
     * every audio frame would then be transmitted twice. */
    for (int i = 0; i < SLOT_MAX; ++i) {
        rt_peer_t *c = &s_peer[i];
        if (c != p && c->in_use && !c->selftest &&
            ((c->fam == 6 || a.fam == 6) ? (c->fam == a.fam && memcmp(c->ip6, a.ip6, 16) == 0)
                                         : (c->ip == a.ip)) && c->port == port) {
            ESP_LOGW(TAG, "peer add: dropping duplicate slot %d for %s:%u",
                     i, ip, (unsigned)port);
            peer_clear(c);
        }
    }
    char ipb[48] = "?";
    peer_addr_text(p, ipb, sizeof(ipb));
    ESP_LOGI(TAG, "peer add: '%s' %s:%u", p->name, ipb, (unsigned)port);
    peer_unlock();
    return ESP_OK;
}

esp_err_t netp2pnet_peer_add(const char *name, const char *ip, uint16_t port)
{
    return netp2pnet_peer_add_confirmed(name, ip, port, false);
}
esp_err_t netp2pnet_peer_del(int index)
{
    peer_lock();
    if (s_access_error) { peer_unlock(); return ESP_ERR_INVALID_STATE; }
    int n = 0;
    for (int i = 0; i < SLOT_MAX; ++i) {
        rt_peer_t *p = &s_peer[i];
        if (!p->in_use || p->selftest || n++ != index) { continue; }
        np_access_record_t record; peer_record(p, &record);
        if (!np_access_has_mac(record.mac) && !peer_addr_set(p)) {
            peer_unlock(); return ESP_ERR_INVALID_STATE;
        }
        if (s_denied_count == NP_DENY_LIMIT) { peer_unlock(); return ESP_ERR_NO_MEM; }
        s_denied[s_denied_count++] = record;
        esp_err_t saved = peers_save_ex(p, NULL);
        if (saved != ESP_OK) {
            memset(&s_denied[--s_denied_count], 0, sizeof(s_denied[0]));
            peer_unlock(); return saved;
        }
        /* Notification is best effort; local refusal does not depend on delivery. */
        if (s_started && !p->via_hub) { (void)frame_send_to(p, F_BYE, 0, false, NULL); }
        snprintf(s_last_del, sizeof(s_last_del), "persistent refusal (%s)",
                 np_access_has_mac(record.mac) ? "identity" : "address only");
        peer_clear(p);
        peer_unlock(); return ESP_OK;
    }
    peer_unlock(); return ESP_ERR_NOT_FOUND;
}
esp_err_t netp2pnet_unblock(const char *id)
{
    if (!id || !*id) { return ESP_ERR_INVALID_ARG; }
    peer_lock();
    for (size_t i = 0; i < s_denied_count; ++i) {
        char key[56]; np_access_id(&s_denied[i], key, sizeof(key));
        if (strcmp(key, id)) { continue; }
        np_access_record_t r = s_denied[i]; denied_remove(i);
        esp_err_t saved = peers_save_ex(NULL, NULL);
        if (saved != ESP_OK) { s_denied[s_denied_count++] = r; }
        peer_unlock(); return saved;
    }
    peer_unlock(); return ESP_ERR_NOT_FOUND;
}

esp_err_t netp2pnet_peer_knock(int index)
{
    peer_lock();
    int n = 0;
    for (int i = 0; i < SLOT_MAX; ++i) {
        rt_peer_t *p = &s_peer[i];
        if (p->in_use && !p->selftest) {
            if (n == index) {
                /*
                 * Report what actually happened.  This used to return OK even
                 * when nothing was sent (engine stopped, or the peer has no
                 * usable address), so the panel's "重敲" button looked dead --
                 * which is exactly what the operator saw after the router
                 * stopped handing out a global IPv6 prefix: the stored peer
                 * address pointed at a network that no longer existed.
                 */
                if (!s_started) {
                    peer_unlock();
                    return ESP_ERR_INVALID_STATE;
                }
                if (!peer_addr_set(p)) {
                    peer_unlock();
                    return ESP_ERR_INVALID_ARG;
                }
                if (!frame_send_to(p, F_KNOCK, 0, false, NULL)) {
                    peer_unlock();
                    return ESP_FAIL;
                }
                p->last_knock_ms = now_ms();
                p->knock_tx_ms = p->last_knock_ms;
                peer_unlock();
                return ESP_OK;
            }
            n++;
        }
    }
    peer_unlock();
    return ESP_ERR_NOT_FOUND;
}

int netp2pnet_peer_count(void)
{
    peer_lock();
    int n = 0;
    for (int i = 0; i < SLOT_MAX; ++i) {
        if (s_peer[i].in_use && !s_peer[i].selftest) {
            n++;
        }
    }
    peer_unlock();
    return n;
}

static const char *peer_state_str(const rt_peer_t *p)
{
    if (!p->in_use) {
        return "empty";
    }
    if (p->via_hub) {
        const rt_peer_t *upstream = peer_by_mac(p->hub_mac);
        if (!upstream || !upstream->linked || now_ms() - upstream->last_frame_ms > PEER_EXPIRE_MS) {
            return "expired";
        }
        return p->linked && now_ms() - p->last_frame_ms <= PEER_EXPIRE_MS ? "linked" : "expired";
    }
    /*
     * Liveness is last_frame_ms, the same clock the engine expires on
     * (engine_tick_timers).  This used last_rx_ms, which is AUDIO only, so a
     * link that was simply quiet -- no voice for 90 s, but PING/PONG every 10 s
     * as designed -- was labelled "expired" while it was perfectly healthy.
     * That is what made an idle but working link look dead in the panel, and
     * sent the operator hunting for a network fault that was not there.
     */
    if (p->linked && p->last_frame_ms > 0 &&
        now_ms() - p->last_frame_ms > PEER_EXPIRE_MS) {
        return "expired";
    }
    if (p->linked) {
        return "linked";
    }
    return "knocking";
}

int netp2pnet_peer_info(int index, char *name, size_t name_len,
                        char *ip, size_t ip_len, uint16_t *port,
                        const char **state, int *rtt_ms,
                        uint32_t *rx, uint32_t *tx, uint32_t *drop)
{
    peer_lock();
    int n = 0;
    for (int i = 0; i < SLOT_MAX; ++i) {
        rt_peer_t *p = &s_peer[i];
        if (p->in_use && !p->selftest) {
            if (n == index) {
                if (name) {
                    snprintf(name, name_len, "%s", p->name);
                }
                if (ip) {
                    char tmp[48] = "0.0.0.0";
                    peer_addr_text(p, tmp, sizeof(tmp));
                    snprintf(ip, ip_len, "%s", tmp);
                }
                if (port) { *port = p->port; }
                if (state) { *state = peer_state_str(p); }
                if (rtt_ms) { *rtt_ms = p->rtt_ms; }
                if (rx) { *rx = p->rx_msgs; }
                if (tx) { *tx = p->tx_msgs; }
                if (drop) { *drop = p->rx_drop; }
                peer_unlock();
                return 0;
            }
            n++;
        }
    }
    peer_unlock();
    return -1;
}

esp_err_t netp2pnet_hub_set(bool on)
{
    peer_lock();
    const bool previous = s_hub_on;
    s_hub_on = on;
    peers_save();
    if (s_peers_dirty) {
        s_hub_on = previous;
        peer_unlock();
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "hub -> %s", on ? "ON" : "off");
    peer_unlock();
    return ESP_OK;
}

bool netp2pnet_hub_on(void)
{
    return s_hub_on;
}

/* Parse "host:port" into the override; "" restores the built-in list.  Returns
 * false when a non-empty override is malformed, so the caller can report the
 * bad input instead of silently keeping the previous server. */
static bool stun_set_inner(const char *host_port)
{
    if (!host_port || !host_port[0]) {
        s_have_override = false;
        s_ovr_host[0] = '\0';
        s_stun_str[0] = '\0';
        return true;
    }
    const char *colon = strrchr(host_port, ':');
    if (!colon || colon == host_port) {
        return false;            /* need "host:port" */
    }
    const size_t hl = (size_t)(colon - host_port);
    if (hl >= sizeof(s_ovr_host)) {
        return false;            /* host name too long */
    }
    const int p = atoi(colon + 1);
    if (p < 1 || p > 65535) {
        return false;            /* missing or out-of-range port */
    }
    memcpy(s_ovr_host, host_port, hl);
    s_ovr_host[hl] = '\0';
    s_ovr_port = (uint16_t)p;
    s_have_override = true;
    snprintf(s_stun_str, sizeof(s_stun_str), "%s:%u", s_ovr_host, (unsigned)s_ovr_port);
    return true;
}

esp_err_t netp2pnet_stun_set(const char *host_port)
{
    if (!stun_set_inner(host_port)) {
        return ESP_ERR_INVALID_ARG;
    }
    peers_save();
    ESP_LOGI(TAG, "stun override -> %s",
             s_stun_str[0] ? s_stun_str : "(built-in list)");
    return ESP_OK;
}

const char *netp2pnet_stun_str(void)
{
    return s_stun_str;
}

void netp2pnet_stun_now(void)
{
    s_stun_now_req = true;
    ESP_LOGI(TAG, "stun: probe requested");
}

void netp2pnet_console(char *out, size_t n)
{
    if (n == 0) {
        return;
    }
    char pub[20] = "0.0.0.0";
    if (s_pub_ip) {
        struct in_addr a;
        a.s_addr = s_pub_ip;
        inet_ntoa_r(a, pub, sizeof(pub));
    }
    int off = snprintf(out, n,
                       "public: %s:%u   nat: %s   stun: %s (rtt %d ms)\n"
                       "hub: %s   room: %u\n",
                       pub, (unsigned)s_pub_port,
                       s_port_preserving == 1 ? "port-preserving"
                       : s_port_preserving == 2 ? "NOT port-preserving" : "unknown",
                       s_stun_str[0] ? s_stun_str : "built-in list",
                       s_last_rtt_ms,
                       s_hub_on ? "ON" : "off", netcom_room());
    if (off < 0) {
        off = 0;
    }
    int li = 0;
    for (int i = 0; i < SLOT_MAX && (size_t)off + 1 < n; ++i) {
        rt_peer_t *p = &s_peer[i];
        if (!p->in_use || p->selftest) {
            continue;
        }
        char ipb[48] = "0.0.0.0";
        peer_addr_text(p, ipb, sizeof(ipb));
        off += snprintf(out + off, n - (size_t)off,
                        "  peer %d: '%s' %s:%u  %s  rtt %d ms  rx %u tx %u drop %u\n",
                        li++, p->name, ipb, (unsigned)p->port, peer_state_str(p),
                        p->rtt_ms, (unsigned)p->rx_msgs, (unsigned)p->tx_msgs,
                        (unsigned)p->rx_drop);
    }
    out[n - 1] = '\0';
}

void netp2pnet_factory_reset(void)
{
    peer_lock();
    for (int i = 0; i < SLOT_MAX; ++i) {
        if (s_peer[i].in_use) {
            if (s_peer[i].linked && s_started) {
                (void)frame_send_to(&s_peer[i], F_BYE, 0, false, NULL);
            }
            peer_clear(&s_peer[i]);
        }
    }
    s_denied_count = 0; memset(s_denied, 0, sizeof(s_denied)); s_access_error = false;
    s_hub_on = false;
    s_have_override = false;
    s_ovr_host[0] = '\0';
    s_stun_str[0] = '\0';
    stun_default_list();
    peers_save();
    ESP_LOGI(TAG, "factory reset: peers cleared, hub off, stun list default");
    peer_unlock();
}

/* True when any interface holds a preferred, non-link-local IPv6 address.
 * Public direct connection is impossible without one, and the panel should say
 * so instead of showing a peer that knocks into the void. */
bool netp2pnet_has_global_ipv6(void)
{
#if LWIP_IPV6
    for (struct netif *ifp = netif_list; ifp; ifp = ifp->next) {
        for (int i = 0; i < LWIP_IPV6_NUM_ADDRESSES; ++i) {
            const ip6_addr_t *a = netif_ip6_addr(ifp, i);
            if (netif_ip6_addr_state(ifp, i) != IP6_ADDR_PREFERRED) {
                continue;
            }
            if (!ip6_addr_islinklocal(a) && !ip6_addr_isloopback(a) &&
                !ip6_addr_isany(a)) {
                return true;
            }
        }
    }
#else
    (void)0;
#endif
    return false;
}

void netp2pnet_status_json(char *buf, size_t n)
{
    if (n == 0) {
        return;
    }
    peer_lock();
    /* The caller runs in the httpd task (internal stack), which is a safe place
     * to write NVS; the engine task only sets the flag. */
    if (s_peers_dirty) {
        s_peers_dirty = false;
        peers_save();
    }
    size_t off = 0;
    char pub[20] = "0.0.0.0";
    if (s_pub_ip) {
        struct in_addr a;
        a.s_addr = s_pub_ip;
        inet_ntoa_r(a, pub, sizeof(pub));
    }
    const char *nat = (s_port_preserving == 1) ? "port-preserving"
                    : (s_port_preserving == 2) ? "not-port-preserving" : "unknown";
    int linked = 0, registered = 0, direct = 0, indirect = 0, hubs = 0;
    unsigned audio_bytes = 0;
    for (int i = 0; i < SLOT_MAX; ++i) {
        if (s_peer[i].in_use && !s_peer[i].selftest) { ++registered; }
        if (s_peer[i].in_use && !s_peer[i].selftest) {
            if (s_peer[i].via_hub) { ++indirect; }
            else { ++direct; if (s_peer[i].hub) { ++hubs; } }
        }
        if (s_peer[i].jbuf) { audio_bytes += JBUF_SLOTS * (NP_AUDIO_OPUS + sizeof(uint16_t) + sizeof(bool)); }
        if (s_peer[i].in_use && s_peer[i].linked && !s_peer[i].selftest) {
            linked++;
        }
    }
    char stun_esc[128];
    json_escape_str_n(s_stun_str, stun_esc, sizeof(stun_esc));
    const int64_t age = s_last_stun_ms ? (now_ms() - s_last_stun_ms) / 1000 : -1;
    const int64_t quiet = hub_quiet_ms();
    const bool hub_offline = (!s_hub_on && quiet > PEER_EXPIRE_MS);
    off += (size_t)snprintf(buf + off, n - off,
        "\"np2p\":{\"pub\":\"%s\",\"port\":%u,\"nat\":\"%s\","
        "\"stun\":\"%s\",\"stun_age_s\":%lld,\"rtt_ms\":%d,"
        "\"bye_tx\":%d,\"bye_rx\":%u,\"last_del\":\"%s\","
        "\"drops\":{\"short\":%u,\"room\":%u,\"self\":%u,\"tomb\":%u,\"psk\":%u},"
        "\"hub\":%s,\"hub_offline\":%s,\"hub_quiet_s\":%lld,\"links\":%d,"
        "\"g6\":%s,\"peer_limit\":%d,\"peer_count\":%d,\"jbuf_bytes\":%u,\"drop_full\":%u,\"save_pending\":%s,"
        "\"access_version\":1,\"access_ready\":%s,\"denied_count\":%u,\"denied_limit\":%u,"
        "\"hub_role\":\"%s\",\"hub_conflict\":%s,\"direct_count\":%d,\"indirect_count\":%d,\"peers\":[",
        pub, (unsigned)s_pub_port, nat, stun_esc,
        (long long)age, s_last_rtt_ms, s_last_send_rc, (unsigned)s_bye_rx,
        s_last_del,
        (unsigned)s_drop_short, (unsigned)s_drop_room, (unsigned)s_drop_self,
        (unsigned)s_drop_tomb, (unsigned)s_drop_psk,
        s_hub_on ? "true" : "false", hub_offline ? "true" : "false",
        (long long)(quiet / 1000), linked,
        netp2pnet_has_global_ipv6() ? "true" : "false", PEER_MAX, registered,
        audio_bytes, (unsigned)s_drop_full, s_peers_dirty ? "true" : "false",
        s_access_error ? "false" : "true", (unsigned)s_denied_count, NP_DENY_LIMIT,
        s_hub_on ? "hub" : hubs ? "member" : "direct",
        s_hub_on && hubs ? "true" : "false", direct, indirect);
    if (off >= n) { off = n - 1; }
    bool first = true;
    size_t peer_start[SLOT_MAX];
    int peer_marks = 0;
    for (int i = 0; i < SLOT_MAX && off + 1 < n; ++i) {
        rt_peer_t *p = &s_peer[i];
        if (!p->in_use || p->selftest) {
            continue;
        }
        char ipb[48] = "", name_esc[32], via_esc[32] = "";
        if (!p->via_hub && peer_addr_set(p)) { peer_addr_text(p, ipb, sizeof(ipb)); }
        if (p->via_hub) {
            const rt_peer_t *upstream = peer_by_mac(p->hub_mac);
            if (upstream) { json_escape_str_n(upstream->name, via_esc, sizeof(via_esc)); }
        }
        json_escape_str_n(p->name, name_esc, sizeof(name_esc));
        if (peer_marks < SLOT_MAX) {
            peer_start[peer_marks++] = off;
        }
        off += (size_t)snprintf(buf + off, n - off,
            "%s{\"name\":\"%s\",\"ip\":\"%s\",\"port\":%u,\"state\":\"%s\","
            "\"rtt_ms\":%d,\"rx\":%u,\"tx\":%u,\"drop\":%u,\"audio_idle_s\":%lld,"
            "\"route\":\"%s\",\"via\":\"%s\",\"hub\":%s}",
            first ? "" : ",", name_esc, ipb, (unsigned)p->port,
            peer_state_str(p), p->rtt_ms,
            (unsigned)p->rx_msgs, (unsigned)p->tx_msgs, (unsigned)p->rx_drop,
            (long long)(p->last_rx_ms ? (now_ms() - p->last_rx_ms) / 1000 : -1),
            p->via_hub ? "hub" : "direct", via_esc, p->hub ? "true" : "false");
        first = false;
        if (off >= n) { off = n - 1; }
    }
    /* Always close the peer array and the object.  When the buffer ran out
     * inside the last peer, step back to the last complete entry instead of
     * emitting an unterminated array: the caller appends the root brace, and a
     * broken document would take the whole /api/status response with it. */
    while (peer_marks > 0 && off + 3 > n) {
        peer_marks--;
        off = peer_start[peer_marks];
    }
    if (off + 3 > n) {
        off = (n >= 3) ? n - 3 : 0;
    }
    buf[off++] = ']';
    buf[off++] = '}';
    buf[off] = '\0';
    peer_unlock();
}

/* Separate bounded endpoint: 64 refusals must not crowd the main status/audio poll. */
esp_err_t netp2pnet_denied_json(char *buf, size_t n)
{
    if (!buf || n < 32) { return ESP_ERR_INVALID_ARG; }
    peer_lock();
    size_t off = (size_t)snprintf(buf, n, "{\"count\":%u,\"limit\":%u,\"ready\":%s,\"entries\":[",
        (unsigned)s_denied_count, NP_DENY_LIMIT, s_access_error ? "false" : "true");
    for (size_t i = 0; i < s_denied_count; ++i) {
        const np_access_record_t *r = &s_denied[i];
        char id[56], name[32], address[48] = "";
        np_access_id(r, id, sizeof(id)); json_escape_str_n(r->peer.name, name, sizeof(name));
        if (r->peer.family == 6) {
            struct in6_addr a; memcpy(&a, r->peer.address, 16); inet_ntop(AF_INET6, &a, address, sizeof(address));
        } else if (r->peer.family == 4) {
            struct in_addr a = {.s_addr = be32(r->peer.address)}; inet_ntoa_r(a, address, sizeof(address));
        }
        if (off >= n) { peer_unlock(); return ESP_ERR_NO_MEM; }
        int wrote = snprintf(buf + off, n - off,
            "%s{\"id\":\"%s\",\"name\":\"%s\",\"ip\":\"%s\",\"port\":%u,\"identity\":%s}",
            i ? "," : "", id, name, address, (unsigned)r->peer.port,
            np_access_has_mac(r->mac) ? "true" : "false");
        if (wrote < 0 || (size_t)wrote >= n - off) { peer_unlock(); return ESP_ERR_NO_MEM; }
        off += (size_t)wrote;
    }
    if (off + 3 > n) { peer_unlock(); return ESP_ERR_NO_MEM; }
    memcpy(buf + off, "]}", 3); peer_unlock(); return ESP_OK;
}
