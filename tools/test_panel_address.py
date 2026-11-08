"""Compile and exercise the production mDNS -> panel-IP redirect helper."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include "panel_address.h"
#include <assert.h>
int main(void) {
    const struct {
        const char *host, *name, *ip;
        bool secure;
        const char *expected;
    } cases[] = {
        {"orl2.local", "orl2", "192.0.2.178", false, "http://192.0.2.178/"},
        {"ORL2.LOCAL.:80", "orl2", "192.0.2.178", false, "http://192.0.2.178/"},
        {"orl2.local", "orl2", "192.0.2.178", true, "https://192.0.2.178/"},
        {"orl2.local:443", "orl2", "192.168.3.77", true, "https://192.168.3.77/"},
        {"orl3.local", "orl3", "192.0.2.198", false, "http://192.0.2.198/"},
        {"orl2.local:443", "orl2", "192.0.2.178", false, NULL},
        {"orl2.local:80", "orl2", "192.0.2.178", true, NULL},
        {"orl2.local:8080", "orl2", "192.0.2.178", false, NULL},
        {"192.0.2.178", "orl2", "192.0.2.178", false, NULL},
        {"192.168.4.1", "orl2", "192.0.2.178", false, NULL},
        {"[fe80::1234]", "orl2", "192.0.2.178", true, NULL},
        {"panel.example.com", "orl2", "192.0.2.178", true, NULL},
        {"orl2.local.evil.example", "orl2", "192.0.2.178", false, NULL},
        {"orl2.local@evil.example", "orl2", "192.0.2.178", false, NULL},
        {"orl2.local", "orl3", "192.0.2.178", false, NULL},
        {"orl2.local", "orl2", "", false, NULL},
        {"orl2.local", "orl2", "0.0.0.0", false, NULL},
        {"orl2.local", "orl2", "127.0.0.1", false, NULL},
        {"orl2.local", "orl2", "999.1.1.1", false, NULL},
        {"orl2.local", "orl2", "192.0.2.178/", false, NULL},
        {"orl2.local", "orl2", "10.1.1.1\r\nX:x", false, NULL},
        {NULL, "orl2", "192.0.2.178", false, NULL},
        {"orl2.local", NULL, "192.0.2.178", false, NULL},
        {"orl2.local", "orl2", NULL, false, NULL},
    };
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
        char out[64] = "stale";
        const bool ok = panel_address_location(cases[i].host, cases[i].name,
            cases[i].ip, cases[i].secure, out, sizeof(out));
        assert(ok == (cases[i].expected != NULL));
        assert(!strcmp(out, cases[i].expected ? cases[i].expected : ""));
    }
    char tiny[4] = "old";
    assert(!panel_address_location("orl2.local", "orl2", "192.0.2.178", false, tiny, sizeof(tiny)));
    assert(tiny[0] == '\0');
    assert(!panel_address_location("orl2.local", "orl2", "192.0.2.178", false, NULL, 0));
    const struct { const char *ip; const char *expected; } https_cases[] = {
        {"192.0.2.178", "https://192.0.2.178/"},
        {"192.168.3.77", "https://192.168.3.77/"},
        {"0.0.0.0", NULL},
        {"127.0.0.1", NULL},
        {"255.1.1.1", NULL},
        {"999.1.1.1", NULL},
        {"192.0.2.178/", NULL},
        {"192.0.2.178:443", NULL},
        {"", NULL},
        {NULL, NULL},
    };
    for (size_t i = 0; i < sizeof(https_cases)/sizeof(https_cases[0]); ++i) {
        char out[64] = "stale";
        const bool ok = panel_https_location(https_cases[i].ip, out, sizeof(out));
        assert(ok == (https_cases[i].expected != NULL));
        assert(!strcmp(out, https_cases[i].expected ? https_cases[i].expected : ""));
    }
    char tiny2[4] = "old";
    assert(!panel_https_location("192.0.2.178", tiny2, sizeof(tiny2)));
    assert(tiny2[0] == '\0');
    puts("PASS: exact configured mDNS name, DHCP address, HTTP/HTTPS, no downgrade, no IP loop, unrelated/proxy hosts, invalid address and bounded output; HTTPS entry");
    return 0;
}
'''


def main():
    scratch = ROOT / '.tmp'
    scratch.mkdir(exist_ok=True)
    env = os.environ.copy()
    env.update(TMP=str(scratch), TEMP=str(scratch))
    with tempfile.TemporaryDirectory(prefix='panel-address-', dir=scratch) as tmp:
        p = Path(tmp)
        (p / 'test.c').write_text(HARNESS, encoding='utf-8')
        exe = p / 'test.exe'
        subprocess.run(['gcc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-I', str(ROOT / 'firmware/main'), str(p / 'test.c'), '-o', str(exe)],
                       env=env, check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
