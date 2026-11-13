"""Production RF transactions and bounded idle recovery; no device access."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import test_radio_profile as fixture

ROOT=Path(__file__).resolve().parents[1]
MAIN=ROOT/'firmware/main'

def main():
    prefix=fixture.HARNESS[:fixture.HARNESS.index('int main(int argc, char **argv)')]
    prefix=prefix.replace('int64_t esp_timer_get_time(void) { return 0; }',
        'static int64_t clock_us=1; static bool rf_busy, net_busy, tail_busy;\nint64_t esp_timer_get_time(void) { return clock_us; }')
    prefix=prefix.replace('bool voice_rf_carrier_present(void) { return false; }','bool voice_rf_carrier_present(void) { return rf_busy; }')
    prefix=prefix.replace('bool voice_network_busy(void) { return false; }','bool voice_network_busy(void) { return net_busy; }')
    prefix=prefix.replace('bool voice_sdr_tail_active(void) { return false; }','bool voice_sdr_tail_active(void) { return tail_busy; }')
    code=prefix+r'''
static void idle_attempt(void) {
    clock_us+=1;sa818_cfg_refresh_tick();
    clock_us+=1000001;sa818_cfg_refresh_tick();
    clock_us+=10000001;sa818_cfg_refresh_tick();
}
static void baseline(void) {
    fail_ack=0;fail_store=false;
    sa818_cfg_t c;sa818_cfg_defaults(&c);c.tx_mhz=c.rx_mhz=438.5f;
    char log[160];assert(sa818_cfg_update(&c,log,sizeof(log))==ESP_OK);
}
int main(void) {
    assert(board_safe_state_init()==ESP_OK && sa818_cfg_init()==ESP_OK);
    assert(sa818_cfg_set_band(SA818_BAND_UHF)==ESP_OK);
    sa818_cfg_t c,a,b,after;char log[160];sa818_cfg_status_t status;
    sa818_cfg_defaults(&c);c.tx_mhz=c.rx_mhz=438.5f;
    assert(sa818_cfg_update_fields(&c,SA818_EDIT_TX|SA818_EDIT_RX,log,sizeof(log))==ESP_OK);
    assert(sa818_cfg_load(&a)==ESP_OK&&sa818_cfg_load(&b)==ESP_OK);
    const sa818_cfg_t before_a=a,before_b=b;
    strcpy(a.tx_ctcss,"0012");strcpy(b.rx_ctcss,"0008");
    assert(sa818_cfg_update_changed(&before_a,&a,log,sizeof(log))==ESP_OK);
    assert(sa818_cfg_update_changed(&before_b,&b,log,sizeof(log))==ESP_OK);
    assert(sa818_cfg_load(&after)==ESP_OK&&!strcmp(after.tx_ctcss,"0012")&&!strcmp(after.rx_ctcss,"0008"));
    puts("PASS: fresh profile creation and interleaved partial edits preserve both TX/RX");

    a=after;b=after;strcpy(b.tx_ctcss,"0000");
    assert(sa818_cfg_update_fields(&b,SA818_EDIT_TXT,log,sizeof(log))==ESP_OK);
    assert(sa818_cfg_update_fields(&a,SA818_EDIT_TXT,log,sizeof(log))==ESP_OK);
    assert(sa818_cfg_load(&after)==ESP_OK&&!strcmp(after.tx_ctcss,"0012"));
    unsigned count=commands;
    assert(sa818_cfg_update_fields(&a,4096,log,sizeof(log))==ESP_ERR_INVALID_ARG&&commands==count);
    sa818_cfg_status(&status);assert(status.ready&&status.revision==status.applied_revision);
    sa818_cfg_arm_refresh();idle_attempt();assert(commands==count);
    puts("PASS: explicit same-value intent, revision/ACK state, and no healthy periodic retune");

    for(int step=1;step<=4;step++) {
        baseline();assert(sa818_cfg_load(&a)==ESP_OK);strcpy(a.tx_ctcss,"0012");fail_ack=step;
        assert(sa818_cfg_update_fields(&a,SA818_EDIT_TXT,log,sizeof(log))!=ESP_OK);
        sa818_cfg_status(&status);assert(!status.ready&&status.error&&status.recovery_pending);
        assert(status.stage==(unsigned)(SA818_APPLY_GROUP+step-1));
        assert(sa818_cfg_load(&after)==ESP_OK&&!strcmp(after.tx_ctcss,"0000"));
        fail_ack=0;unsigned writes=blob_writes;idle_attempt();
        sa818_cfg_status(&status);assert(status.ready&&!status.recovery_pending&&!status.maintenance_pending);
        assert(status.applied_revision==status.revision&&blob_writes==writes);
        assert(!strcmp(last_group.tx_ctcss,"0000"));
    }
    puts("PASS: all four AT failures visible; recovery applies saved whole profile without NVS rewrite");

    baseline();assert(sa818_cfg_load(&a)==ESP_OK);strcpy(a.tx_ctcss,"0012");fail_store=true;
    assert(sa818_cfg_update_fields(&a,SA818_EDIT_TXT,log,sizeof(log))!=ESP_OK);
    sa818_cfg_status(&status);assert(status.stage==SA818_APPLY_STORAGE&&!status.ready);
    fail_store=false;unsigned writes=blob_writes;idle_attempt();assert(blob_writes==writes&&sa818_cfg_tx_ready());

    for(int busy=0;busy<4;busy++) {
        baseline();assert(sa818_cfg_load(&a)==ESP_OK);fail_ack=2;
        assert(sa818_cfg_update_fields(&a,SA818_EDIT_TXT,log,sizeof(log))!=ESP_OK);fail_ack=0;
        rf_busy=busy==0;net_busy=busy==1;tail_busy=busy==2;
        if(busy==3){pin_level[PIN_SA818_PTT]=0;s_ptt_tx=true;}
        count=commands;idle_attempt();assert(commands==count);
        rf_busy=net_busy=tail_busy=false;pin_level[PIN_SA818_PTT]=1;s_ptt_tx=false;
        idle_attempt();assert(sa818_cfg_tx_ready());
    }
    puts("PASS: RF COS, network pending, tail and physical PTT each defer recovery");

    baseline();assert(sa818_cfg_load(&a)==ESP_OK);fail_ack=2;
    assert(sa818_cfg_update_fields(&a,SA818_EDIT_TXT,log,sizeof(log))!=ESP_OK);
    sa818_cfg_status(&status);unsigned attempts=status.attempts;
    idle_attempt();idle_attempt();idle_attempt();sa818_cfg_status(&status);
    assert(status.attempts==attempts+3&&!status.ready&&status.recovery_pending&&!status.maintenance_pending);
    count=commands;idle_attempt();idle_attempt();assert(commands==count);
    fail_ack=0;assert(sa818_cfg_update_fields(&a,SA818_EDIT_TXT,log,sizeof(log))==ESP_OK);
    assert(sa818_cfg_tx_ready());
    puts("PASS: bounded three-attempt recovery; failure remains visible and manual valid edit recovers");
    return 0;
}
'''
    scratch=ROOT/'.tmp';scratch.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='resource-profile-',dir=scratch) as temp:
        d=Path(temp);(d/'freertos').mkdir()
        for name in ('sa818_cfg.c','sa818_cfg.h','sa818_freq.c','sa818_freq.h','sa818.h','ctcss.c','ctcss.h'):
            shutil.copy2(MAIN/name,d/name)
        (d/'stubs.h').write_text(fixture.STUBS,encoding='utf-8')
        for name in ('esp_err.h','esp_log.h','nvs.h','nvs_flash.h','board_hw.h','bridge.h','ptt.h','voice.h','esp_timer.h','freertos/FreeRTOS.h','freertos/semphr.h'):
            (d/name).write_text('#include "stubs.h"\n',encoding='utf-8')
        board=(MAIN/'board_hw.c').read_text('utf-8')
        state=board[board.index('static bool s_ptt_tx;'):board.index('/* ------------------------------------------------------------------ safety */')]
        safe=board[board.index('esp_err_t board_safe_state_init('):board.index('/* --------------------------------------------------------------------- LED */')]
        power=board[board.index('void board_sa818_power('):board.index('/* --------------------------------------------------------------------- I2C */')]
        (d/'board_power.c').write_text('static bool s_amp_on;\n'+state+safe+power,encoding='utf-8')
        (d/'test.c').write_text(code,encoding='utf-8');exe=d/'test.exe'
        env=dict(os.environ,TEMP=temp,TMP=temp,TMPDIR=temp)
        subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function','-I',str(d),str(d/'test.c'),str(d/'sa818_freq.c'),str(d/'ctcss.c'),'-lm','-o',str(exe)],check=True,env=env)
        subprocess.run([str(exe)],check=True,env=env)

if __name__=='__main__':main()
