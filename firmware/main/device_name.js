/* Shared handset name. Stable device_id remains the identity. */
(() => {
  'use strict';
  const el = id => document.getElementById(id);
  let id = '', revision = 0, editRevision = 0, dirty = false, saving = false, activity = '待机';
  function accept(d) {
    if (!d || !d.device_id || (id && id !== d.device_id)) return;
    id = d.device_id;
    if (typeof d.custom_name !== 'string' || !Number.isSafeInteger(d.name_revision)) return;
    revision = d.name_revision;
    const display = d.custom_name || '黑猫智能手台-' + id.slice(-6).toUpperCase();
    el('handsetName').textContent = display;
    el('handsetIdentity').textContent = '设备尾号 ' + id.slice(-6).toUpperCase() + ' · ' + location.host;
    document.title = display + ' · 黑猫控制面板';
    if (!dirty && !saving) { el('handsetNameInput').value = d.custom_name; editRevision = revision; }
    if (d.ptt) activity = (d.fp && (d.fp.rf_tx || d.fp.net_tx)) || d.ptt.startsWith('TX') || d.ptt === 'TAIL' ? '发话中' :
      d.ptt === 'PREPARE' ? '准备发话' : d.ptt === 'FAULT' ? '故障' :
      d.fp && (d.fp.rf_rx || d.fp.net_rx) ? '接收中' : d.sdr && d.sdr.on ? '收听中' : '待机';
    el('handsetUse').textContent = display + ' · ' + activity;
  }
  window.HMDeviceName = {accept};
  el('handsetNameInput').addEventListener('input', () => { dirty = true; });
  el('handsetNameSave').addEventListener('click', async () => {
    if (!id || saving) return;
    const name = el('handsetNameInput').value.trim().normalize('NFC');
    if ([...name].length > 24 || new TextEncoder().encode(name).length > 96 || /[\u0000-\u001f\u007f-\u009f]/u.test(name)) {
      el('handsetNameResult').textContent = '名称最多 24 字，不能含控制字符'; return;
    }
    saving = true; el('handsetNameSave').disabled = true;
    try {
      const response = await fetch('/api/device/name', {method:'POST', headers:{'Content-Type':'application/json'},
        body:JSON.stringify({custom_name:name, name_revision:editRevision})});
      const result = await response.json();
      if (response.status === 409) {
        const fresh = await fetch('/api/device', {cache:'no-store'});
        if (fresh.ok) { accept(await fresh.json()); editRevision = revision; }
        throw Error('名称已变化或存储不可用；已刷新当前名称，请核对后重试');
      }
      if (!response.ok || !result.ok || result.device_id !== id) throw Error(result.err || '保存未成功');
      dirty = false; saving = false; accept(result);
      el('handsetNameResult').textContent = '已保存到手台，其他客户端刷新后同步';
    } catch(e) { el('handsetNameResult').textContent = e.message; }
    finally { saving = false; el('handsetNameSave').disabled = false; }
  });
  const ready = window.ORLControl ? window.ORLControl.identity : fetch('/api/device').then(r => r.json());
  ready.then(accept).catch(e => {el('handsetNameResult').textContent = '名称读取失败：' + e.message;});
})();
