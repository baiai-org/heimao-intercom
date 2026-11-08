"""Execute production Wi-Fi and lease policies with a deterministic clock.
No board, Wi-Fi configuration or RF transmission is involved.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
HARNESS = r'''
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "wifi_policy.h"
#include "control_lease.h"
static wifi_profile_store_t saved;
static wifi_policy_t policy;
static void network(int i, const char *name, int priority, bool enabled) {
    saved.profiles[i].id=i+1; saved.profiles[i].auto_join=enabled;
    saved.profiles[i].priority=priority;
    snprintf(saved.profiles[i].ssid,33,"%s",name);
}
int main(void) {
    saved.version=1; saved.next_id=4;
    network(0,"router",0,true); network(1,"phone",0,true); network(2,"disabled",100,false);
    assert(wifi_profiles_valid(&saved));
    wifi_policy_init(&policy,0);
    assert(!wifi_policy_ap_due(&policy,59999));
    assert(wifi_policy_ap_due(&policy,60000)); /* boot time zero is not a sentinel */
    wifi_policy_seen(&policy,0,-80); wifi_policy_seen(&policy,1,-45); wifi_policy_seen(&policy,2,-20);
    assert(wifi_policy_pick(&policy,&saved,0)==1); /* never choose a disabled network */
    saved.profiles[0].priority=1;
    assert(wifi_policy_pick(&policy,&saved,0)==0);
    saved.profiles[0].priority=0;
    wifi_policy_begin(&policy,1,1000); assert(policy.attempt_until==21000);
    wifi_policy_failed(&policy,2000); assert(policy.candidates[1].retry_at==32000);
    assert(policy.offline_since==0 && wifi_policy_pick(&policy,&saved,2000)==0);
    wifi_policy_begin(&policy,0,55000);
    assert(policy.attempt_until==60000 && policy.offline_since==0);
    wifi_policy_failed(&policy,60000);
    assert(wifi_policy_ap_due(&policy,60000));
    policy.recovery_ap=true;
    wifi_policy_begin(&policy,1,60001); assert(policy.attempt_until==80001);
    wifi_policy_failed(&policy,61000); assert(policy.candidates[1].retry_at==121000);
    wifi_policy_begin(&policy,1,121000); wifi_policy_failed(&policy,122000);
    assert(policy.candidates[1].retry_at==242000);
    wifi_policy_begin(&policy,1,242000); wifi_policy_online(&policy,243000);
    assert(policy.offline_since==-1 && !policy.recovery_ap && !policy.candidates[1].failures);
    assert(!wifi_policy_ap_due(&policy,999999999)); /* no WAN health input */
    wifi_policy_offline(&policy,300000); wifi_policy_offline(&policy,310000);
    assert(policy.offline_since==300000 && !wifi_policy_ap_due(&policy,359999));
    assert(wifi_policy_ap_due(&policy,360000));
    wifi_policy_scan_clear(&policy); assert(wifi_policy_pick(&policy,&saved,400000)==-1);
    wifi_policy_seen(&policy,1,-70); wifi_policy_seen(&policy,1,-90);
    assert(policy.candidates[1].rssi==-70);
    saved.profiles[1].auto_join=false;
    assert(wifi_policy_pick(&policy,&saved,400000)==-1);
    saved.profiles[1].auto_join=true;
    memset(&saved.profiles[1],0,sizeof(saved.profiles[1]));
    assert(wifi_policy_pick(&policy,&saved,400000)==-1); /* forgotten profile cannot reappear */
    assert(wifi_profiles_find(&saved,0)==-1 && wifi_profiles_find(&saved,2)==-1);
    saved.profiles[1]=saved.profiles[0]; assert(!wifi_profiles_valid(&saved));
    memset(&saved.profiles[1],0,sizeof(saved.profiles[1]));
    memset(saved.profiles[0].ssid,'x',33); assert(!wifi_profiles_valid(&saved));
    network(0,"router",0,true); saved.version=2; assert(!wifi_profiles_valid(&saved));
    saved.version=1; assert(wifi_profiles_valid(&saved));
    wifi_policy_init(&policy,(int64_t)UINT32_MAX+1000);
    assert(wifi_policy_ap_due(&policy,(int64_t)UINT32_MAX+61000));
    /* The previous client cannot revive a released/replaced lease. Deadline
     * checks are exact and only an explicit renewal extends expiry. */
    control_lease_t lease={0}; char a[65],b[65];
    memset(a,'a',64);a[64]=0;memset(b,'b',64);b[64]=0;
    assert(control_lease_grant(&lease,"A","Phone A",a,0));
    assert(lease.generation==1 && control_lease_valid(&lease,a,14999));
    assert(!control_lease_valid(&lease,a,15000) && control_lease_expired(&lease,15000));
    assert(!control_lease_valid(&lease,b,1));
    assert(!control_lease_grant(&lease,"B","Phone B",b,1));
    control_lease_release(&lease); assert(!control_lease_valid(&lease,a,1));
    assert(!*lease.token && !*lease.client);
    assert(control_lease_grant(&lease,"B","Phone B",b,15000));
    assert(lease.generation==2 && control_lease_valid(&lease,b,15001));
    assert(!control_lease_valid(&lease,a,15001));
    lease.expires_at=45000;
    assert(control_lease_valid(&lease,b,44999) && !control_lease_valid(&lease,b,45000));
    control_lease_release(&lease);lease.generation=UINT32_MAX;
    assert(!control_lease_grant(&lease,"A","A",a,0)); /* no generation wrap */
    puts("PASS: saved candidate selection, disabled/forgotten networks, bounded 60 s recovery, 20 s attempts, per-network cooldown, DHCP success and 64-bit uptime; exclusive lease expiry, release, replacement, token rejection and generation exhaustion");
}
'''

def main():
    compiler = shutil.which('gcc')
    if not compiler:
        raise SystemExit('Host GCC required')
    scratch = ROOT / '.tmp'
    scratch.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='wifi-control-', dir=scratch) as temp:
        source = Path(temp) / 'test.c'
        source.write_text(HARNESS, encoding='utf-8')
        env = dict(os.environ, TEMP=temp, TMP=temp, TMPDIR=temp)
        main_dir = ROOT / 'firmware/main'
        exe = Path(temp) / 'test.exe'
        subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(main_dir),
                        str(source), str(main_dir/'wifi_policy.c'), str(main_dir/'control_lease.c'),
                        '-o', str(exe)], check=True, env=env)
        subprocess.run([str(exe)], check=True, env=env)

if __name__ == '__main__':
    main()
