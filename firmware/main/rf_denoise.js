(() => {
  'use strict';
  const card = document.getElementById('rfDenoiseCard');
  const status = document.getElementById('rfDenoiseStatus');
  const p2p = document.getElementById('rfDenoiseP2P');
  const mqtt = document.getElementById('rfDenoiseMqtt');
  const refresh = document.getElementById('rfDenoiseRead');
  const modes = [[1, 'p2p', p2p, '公网直连'], [2, 'mqtt', mqtt, 'MQTT 中继']];
  const states = ['关闭', '等待下一话段', '正在降噪', '内存不足，原声旁路', '处理超时，原声旁路'];
  let saved;
  let busy = false;
  let needsRead = false;

  function buttons() {
    for (const [, key, button, name] of modes) {
      button.disabled = busy || !saved || needsRead;
      if (!saved) continue;
      button.setAttribute('aria-pressed', String(saved[key]));
      button.classList.toggle('on', saved[key]);
      button.textContent = name + (saved[key] ? ' · 开启' : ' · 关闭');
    }
    refresh.disabled = busy;
  }

  async function request(mode, key) {
    if (busy || (key && (!saved || needsRead))) return;
    busy = true;
    buttons();
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), 4000);
    try {
      const device = (await window.ORLControl.identity).device_id;
      const options = {cache: 'no-store', signal: controller.signal};
      if (key) {
        options.method = 'POST';
        options.headers = {'Content-Type': 'application/json', 'X-ORL-Device': device};
        options.body = JSON.stringify({mode, expected: saved[key], enabled: !saved[key]});
      }
      const res = await fetch('/api/rf_denoise', options);
      const result = await res.json();
      if (!res.ok || !result.ok || result.device_id !== device ||
          typeof result.p2p !== 'boolean' || typeof result.mqtt !== 'boolean') {
        throw Error(result.err || '降噪状态读取失败');
      }
      saved = result;
      needsRead = false;
      const active = modes.find(([id]) => id === result.active_mode);
      status.textContent = (active ? active[3] + ' · ' : '') + (states[result.state] || '状态未知') +
        '。处理帧 ' + result.frames + '，最高 ' + result.max_us + ' μs，超时 ' + result.overruns + '。';
    } catch (e) {
      needsRead = true;
      status.textContent = key ? '未确认保存，请刷新状态后重试。' : '读取失败，请刷新状态。';
    } finally {
      clearTimeout(timer);
      busy = false;
      buttons();
    }
  }

  for (const [mode, key, button] of modes) button.onclick = () => request(mode, key);
  refresh.onclick = () => request();
  setInterval(() => {
    if (!needsRead && !document.hidden && card.getClientRects().length) request();
  }, 2000);
})();
