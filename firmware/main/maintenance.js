/* Device maintenance and stable-identity peer labels. No mutation is retried. */
(() => {
  'use strict';
  const el = id => document.getElementById(id);
  let identity, current, dirty = false, saving = false, polling = false, waitingBoot = false;
  const busyText = {idle:'空闲',writing:'正在写入文件',transmitting:'正在发射或测试',receiving:'正在接收',audio_pending:'音频尚未结束'};
  const weekdays = ['周一','周二','周三','周四','周五','周六','周日'];
  const clockLabels = {waiting:'等待首次校时',synced:'已校时',holdover:'暂用上次校准的时钟',stale:'校时已过期'};
  const chinaTime = new Intl.DateTimeFormat('zh-CN', {timeZone:'Asia/Shanghai', month:'2-digit', day:'2-digit',
    weekday:'short', hour:'2-digit', minute:'2-digit', hourCycle:'h23'});
  const showTime = utc => utc > 0 ? chinaTime.format(new Date(utc * 1000)) : '—';
  const weekdayInputs = () => [...el('maintenanceDays').querySelectorAll('input')];
  const hhmm = minute => [Math.floor(minute / 60), minute % 60].map(value => String(value).padStart(2,'0')).join(':');
  const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
  async function request(path, body, timeout = 8000) {
    const d = identity || await window.ORLControl.identity;
    const controller = new AbortController(), timer = setTimeout(() => controller.abort(), timeout);
    try {
      const response = await fetch(path, {method:body ? 'POST':'GET', cache:'no-store', signal:controller.signal,
        headers:{'Content-Type':'application/json','X-ORL-Device':d.device_id}, ...(body ? {body:JSON.stringify(body)} : {})});
      const data = await response.json();
      if (!response.ok || data.ok === false) {
        const error = new Error(data.err || 'HTTP '+response.status); error.data = data; throw error;
      }
      if (data.device_id && data.device_id !== d.device_id) throw Error('设备身份已变化，请重新连接');
      return data;
    } finally { clearTimeout(timer); }
  }
  function note(text, bad = false) {
    if (el('maintenanceMessage')) { el('maintenanceMessage').textContent=text; el('maintenanceMessage').style.color=bad?'#f87171':''; }
  }
  function setControls() {
    const ready = current?.ready === true;
    for (const id of ['maintenanceCleanup','maintenanceRestart','maintenanceTime','maintenanceSave','maintenanceCleanNow']) {
      if (el(id)) el(id).disabled=!ready || saving || waitingBoot || (current?.reboot_pending && id!=='maintenanceSave');
    }
    if (el('maintenanceDays')) {
      for (const input of weekdayInputs()) input.disabled=el('maintenanceTime').disabled;
    }
    if (el('facWipe')) el('facWipe').disabled=!current?.boot_id || saving || waitingBoot || current?.reboot_pending;
    if (el('maintenanceReload')) el('maintenanceReload').disabled=saving || waitingBoot;
    if (el('maintenanceReboot')) el('maintenanceReboot').disabled=!current?.boot_id || saving || waitingBoot || current?.reboot_pending;
  }
  function render(data) {
    if (data.version !== 2 || !data.device_id || !data.boot_id) throw Error('维护状态不完整，请重新读取');
    if (current?.boot_id === data.boot_id && current.revision > data.revision) return;
    current=data;
    if (!dirty) {
      el('maintenanceCleanup').checked=data.cleanup_enabled;
      el('maintenanceRestart').checked=data.restart_enabled;
      el('maintenanceTime').value=hhmm(data.restart_minute);
      weekdayInputs().forEach((input, day) => { input.checked=Boolean(data.restart_days & (1 << day)); });
      el('maintenanceForm').dataset.revision=String(data.revision);
    }
    const schedule=data.reboot_pending?'重启请求已接受':!data.restart_enabled?'定时重启已关闭':
      data.restart_save_failed?'执行记录写入失败，正在重试':
      data.restart_waiting?'已到点，等待写入结束或 BOOT 松开':
      data.next_restart_utc>0?'下次重启 '+showTime(data.next_restart_utc):'等待校时后安排重启';
    el('maintenanceState').textContent=schedule+' · '+(busyText[data.busy] || data.busy)+' · 连续空闲 '+(data.idle_s || 0)+'秒';
    el('maintenanceClock').textContent=(clockLabels[data.clock_state] || '校时状态未知')+
      ' · 最近校时 '+showTime(data.last_sync_utc)+'（北京时间）';
    const result={not_run:'尚未执行',reclaimed:'已回收过期资源',nothing_to_reclaim:'无需清理'};
    el('maintenanceStats').textContent=(result[data.result] || data.result || '')+' · 检查 '+(data.cleanup_runs || 0)+'次 · 延后 '+(data.cleanup_delays || 0)+'次'+
      ' · 上次释放缓冲 '+(data.released_buffer_bytes || 0)+'字节 / 解码器 '+(data.released_decoders || 0)+'个'+
      ' · 上次检查：'+(data.last_cleanup_uptime_s ? '启动后'+data.last_cleanup_uptime_s+'秒':'尚未执行')+
      ' · 内部可用 '+data.internal_free+'字节 / 最大连续 '+data.internal_largest+'字节 · PSRAM可用 '+data.psram_free+'字节'+
      ' · 最近启动：'+({manual:'手动重启',scheduled:'定时重启',factory:'恢复出厂',other:'上电或其他原因'}[data.last_reboot] || '未知');
    if (!data.ready) note('维护配置读取失败，已停止自动维护，请检查诊断。', true);
    setControls();
  }
  async function refresh() {
    if (polling || waitingBoot || saving) return;
    polling=true;
    try {
      const data=await request('/api/maintenance');
      if (!saving && !waitingBoot) render(data);
    }
    catch (e) { note('维护状态读取失败：'+e.message, true); }
    finally { polling=false; }
  }
  async function waitForBoot(oldBoot) {
    waitingBoot=true; setControls();
    note('重启请求已接受，正在等待同一台设备重新上线…');
    const deadline=Date.now()+60000;
    while (Date.now()<deadline) {
      await sleep(1500);
      try {
        const data=await request('/api/maintenance',null,2500);
        if (data.boot_id && data.boot_id!==oldBoot) {
          dirty=false; render(data); note('设备已重新上线，启动标识已更新。');
          if (typeof window.refresh==='function') window.refresh();
          waitingBoot=false; setControls(); return;
        }
      } catch (e) { if (/身份已变化/.test(e.message)) { note(e.message,true); waitingBoot=false;setControls();return; } }
    }
    waitingBoot=false; setControls(); note('尚未确认重启完成，请检查网络后点击“重新读取”；不会自动重发重启指令。',true);
  }
  async function reboot() {
    if (saving || waitingBoot || !current?.boot_id) return;
    const name=window._st?.custom_name || identity.custom_name || identity.device_id;
    if (!confirm('重启当前手台「'+name+'」？\n地址：'+location.host+'\n将中断当前通联，保留已保存配置。')) return;
    saving=true; setControls();
    try {
      const old=current.boot_id;
      await request('/api/maintenance',{op:'reboot',boot_id:old});
      saving=false; await waitForBoot(old);
    } catch (e) { note('重启未确认：'+e.message+'；请重新读取状态后再决定是否重试。',true); }
    finally { saving=false; setControls(); }
  }
  async function factoryReset() {
    if (saving || waitingBoot || !current?.boot_id) return;
    const name=window._st?.custom_name || identity.custom_name || identity.device_id;
    if (!confirm('彻底恢复「'+name+'」的出厂设置？\n将清空板上的 Wi-Fi、信道、尾音及其他设置。恢复后请连接手台配网热点。')) return;
    saving=true; setControls();
    try {
      await request('/api/maintenance',{op:'factory_reset',boot_id:current.boot_id});
      waitingBoot=true;
      note('恢复请求已接受。擦除校验完成后，手台将开启配网热点。');
    } catch (e) {
      note('恢复请求未确认：'+e.message+'。若原地址断开，请查找手台配网热点。',true);
    } finally {
      saving=false; setControls();
    }
  }
  function mount() {
    if (el('maintenanceForm')) return;
    const card=document.createElement('div');card.id='maintenanceBox';card.className='subcard';
    card.innerHTML='<div class="subcard-head">设备自动维护与重启</div>'+
      '<div id="maintenanceForm"><label><input id="maintenanceCleanup" type="checkbox"> 启用自动资源维护</label>'+
      '<p class="hint">每5分钟检查；连续空闲60秒后回收过期资源，保留配置及有效音频缓存。</p>'+
      '<label><input id="maintenanceRestart" type="checkbox"> 启用定时重启</label>'+
      '<div id="maintenanceDays" role="group" aria-label="重启星期">'+weekdays.map(day =>
        '<label><input type="checkbox"> '+day+'</label>').join('')+'</div>'+
      '<div class="row"><label for="maintenanceTime">重启时间</label><input id="maintenanceTime" type="time" value="04:00" step="60"></div>'+
      '<p class="hint">按北京时间到点重启，当前通联会结束。正在升级或写入文件时顺延。</p>'+
      '<div class="row"><button id="maintenanceSave" class="pri" type="button">保存维护设置</button><button id="maintenanceReload" type="button">重新读取</button><button id="maintenanceCleanNow" type="button">立即检查并回收</button></div></div>'+
      '<p id="maintenanceState" role="status">正在读取…</p><p id="maintenanceClock" class="hint"></p><p id="maintenanceMessage" role="status"></p><p id="maintenanceStats" class="hint" style="overflow-wrap:anywhere"></p>';
    el('log').closest('.card').append(card);
    const button=document.createElement('button');button.id='maintenanceReboot';button.type='button';button.className='warn-btn';button.textContent='重启本机';
    button.style.cssText='min-height:48px;padding:10px 16px;font-size:16px;white-space:nowrap';el('panelLayoutToggle').after(button);button.onclick=reboot;
    if (el('facWipe')) el('facWipe').onclick=factoryReset;
    const style=document.createElement('style');style.textContent='#maintenanceDays{display:flex;gap:8px 14px;flex-wrap:wrap;margin:8px 0}#maintenanceBox input[type=time]{max-width:130px}#maintenanceBox label{white-space:normal}#maintenanceBox .row{flex-wrap:wrap}#maintenanceBox .row>label{flex:0 0 90px;min-width:90px}#maintenanceBox button{min-height:44px}#maintenanceBox{margin-top:16px}#maintenanceBox input[type=checkbox]{width:18px;height:18px;vertical-align:middle}';document.head.append(style);
    el('maintenanceForm').addEventListener('input',()=>{dirty=true;note('有未保存的维护设置');});
    el('maintenanceReload').onclick=()=>{if(saving || waitingBoot)return;dirty=false;refresh();};
    el('maintenanceSave').onclick=async()=>{
      const value=el('maintenanceTime').value;
      const restart=el('maintenanceRestart').checked;
      const days=weekdayInputs().reduce((mask,input,day) => mask | (input.checked ? 1 << day : 0),0);
      if(!/^(?:[01][0-9]|2[0-3]):[0-5][0-9]$/.test(value) || (restart && !days)) {
        note('请填写有效的时间，并为已启用的计划至少选择一天。',true);return;
      }
      const [hour,minute]=value.split(':').map(Number);
      saving=true;setControls();
      try {const data=await request('/api/maintenance',{op:'set',revision:Number(el('maintenanceForm').dataset.revision),
        cleanup_enabled:el('maintenanceCleanup').checked,restart_enabled:restart,restart_days:days,restart_minute:hour*60+minute});
        dirty=false;render(data);note('维护设置已确认并保存。');
      }catch(e){note('保存失败，草稿已保留：'+e.message,true);}finally{saving=false;setControls();}
    };
    el('maintenanceCleanNow').onclick=async()=>{
      saving=true;setControls();
      try{render(await request('/api/maintenance',{op:'cleanup'}));note('检查完成。');}
      catch(e){note('未执行回收：'+e.message,true);}finally{saving=false;setControls();}
    };
    setControls();refresh();setInterval(refresh,10000);
  }
  function validateName(name) {
    if ([...name].length>24 || new TextEncoder().encode(name).length>96 || /[\u0000-\u001f\u007f-\u009f]/u.test(name)) throw Error('名称最多24个字符/96字节，不能包含控制字符');
  }
  window.ORLPeerLabels={
    async add(name,address,restore,revision) {
      validateName(name);
      return request('/api/np2p/peer',{op:'add',name,address,restore,revision});
    },
    edit(peer,revision) {
      if (el('peerLabelDialog')) return;
      const dialog=document.createElement('dialog');dialog.id='peerLabelDialog';dialog.style.cssText='width:min(430px,90vw);box-sizing:border-box;background:#142333;color:#eef5ff;border:1px solid #52768d;border-radius:14px;padding:22px';
      dialog.innerHTML='<form><h3>修改对端名称</h3><p id="peerLabelAddress" style="overflow-wrap:anywhere"></p><label for="peerLabelInput">名称（最多24字符，留空恢复默认）</label><input id="peerLabelInput" maxlength="96" style="box-sizing:border-box;width:100%;margin:12px 0;font-size:16px">'+
        '<p id="peerLabelError" role="status" style="color:#f87171"></p><div style="display:flex;gap:12px"><button id="peerLabelSave" class="pri" type="submit">保存</button><button type="button" id="peerLabelCancel">取消</button></div></form>';
      document.body.append(dialog);el('peerLabelAddress').textContent=peer.ip?(peer.ip.includes(':')?'['+peer.ip+']':peer.ip)+':'+peer.port:'经枢纽转发';
      el('peerLabelInput').value=peer.name || '';el('peerLabelCancel').onclick=()=>dialog.close();dialog.onclose=()=>dialog.remove();
      dialog.querySelector('form').onsubmit=async event=>{
        event.preventDefault();const name=el('peerLabelInput').value.trim(),save=el('peerLabelSave');save.disabled=true;
        try{validateName(name);await request('/api/np2p/peer',{op:'rename',id:peer.id,name,revision});dialog.close();if(typeof window.refresh==='function')await window.refresh();}
        catch(e){el('peerLabelError').textContent=e.message;}finally{save.disabled=false;}
      };
      dialog.showModal();el('peerLabelInput').focus();
    }
  };
  window.ORLControl.identity.then(d=>{identity=d;if(d.capabilities?.maintenance===2)mount();}).catch(()=>{});
})();
