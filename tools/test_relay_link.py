"""MQTT connection status and PTT readiness at production event boundaries."""
import os
from pathlib import Path
import subprocess
import tempfile
from test_tx_ownership import function

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "firmware/main"

ENV = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "netrelay.h"
#define portMUX_TYPE int
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define REL_PKT_LEN 32
#define REL_HDR_LEN 16
#define REL_AUDIO_OPUS 16
#define REL_MAGIC 7
typedef struct {int magic;uint8_t mac[6];} rel_header_t;
typedef void *esp_mqtt_client_handle_t;
typedef esp_mqtt_client_handle_t esp_mqtt5_client_handle_t;
typedef void *esp_event_base_t;
typedef struct {bool no_local_flag;} esp_mqtt5_subscribe_property_config_t;
#define ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME 0x8001
#include "net_endpoint.h"
typedef struct {int connect_return_code,error_type,esp_transport_sock_errno,esp_tls_stack_err,esp_tls_last_esp_err;} mqtt_error_t;
typedef struct {
 int event_id, msg_id, data_len, current_data_offset, total_data_len;
 esp_mqtt_client_handle_t client; const char *data; mqtt_error_t *error_handle;
} mqtt_event_t, *esp_mqtt_event_handle_t;
enum {MQTT_EVENT_BEFORE_CONNECT,MQTT_EVENT_ERROR,MQTT_EVENT_CONNECTED,
      MQTT_EVENT_SUBSCRIBED,MQTT_EVENT_DISCONNECTED,MQTT_EVENT_DATA};
enum {MQTT_CONNECTION_REFUSE_BAD_USERNAME=4,MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED=5};
static int64_t clock_ms, s_start_ms;
static int s_attempts;
static char s_last_err[48],s_uri[NET_ENDPOINT_SIZE],s_sub_topic[32],s_topic_root[24]="test";
static int mqtt_client, previous_client, sub_message_id=11;
static esp_mqtt_client_handle_t s_client=&mqtt_client;
static uint8_t s_mac[6];
static uint32_t s_ev_total,s_ev_badlen,s_ev_badmagic,s_ev_self,s_ev_ok;
static int64_t esp_timer_get_time(void) { return clock_ms*1000; }
static int esp_mqtt5_client_set_subscribe_property(void *p,void *props){return 0;}
static int esp_mqtt_client_subscribe(void *p,const char *topic,int qos){return sub_message_id;}
static void rx_lock(void){}
static void rx_unlock(void){}
static void peer_seen(const char *id){}
static void relay_store_packet(const rel_header_t *h,const uint8_t *bytes,size_t n){}

enum {NETCOM_MODE_LOCAL,NETCOM_MODE_NETP2P,NETCOM_MODE_RELAY};
static struct { int mode; } s_cfg;
static bool s_engine_up=true;
typedef struct {void (*stats)(uint32_t*,uint32_t*,int*,const char**);} netcom_engine_t;
static void stats(uint32_t *tx,uint32_t *rx,int *peers,const char **state) {
    *tx=0;*rx=1;*peers=5;*state="rx"; /* Simulate a recent packet after a disconnect. */
}
static netcom_engine_t engine={.stats=stats};
static const netcom_engine_t *active_engine(void){return &engine;}
'''

CHECK = r'''
static void event(int id,int message,const char *data,mqtt_error_t *error,void *client){
    mqtt_event_t e={.client=client,.event_id=id,.msg_id=message,.data=data,.data_len=data?1:0,.error_handle=error};
    relay_mqtt_handler(NULL,NULL,id,&e);
}
static void state(const char *phase,const char *reason){
    const netrelay_link_t r=netrelay_link_status();
    assert(!strcmp(r.state,phase)&&!strcmp(r.reason,reason));
}
int main(void){
    s_started=true;s_cfg.mode=NETCOM_MODE_RELAY;
    event(MQTT_EVENT_BEFORE_CONNECT,0,NULL,NULL,s_client);state("connecting","");
    assert(!netcom_tx_ready());
    event(MQTT_EVENT_CONNECTED,0,NULL,NULL,s_client);state("connecting","subscribing");
    assert(netcom_tx_ready());
    char success=0, refused=(char)0x87;
    event(MQTT_EVENT_SUBSCRIBED,99,&success,NULL,s_client);state("connecting","subscribing");
    event(MQTT_EVENT_SUBSCRIBED,11,&success,NULL,&previous_client);state("connecting","subscribing");
    clock_ms=8001;state("error","subscribe_timeout");
    event(MQTT_EVENT_SUBSCRIBED,11,&success,NULL,s_client);state("connected","");
    event(MQTT_EVENT_DISCONNECTED,0,NULL,NULL,s_client);state("disconnected","");
    assert(!netcom_tx_ready());
    event(MQTT_EVENT_SUBSCRIBED,11,&success,NULL,s_client);state("disconnected","");
    event(MQTT_EVENT_BEFORE_CONNECT,0,NULL,NULL,s_client);state("reconnecting","");
    sub_message_id=12;
    event(MQTT_EVENT_CONNECTED,0,NULL,NULL,s_client);
    event(MQTT_EVENT_SUBSCRIBED,12,&refused,NULL,s_client);state("error","subscribe_failed");
    const mqtt_error_t bad={.connect_return_code=0x86};
    event(MQTT_EVENT_ERROR,0,NULL,(mqtt_error_t*)&bad,s_client);state("error","authentication");
    event(MQTT_EVENT_DISCONNECTED,0,NULL,NULL,s_client);state("error","authentication");
    const mqtt_error_t dns={.esp_tls_last_esp_err=ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME};
    event(MQTT_EVENT_ERROR,0,NULL,(mqtt_error_t*)&dns,s_client);state("error","dns");
    assert(!netcom_tx_ready());
    s_cfg.mode=NETCOM_MODE_NETP2P;assert(netcom_tx_ready());
    puts("PASS: CONNACK/SUBACK, wrong client and message, subscription timeout/refusal, authentication, immediate disconnect and P2P readiness");
}
'''

def main():
    relay = (MAIN / "netrelay.c").read_text(encoding="utf-8")
    netcom = (MAIN / "netcom.c").read_text(encoding="utf-8")
    variables = relay[relay.index("static volatile bool     s_started;"):relay.index("/* Connection diagnostics")]
    code = ENV + variables + "".join(function(relay, name) for name in (
        "now_ms", "relay_link_set", "netrelay_link_status", "netrelay_broker_connected",
        "relay_subscribe", "relay_mqtt_handler"))
    code += function(netcom, "netcom_tx_ready") + CHECK
    with tempfile.TemporaryDirectory(prefix="relay-link-", dir=ROOT / ".tmp") as tmp:
        path = Path(tmp)
        source, binary = path / "test.c", path / "test.exe"
        source.write_text(code, encoding="utf-8")
        env = dict(os.environ,TEMP=tmp,TMP=tmp,TMPDIR=tmp)
        subprocess.run(["gcc","-std=c11","-I",str(MAIN),str(source),"-o",str(binary)], env=env, check=True)
        subprocess.run([str(binary)], env=env, check=True)

if __name__ == "__main__":
    main()
