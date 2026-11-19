(() => {
  'use strict';
  const card = document.getElementById('rfDenoiseCard');
  const connection = document.getElementById('rfDenoiseStatus');
  const inactiveTx = document.getElementById('eqDenoiseTxInactive');
  const states = ['已关闭', '等待下一话段', '正在降噪', '内存不足 · 原声旁路', '处理超时 · 原声旁路'];
  const rows = [...card.querySelectorAll('[data-dn-key]')].map(row => ({
    row, key: row.dataset.dnKey, mode: Number(row.dataset.dnMode),
    button: row.querySelector('button'), detail: row.querySelector('small')
  }));
  const pending = new Set();
  const errors = new Map();
  let saved;
  let reading = false;
  let epoch = 0;
  let refreshWhenIdle = false;

  function text(node, value) {
    if (node.textContent !== value) node.textContent = value;
  }

  function attribute(node, name, value) {
    if (node.getAttribute(name) !== value) node.setAttribute(name, value);
  }

  function render() {
    if (!saved) return;
    for (const {row, key, mode, button, detail} of rows) {
      const network = mode <= 2;
      const visible = network ? saved.mode === mode : key !== 'handset' || saved.mode === 0 || saved.rf_tx;
      if (row.hidden === visible) row.hidden = !visible;
      if (key === 'handset' && inactiveTx.hidden !== visible) inactiveTx.hidden = visible;
      if (button.disabled) button.disabled = false;
      const isSaving = pending.has(key);
      attribute(button, 'aria-busy', String(isSaving));
      attribute(button, 'aria-pressed', String(saved[key]));
      if (button.classList.contains('on') !== saved[key]) button.classList.toggle('on', saved[key]);
      text(button, saved[key] ? '开启' : '关闭');
      const telemetry = network ? saved : saved[key + '_stats'];
      const action = isSaving ? '保存中…' : errors.get(key)?.message;
      if (action) {
        text(detail, action);
      } else if (network && saved[key] && !saved.bridge_active) {
        text(detail, '已开启 · 等待桥接');
      } else {
        const isActive = network ? saved.active_mode === mode : telemetry.state >= 2;
        const state = isActive ? telemetry.state : saved[key] ?
          (telemetry.state === 3 ? 3 : 1) : 0;
        const label = (saved[key] && state !== 0 ? '已开启 · ' : '') + (states[state] || '状态未知');
        text(detail, label + (telemetry.frames ? ' · 处理 ' + telemetry.frames +
          ' 帧 · 最高 ' + telemetry.max_us + ' μs · 超时 ' + telemetry.overruns : ''));
      }
    }
  }

  async function fetchStatus(method, body) {
    const device = (await window.ORLControl.identity).device_id;
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), 4000);
    try {
      const options = {method, cache: 'no-store', signal: controller.signal};
      if (body) {
        options.headers = {'Content-Type': 'application/json', 'X-ORL-Device': device};
        options.body = JSON.stringify(body);
      }
      const response = await fetch('/api/rf_denoise', options);
      const next = await response.json();
      if (!response.ok || !next.ok || next.device_id !== device ||
          !rows.every(({key}) => typeof next[key] === 'boolean') ||
          !next.speaker_stats || !next.handset_stats) throw Error('降噪状态未确认');
      return next;
    } finally {
      clearTimeout(timer);
    }
  }

  async function read() {
    if (reading || pending.size) { refreshWhenIdle = true; return; }
    refreshWhenIdle = false;
    reading = true;
    const version = epoch;
    try {
      const next = await fetchStatus('GET');
      if (version !== epoch || pending.size) return;
      saved = next;
      for (const [key, error] of errors) {
        if (saved[key] === error.requested) errors.delete(key);
        else error.message = '保存未确认，当前显示设备回读值';
      }
      text(connection, '');
      render();
    } catch (e) {
      if (version === epoch) text(connection, saved ? '状态暂未更新' : '读取降噪设置失败');
    } finally {
      reading = false;
      if (refreshWhenIdle && !pending.size) void read();
    }
  }

  async function save({mode, key}) {
    if (pending.has(key) || !saved) return;
    pending.add(key);
    ++epoch;
    errors.delete(key);
    render();
    const requested = !saved[key];
    try {
      const next = await fetchStatus('POST', {mode, expected: saved[key], enabled: requested});
      saved[key] = next[key];
      if (next[key + '_stats']) saved[key + '_stats'] = next[key + '_stats'];
    } catch (e) {
      errors.set(key, {requested, message: '保存未确认，正在核对设备设置'});
    } finally {
      pending.delete(key);
      ++epoch;
      render();
      if (!pending.size) void read();
    }
  }

  for (const entry of rows) entry.button.onclick = () => save(entry);
  void read();
  setInterval(() => {
    if (!document.hidden && card.getClientRects().length && !reading && !pending.size) void read();
  }, 2000);
})();
