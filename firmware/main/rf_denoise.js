(() => {
  'use strict';
  const card = document.getElementById('rfDenoiseCard');
  const get = name => card.querySelector('[data-dn="' + name + '"]');
  const selects = [...card.querySelectorAll('select[data-path]')];
  const states = ['已关闭', '等待下一话段', '正在降噪', '内存不足 · 暂用原声', '当前使用原声'];
  const modes = ['本地模拟', '公网直连', 'MQTT 网络中继'];
  const levelNames = ['关闭', '轻度', '标准', '强力', '极强'];
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

  function settingFor(path, mode) {
    if (path === 'handset') return 4;
    if (path === 'speaker') return 3;
    if (!mode) return 0;
    if (path === 'forward') return mode;
    const first = mode === 1 ? 5 : 8;
    return first + ['netTx', 'netSpeaker', 'netRf'].indexOf(path);
  }

  function levelFor(levels, setting) {
    return (levels >> ((setting - 1) * 3)) & 7;
  }

  function modeDrafts(mode) {
    return selects.map(select => settingFor(select.dataset.path, mode)).filter(setting => drafts.has(setting));
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
    const {mode, levels} = saved;
    text(get('mode'), modes[mode]);
    text(get('txLabel'), '发到' + modes[mode]);
    text(get('forwardLabel'), '转发到' + modes[mode]);
    get('network').hidden = mode === 0;
    let extreme = false;
    for (const select of selects) {
      const setting = settingFor(select.dataset.path, mode);
      select.closest('label').hidden = !setting;
      if (!setting) continue;
      const level = drafts.get(setting)?.value ?? levelFor(levels, setting);
      if (select.value !== String(level)) select.value = level;
      if (select.getAttribute('aria-disabled') !== String(pending)) {
        select.setAttribute('aria-disabled', String(pending));
      }
      extreme ||= level === 4;
    }
    get('levelNote').hidden = !extreme;
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
    const rows = selects.map((select, index) => {
      const setting = settingFor(select.dataset.path, mode);
      if (!setting) return '';
      const item = stats[index];
      const configured = levelFor(levels, setting);
      const active = item?.state === 2 || item?.state === 4;
      const state = configured || active ? item?.state || 1 : 0;
      return pathNames[index] + ' · 已保存 ' + levelNames[configured] + ' · ' + (states[state] || states[1]) +
        (item?.level && (active || item.input_frames) ?
          ' · ' + (active ? '当前话段 ' : '最近话段 ') + levelNames[item.level] : '') +
        (item?.input_frames ? ' · ' + item.input_frames + ' 帧 · 输出降噪 ' +
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
      if (!Number.isInteger(next.mode) || !modes[next.mode] || !Number.isInteger(next.levels) ||
          next.levels < 0 || next.levels > 0x3fffffff || !Array.isArray(next.network_stats) ||
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
    const settings = modeDrafts(mode);
    if (!settings.length) return;
    const body = {mode, mask: 0, expected_levels: 0, levels: 0};
    let fields = 0;
    for (const setting of settings) {
      const {expected, value} = drafts.get(setting);
      const shift = (setting - 1) * 3;
      body.mask |= 1 << setting;
      body.expected_levels |= expected << shift;
      body.levels |= value << shift;
      fields |= 7 << shift;
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
    if (next && (next.levels & fields) === body.levels) {
      for (const setting of settings) drafts.delete(setting);
      messages.set(mode, '已保存，下一话段生效');
    } else if (next && (conflict || (next.levels & fields) !== body.expected_levels)) {
      const latest = [];
      for (const setting of settings) {
        const draft = drafts.get(setting);
        const expected = levelFor(next.levels, setting);
        if (draft.expected !== expected) {
          const index = selects.findIndex(select => settingFor(select.dataset.path, mode) === setting);
          latest.push(pathNames[index] + '为' + levelNames[expected]);
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

  for (const select of selects) {
    select.replaceChildren(...levelNames.map((label, level) => new Option(label, level)));
    select.addEventListener('keydown', event => { if (pending && event.key !== 'Tab') event.preventDefault(); });
    select.addEventListener('change', () => {
      if (!saved || pending) { render(); return; }
      const setting = settingFor(select.dataset.path, saved.mode);
      if (!setting) return;
      const expected = levelFor(saved.levels, setting);
      const value = Number(select.value);
      if (value === expected) drafts.delete(setting);
      else drafts.set(setting, {value, expected});
      messages.delete(saved.mode);
      render();
    });
  }
  get('save').onclick = save;
  get('retry').onclick = read;
  get('undo').onclick = () => {
    if (pending || !saved) return;
    for (const setting of modeDrafts(saved.mode)) drafts.delete(setting);
    messages.delete(saved.mode);
    render();
  };
  void read();
  setInterval(() => {
    if (!document.hidden && card.getClientRects().length) void read();
  }, 2000);
})();
