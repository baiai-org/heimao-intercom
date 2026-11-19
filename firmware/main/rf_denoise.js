(() => {
  'use strict';
  const card = document.getElementById('rfDenoiseCard');
  const get = name => card.querySelector('[data-dn="' + name + '"]');
  const inputs = [...card.querySelectorAll('[data-path]')];
  const states = ['已关闭', '等待下一话段', '正在降噪', '内存不足 · 暂用原声', '当前使用原声'];
  const modes = ['本地模拟', '公网直连', 'MQTT 网络中继'];
  const pathNames = ['手台发话 → 本机射频', '手台发话 → 网络', '射频来话 → 本机喇叭',
    '射频来话 → 网络转发', '网络来话 → 本机喇叭', '网络来话 → 模拟桥接'];
  const drafts = new Map();
  const messages = new Map();
  const status = document.getElementById('rfDenoiseStatus');
  let saved;
  let device;
  let pending = false;
  let reading = false;
  let readError = '';
  let epoch = 0;

  function bitFor(path, mode) {
    if (path === 'handset') return 1 << 4;
    if (path === 'speaker') return 1 << 3;
    if (!mode) return 0;
    if (path === 'forward') return 1 << mode;
    const first = mode === 1 ? 5 : 8;
    return 1 << (first + ['netTx', 'netSpeaker', 'netRf'].indexOf(path));
  }

  function modeDrafts(mode) {
    return inputs.map(input => bitFor(input.dataset.path, mode)).filter(bit => drafts.has(bit));
  }

  function text(node, value) {
    if (node.textContent !== value) node.textContent = value;
  }

  function render() {
    get('form').hidden = !saved;
    get('retry').hidden = !readError;
    get('connection').hidden = Boolean(saved && !readError);
    text(get('connection'), readError || (saved ? '' : '正在读取降噪设置…'));
    if (!saved) return;
    const {mode, flags} = saved;
    text(get('mode'), modes[mode]);
    text(get('txLabel'), '发到' + modes[mode]);
    text(get('forwardLabel'), '转发到' + modes[mode]);
    get('network').hidden = mode === 0;
    for (const input of inputs) {
      const bit = bitFor(input.dataset.path, mode);
      input.closest('label').hidden = !bit;
      const value = drafts.get(bit)?.value ?? Boolean(flags & bit);
      if (input.checked !== value) input.checked = value;
      if (input.getAttribute('aria-disabled') !== String(pending)) {
        input.setAttribute('aria-disabled', String(pending));
      }
    }
    const notes = {
      rfNote: mode && !saved.rf_tx ? '启用本机射频发射时使用「发到本机射频」。' : '',
      bridgeNote: mode ? (saved.bridge_keyed ? '桥接发射中 · 使用射频出口。桥接时本机喇叭与射频共用音频。' :
        saved.bridge_active ? '桥接发射时，两个出口共用射频音频。' :
        '两项桥接设置等待模拟桥接开启。') : '',
    };
    for (const [name, label] of Object.entries(notes)) {
      get(name).hidden = !label;
      text(get(name), label);
    }
    const stats = [saved.handset_stats, saved.network_stats[0], saved.speaker_stats,
      saved.bridge_stats || saved, saved.network_stats[1], saved.network_stats[2]];
    const rows = inputs.map((input, index) => {
      const bit = bitFor(input.dataset.path, mode);
      if (!bit) return '';
      const item = stats[index];
      const enabled = Boolean(flags & bit);
      const state = !enabled ? 0 : item?.state || 1;
      return pathNames[index] + ' · ' + (states[state] || states[1]) +
        (item?.input_frames ? ' · 当前或最近话段 ' + item.input_frames + ' 帧 · 输出降噪 ' +
          item.wet_frames + ' · 原声 ' + item.dry_frames + ' · 队列满 ' + item.queue_full +
          ' · 恢复 ' + item.recoveries : '') +
        (item?.frames ? ' · 累计计算 ' + item.frames + ' 帧 · 计算最长 ' + item.max_us +
          ' μs · 迟到 ' + item.overruns + (Number.isInteger(item.wait_max_us) ?
            ' · 排队最长 ' + item.wait_max_us + ' μs' : '') : '');
    }).filter(Boolean);
    if (saved.i2s) rows.push('I2S 累计 · 接收欠载 ' + saved.i2s.receive_stalls +
      ' · 短写 ' + saved.i2s.short_writes + ' · 错误 ' + saved.i2s.errors);
    const list = get('stats');
    if (list.dataset.summary !== rows.join('\n')) {
      list.dataset.summary = rows.join('\n');
      list.replaceChildren(...rows.map(value => {
        const li = document.createElement('li'); li.textContent = value; return li;
      }));
    }
    const dirty = modeDrafts(mode).length !== 0;
    get('save').disabled = pending || !dirty;
    get('undo').disabled = pending || !dirty;
    text(status, pending ? '正在确认保存…' :
      [messages.get(mode), dirty ? '有未保存修改' : ''].filter(Boolean).join(' · '));
  }

  async function request(body, boundDevice) {
    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), 4000);
    try {
      const options = {method: body ? 'POST' : 'GET', cache: 'no-store', signal: controller.signal};
      if (body) {
        options.headers = {'Content-Type': 'application/json', 'X-ORL-Device': boundDevice};
        options.body = JSON.stringify(body);
      }
      const response = await fetch('/api/rf_denoise', options);
      if (!response.ok) {
        const error = Error('降噪设置未确认');
        error.conflict = response.status === 409;
        throw error;
      }
      const next = await response.json();
      const identity = boundDevice || device || window.ORLControl?.device;
      if (!next.ok || !next.device_id || (identity && next.device_id !== identity)) {
        throw Error('设备身份已变化，请重新打开面板');
      }
      if (!Number.isInteger(next.mode) || !modes[next.mode] || !Number.isInteger(next.flags) ||
          (next.flags & 0x7fe) !== next.flags || !Array.isArray(next.network_stats) ||
          !next.speaker_stats || !next.handset_stats) throw Error('降噪状态未确认');
      return next;
    } finally {
      clearTimeout(timeout);
    }
  }

  async function read() {
    if (reading || pending) return;
    reading = true;
    const version = epoch;
    try {
      const next = await request();
      if (version !== epoch) return;
      device = next.device_id;
      saved = next;
      readError = '';
    } catch (error) {
      if (version !== epoch) return;
      readError = error.message.includes('设备身份') ? error.message :
        saved ? '状态暂未更新' : '读取降噪设置失败';
    } finally {
      reading = false;
      render();
    }
  }

  async function save() {
    if (pending || !saved) return;
    const mode = saved.mode;
    const bits = modeDrafts(mode);
    if (!bits.length) return;
    const body = {mode, mask: 0, expected: 0, values: 0};
    for (const bit of bits) {
      const {expected, value} = drafts.get(bit);
      body.mask |= bit;
      if (expected) body.expected |= bit;
      if (value) body.values |= bit;
    }
    const boundDevice = device;
    pending = true;
    ++epoch;
    messages.delete(mode);
    render();
    let next;
    let conflict = false;
    try {
      next = await request(body, boundDevice);
    } catch (error) {
      conflict = error.conflict;
      try { next = await request(null, boundDevice); } catch (readFailure) { next = null; }
    }
    if (next) {
      saved = next;
      readError = '';
    }
    if (next && (next.flags & body.mask) === body.values) {
      for (const bit of bits) drafts.delete(bit);
      messages.set(mode, '已保存，下一话段生效');
    } else if (next && (conflict || (next.flags & body.mask) !== body.expected)) {
      const latest = [];
      for (const bit of bits) {
        const draft = drafts.get(bit);
        const expected = Boolean(next.flags & bit);
        if (draft.expected !== expected) {
          const index = inputs.findIndex(input => bitFor(input.dataset.path, mode) === bit);
          latest.push(pathNames[index] + (expected ? '已开启' : '已关闭'));
          draft.expected = expected;
        }
      }
      messages.set(mode, latest.length ? '设备设置有变化（' + latest.join('、') + '），请核对后再保存' :
        '设置未保存，请重试');
    } else {
      messages.set(mode, '保存未确认，请重试');
    }
    pending = false;
    render();
  }

  for (const input of inputs) {
    input.addEventListener('click', event => { if (pending) event.preventDefault(); });
    input.addEventListener('change', () => {
      if (!saved || pending) { render(); return; }
      const bit = bitFor(input.dataset.path, saved.mode);
      if (!bit) return;
      const expected = Boolean(saved.flags & bit);
      if (input.checked === expected) drafts.delete(bit);
      else drafts.set(bit, {value: input.checked, expected});
      messages.delete(saved.mode);
      render();
    });
  }
  get('save').onclick = save;
  get('retry').onclick = read;
  get('undo').onclick = () => {
    if (pending || !saved) return;
    for (const bit of modeDrafts(saved.mode)) drafts.delete(bit);
    messages.delete(saved.mode);
    render();
  };
  void read();
  setInterval(() => {
    if (!document.hidden && card.getClientRects().length) void read();
  }, 2000);
})();
