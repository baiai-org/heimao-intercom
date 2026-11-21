(() => {
  'use strict';
  const card = document.getElementById('rfDenoiseCard');
  const get = name => card.querySelector('[data-dn="' + name + '"]');
  const level = get('level');
  const paths = [...card.querySelectorAll('input[data-path]')];
  const status = document.getElementById('rfDenoiseStatus');
  const levels = ['关闭', '轻度', '标准', '强力', '极强'];
  const states = ['已关闭', '等待下一话段', '正在降噪', '内存不足 · 暂用原声', '当前使用原声'];
  let saved, device, draft;
  let pending = false;
  let reading = false;
  let readError = '';
  let feedback = '';
  let epoch = 0;

  function levelAt(packed, setting) {
    return (packed >> ((setting - 1) * 3)) & 7;
  }

  function packed(value, mask) {
    return paths.reduce((result, input) => {
      const setting = Number(input.dataset.path);
      return mask & (1 << setting) ? result | (value << ((setting - 1) * 3)) : result;
    }, 0);
  }

  function matches(snapshot, value, mask) {
    return snapshot.selected_level === value && snapshot.selected_paths === mask &&
      snapshot.levels === packed(value, mask);
  }

  function renderStats() {
    const stats = {3: saved.speaker_stats, 4: saved.handset_stats};
    if (saved.mode) {
      const first = saved.mode === 1 ? 5 : 8;
      stats[saved.mode] = saved.bridge_stats;
      saved.network_stats.forEach((value, index) => { stats[first + index] = value; });
    }
    const rows = paths.flatMap(input => {
      const setting = Number(input.dataset.path);
      const item = stats[setting];
      if (!item) return [];
      const configured = levelAt(saved.levels, setting);
      const active = item.state === 2 || item.state === 4;
      const state = configured || active ? item.state || 1 : 0;
      return [input.parentElement.textContent.trim() + ' · 已保存 ' + levels[configured] + ' · ' + states[state] +
        (item.level && (active || item.input_frames) ?
          ' · ' + (active ? '当前话段 ' : '最近话段 ') + levels[item.level] : '') +
        (item.input_frames ? ' · ' + item.input_frames + ' 帧 · 输出降噪 ' +
          item.wet_frames + ' · 原声 ' + item.dry_frames + ' · 队列满 ' + item.queue_full +
          ' · 恢复 ' + item.recoveries : '') +
        (item.frames ? ' · 累计计算 ' + item.frames + ' 帧 · 计算最长 ' + item.max_us +
          ' μs · 迟到 ' + item.overruns + (Number.isInteger(item.wait_max_us) ?
            ' · 排队最长 ' + item.wait_max_us + ' μs' : '') : '')];
    });
    if (saved.i2s) rows.push('I2S 累计 · 接收欠载 ' + saved.i2s.receive_stalls +
      ' · 短写 ' + saved.i2s.short_writes + ' · 错误 ' + saved.i2s.errors);
    const list = get('stats');
    const summary = rows.join('\n');
    if (list.dataset.summary !== summary) {
      list.dataset.summary = summary;
      list.replaceChildren(...rows.map(value => {
        const li = document.createElement('li');
        li.textContent = value;
        return li;
      }));
    }
  }

  function render() {
    get('form').hidden = !saved;
    get('retry').hidden = !readError;
    get('connection').hidden = Boolean(saved && !readError);
    get('connection').textContent = readError || (saved ? '' : '正在读取降噪设置…');
    if (!saved) return;
    const selected = draft?.level ?? saved.selected_level;
    const mask = draft?.paths ?? saved.selected_paths;
    const mixed = !matches(saved, saved.selected_level, saved.selected_paths);
    level.value = String(selected);
    level.disabled = pending;
    for (const input of paths) {
      input.checked = Boolean(mask & (1 << Number(input.dataset.path)));
      input.disabled = pending;
    }
    get('mixed').hidden = !mixed;
    get('levelNote').hidden = !mask || selected !== 4;
    get('bridgeNote').hidden = !saved.mode;
    get('save').disabled = pending || (!draft && !mixed);
    get('undo').disabled = pending || !draft;
    status.textContent = pending ? '正在确认保存…' : feedback || (draft ? '有未保存修改' : '');
    renderStats();
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
      if (!response.ok) throw Error('降噪设置未确认');
      const next = await response.json();
      const identity = boundDevice || device || window.ORLControl?.device;
      if (!next.ok || !next.device_id || (identity && next.device_id !== identity)) {
        throw Error('设备身份已变化，请重新打开面板');
      }
      if (!Number.isInteger(next.mode) || next.mode < 0 || next.mode > 2 ||
          !Number.isInteger(next.selected_level) || next.selected_level < 0 || next.selected_level > 4 ||
          !Number.isInteger(next.selected_paths) || next.selected_paths < 0 || (next.selected_paths & ~0x7fe) ||
          !Number.isInteger(next.config_revision) || next.config_revision < 0 || next.config_revision > 0xffffffff ||
          !Number.isInteger(next.levels) || next.levels < 0 || next.levels > 0x3fffffff ||
          !Number.isInteger(next.flags) || next.flags < 0 || (next.flags & ~0x7fe) ||
          paths.some(input => levelAt(next.levels, Number(input.dataset.path)) > 4) ||
          !Array.isArray(next.network_stats) || !next.speaker_stats || !next.handset_stats) {
        throw Error('降噪状态未确认');
      }
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

  function edit() {
    if (pending || !saved) return;
    const value = Number(level.value);
    const mask = paths.reduce((bits, input) =>
      input.checked ? bits | (1 << Number(input.dataset.path)) : bits, 0);
    draft = matches(saved, value, mask) ? null : {level: value, paths: mask, expected: draft?.expected ?? saved};
    feedback = '';
    render();
  }

  async function save() {
    if (pending || !saved) return;
    const change = draft ?? {level: saved.selected_level, paths: saved.selected_paths, expected: saved};
    const body = {selected_level: change.level, selected_paths: change.paths,
      config_revision: change.expected.config_revision};
    pending = true;
    ++epoch;
    render();
    let next;
    try {
      next = await request(body, device);
    } catch (error) {
      try { next = await request(null, device); } catch (readFailure) { next = null; }
    }
    if (next) {
      saved = next;
      readError = '';
    }
    if (next && matches(next, change.level, change.paths)) {
      draft = null;
      feedback = '已保存，下一话段生效';
    } else {
      draft = {...change, expected: next ?? change.expected};
      feedback = next ? '未确认保存，已读取手台当前设置。请核对后再保存。' : '保存未确认，请重试';
    }
    pending = false;
    render();
  }

  level.addEventListener('change', edit);
  for (const input of paths) input.addEventListener('change', edit);
  get('save').onclick = save;
  get('retry').onclick = read;
  get('undo').onclick = () => {
    draft = null;
    feedback = '';
    render();
  };
  void read();
  setInterval(() => {
    if (!document.hidden && card.getClientRects().length) void read();
  }, 2000);
})();
