"""Compile the build-local HTTPS patch against the actual pinned SDK source.
No shared SDK files are modified and no network connection is opened.
"""
from pathlib import Path
import argparse, os, subprocess, tempfile
from patch_https_server import patched_source
from test_np_peer_runtime import function
ROOT=Path(__file__).resolve().parents[1]
PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 1
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define HTTPS_SERVER_EVENT_ERROR 1
#define HTTPS_SERVER_EVENT_ON_CONNECTED 2
#define HTTPD_SSL_USER_CB_SESS_CREATE 3
typedef int esp_err_t;
typedef void *httpd_handle_t;
typedef struct { int live; } esp_tls_t;
typedef void *esp_tls_error_handle_t;
typedef struct { int last_error, esp_tls_error_code, esp_tls_flags; } esp_https_server_last_error_t;
typedef struct { int user_cb_state; esp_tls_t *tls; } esp_https_server_user_cb_arg_t;
typedef struct { void *tls_cfg; void (*open_fn)(void *,int); void (*user_cb)(void *); } httpd_ssl_ctx_t;
typedef struct { esp_tls_t *tls; httpd_ssl_ctx_t *global_ctx; } httpd_ssl_transport_ctx_t;
static httpd_ssl_ctx_t global;
static unsigned tls_created, tls_deleted, registered;
static bool fail_context, fail_handshake;
static void *attached;
static void *httpd_get_global_transport_ctx(httpd_handle_t h) { (void)h;return &global; }
static esp_tls_t *esp_tls_init(void) { esp_tls_t *p=calloc(1,sizeof(*p));assert(p);p->live=1;tls_created++;return p; }
static int esp_tls_server_session_create(void *c,int s,esp_tls_t *t) { (void)c;(void)s;(void)t;return fail_handshake?-1:0; }
static void esp_tls_server_session_delete(esp_tls_t *t) { assert(t->live);t->live=0;tls_deleted++;free(t); }
static void http_dispatch_event_to_event_loop(int e,const void *d,size_t n) { (void)e;(void)d;(void)n; }
static void httpd_ssl_close(void *p) { httpd_ssl_transport_ctx_t *ctx=p;esp_tls_server_session_delete(ctx->tls);free(ctx); }
static void httpd_sess_set_transport_ctx(void *h,int fd,void *p,void(*f)(void *)) { (void)h;(void)fd;(void)f;attached=p;registered++; }
static void httpd_ssl_send(void) {}
static void httpd_ssl_recv(void) {}
static void httpd_ssl_pending(void) {}
static void httpd_sess_set_send_override(void *h,int fd,void(*f)(void)) { (void)h;(void)fd;(void)f; }
static void httpd_sess_set_recv_override(void *h,int fd,void(*f)(void)) { (void)h;(void)fd;(void)f; }
static void httpd_sess_set_pending_override(void *h,int fd,void(*f)(void)) { (void)h;(void)fd;(void)f; }
static int esp_tls_get_error_handle(esp_tls_t *t,esp_tls_error_handle_t *p) { (void)t;*p=NULL;return ESP_OK; }
static int esp_tls_get_and_clear_last_error(esp_tls_error_handle_t h,int *a,int *b) { (void)h;*a=*b=0;return 0; }
static void *context_calloc(size_t n,size_t size) { return fail_context?NULL:calloc(n,size); }
#define calloc context_calloc
'''
TEST = r'''
#undef calloc
int main(void) {
    assert(httpd_ssl_open((void *)1,7)==ESP_OK && registered==1);
    httpd_ssl_close(attached);attached=NULL;
    assert(tls_created==tls_deleted);
    puts("CONTROL PASS: successful session attaches cleanup, closing frees TLS");
    fail_handshake=true;
    assert(httpd_ssl_open((void *)1,7)!=ESP_OK && tls_created==tls_deleted);
    puts("CONTROL PASS: handshake failure deletes TLS");
    fail_handshake=false;fail_context=true;
    unsigned baseline=tls_created-tls_deleted;
    for(int i=0;i<3;i++)assert(httpd_ssl_open((void *)1,7)==ESP_ERR_NO_MEM);
    assert(tls_created-tls_deleted==baseline && attached==NULL && registered==1);
    puts("PASS: context allocation failure after handshake frees TLS; repeated failures leave no live objects");
    return 0;
}
'''

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--source',type=Path,required=True)
    args=parser.parse_args();raw=args.source.read_bytes()
    patched=patched_source(raw).decode('utf-8')
    assert patched_source(raw)==patched_source(raw.replace(b'\r\n',b'\n'))
    try:patched_source(raw+b' ')
    except ValueError:pass
    else:raise AssertionError('Unsupported SDK source accepted')
    scratch=ROOT/'.tmp';scratch.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='https-cleanup-',dir=scratch) as temp:
        d=Path(temp);(d/'test.c').write_text(PREFIX+function(patched,'httpd_ssl_open')+TEST,encoding='utf-8');exe=d/'test.exe'
        env=dict(os.environ,TEMP=temp,TMP=temp,TMPDIR=temp)
        subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror',str(d/'test.c'),'-o',str(exe)],check=True,env=env)
        subprocess.run([str(exe)],check=True,env=env)
if __name__=='__main__':main()
