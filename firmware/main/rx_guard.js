/* Fixed post-TX RF receive guard. Settings are atomic per network mode. */
(() => {
  'use strict';
  function parseDelay(text, unit) {
    const s = String(text).trim();
    if (!(unit === 'ms' ? /^\d+$/ : /^\d+(?:\.\d{1,3})?$/).test(s)) {
      throw Error(unit === 'ms' ? '请输入整数毫秒' : '请输入秒数，最多三位小数');
    }
    const parts = s.split('.');
    const ms = unit === 'ms' ? Number(s) : Number(parts[0]) * 1000 + Number((parts[1] || '').padEnd(3, '0'));
    if (!Number.isSafeInteger(ms) || ms < 1 || ms > 60000) throw Error('保护时间须为 1–60000 毫秒（0.001–60 秒）');
    return ms;
  }
  const formatDelay = (ms, unit) => unit === 'ms' ? String(ms) : (ms / 1000).toFixed(3);
  function encode(enabled, ms, sources, outputs) {
    parseDelay(ms, 'ms');
    if (!Number.isInteger(sources) || sources < 1 || sources > 7) throw Error('请至少选择一个触发来源');
    if (!Number.isInteger(outputs) || outputs < 1 || outputs > 7) throw Error('请至少选择一个屏蔽出口');
    return [enabled ? 1 : 0, ms, sources, outputs].join(',');
  }
  function mount(root, mode, send) {
    let dirty = false, busy = false, known = false, unit = 's';
    root.className = 'subcard';
    root.innerHTML = `<div class="subcard-head"><span>发射后接收抑制 · 固定时间</span><span data-rg="state">等待手台状态</span></div>
      <div data-rg="form" hidden>
        <label style="display:flex;align-items:center;gap:8px"><input type="checkbox" data-rg="enabled">启用发射后接收抑制</label>
        <div class="row" style="flex-wrap:wrap;margin:8px 0">
          <label>保护时间</label><input data-rg="delay" type="number" min="0.001" max="60" step="0.001" inputmode="decimal" style="width:112px;flex:none">
          <select data-rg="unit" aria-label="时间单位" style="min-width:76px;flex:0 0 76px"><option value="s">秒</option><option value="ms">毫秒</option></select>
          <button type="button" data-rg="minus" aria-label="减少保护时间">−</button>
          <button type="button" data-rg="plus" aria-label="增加保护时间">＋</button>
          <select data-rg="step" aria-label="微调步进"><option value="1">每步 1 ms</option><option value="10">每步 10 ms</option><option value="100">每步 100 ms</option><option value="1000">每步 1000 ms</option></select>
        </div>
        <div class="row" style="flex-wrap:wrap;margin:4px 0"><label>快速填入</label>
          <select data-rg="preset" aria-label="保护时间预设"><option value="">自定义</option><option value="100">100 ms</option><option value="250">250 ms</option><option value="500">500 ms</option><option value="1000">1 秒</option><option value="2000">2 秒</option><option value="3000">3 秒</option><option value="5000">5 秒</option><option value="10000">10 秒</option></select>
        </div>
        <details><summary>高级选项：触发来源与屏蔽出口</summary>
          <div style="margin-top:8px">哪些射频发射结束后触发：</div>
          <div data-rg="sources" style="display:flex;flex-wrap:wrap;gap:10px"></div>
          <div style="margin-top:8px">保护期间屏蔽：</div>
          <div data-rg="outputs" style="display:flex;flex-wrap:wrap;gap:10px"></div>
        </details>
        <p data-rg="warning" hidden style="font-size:12px;color:var(--amber)">未屏蔽射频 → 网络转发，模拟中继的拖尾仍可能传回对端。</p>
        <p style="font-size:12px;color:var(--dim)">从本机射频 PTT 实际释放后计时；到期恢复，不等待载波关闭。仅模拟桥接启用并就绪时生效。被屏蔽的音频直接丢弃，新讲话也可能被截掉。网络来话与尾音发送继续工作。</p>
        <p style="font-size:12px;color:var(--dim)">支持 1 毫秒设置；实际恢复以音频块与调度边界为准。两种网络模式分别保存；关闭开关立即解除保护，其余修改在下一次发射结束时生效。</p>
        <div class="row" style="flex-wrap:wrap"><button type="button" class="pri" data-rg="save">保存本模式设置</button><button type="button" data-rg="reset">撤销未保存修改</button><span data-rg="message" role="status" style="font-size:12px"></span></div>
      </div>`;
    const get = name => root.querySelector('[data-rg="' + name + '"]');
    const groups = {sources: [[1, '网络桥接'], [2, 'APP／网页 PTT'], [4, '本机话筒／实体 PTT']],
      outputs: [[1, '射频 → 网络转发'], [2, 'APP／网页收听'], [4, '本机喇叭']]};
    for (const [name, entries] of Object.entries(groups)) {
      get(name).innerHTML = entries.map(([bit, label]) => '<label style="display:flex;align-items:center;gap:5px"><input type="checkbox" value="' + bit + '">' + label + '</label>').join('');
    }
    const boxes = name => [...get(name).querySelectorAll('input')];
    const mask = name => boxes(name).reduce((sum, b) => sum | (b.checked ? Number(b.value) : 0), 0);
    const message = text => { get('message').textContent = text; };
    const changed = () => { dirty = true; message('有未保存修改'); get('warning').hidden = !!(mask('outputs') & 1); };
    root.addEventListener('input', changed);
    root.addEventListener('change', changed);
    let lastStatus = null;
    function fill(c) {
      get('enabled').checked = !!c.en;
      get('delay').value = formatDelay(c.ms, unit);
      for (const name of ['sources', 'outputs']) for (const b of boxes(name)) b.checked = !!(c[name] & Number(b.value));
      get('warning').hidden = !!(c.outputs & 1);
      get('preset').value = '';
    }
    function setTime(ms) { get('delay').value = formatDelay(Math.max(1, Math.min(60000, ms)), unit); changed(); }
    get('unit').onchange = () => {
      try {
        const ms = parseDelay(get('delay').value, unit);
        unit = get('unit').value;
        Object.assign(get('delay'), {min: unit === 'ms' ? '1' : '0.001', max: unit === 'ms' ? '60000' : '60', step: unit === 'ms' ? '1' : '0.001'});
        get('delay').value = formatDelay(ms, unit);
      } catch (e) { get('unit').value = unit; message(e.message); }
    };
    for (const [name, sign] of [['minus', -1], ['plus', 1]]) get(name).onclick = () => {
      try { setTime(parseDelay(get('delay').value, unit) + sign * Number(get('step').value)); } catch (e) { message(e.message); }
    };
    get('preset').onchange = () => { if (get('preset').value) setTime(Number(get('preset').value)); };
    get('reset').onclick = () => {
      if (lastStatus) { fill(lastStatus); dirty = false; message('已恢复为手台保存值'); }
    };
    get('save').onclick = async () => {
      if (!known || busy) return;
      try {
        const ms = parseDelay(get('delay').value, unit);
        const sources = mask('sources'), outputs = mask('outputs'), en = get('enabled').checked ? 1 : 0;
        const value = encode(en, ms, sources, outputs);
        busy = true;
        for (const c of root.querySelectorAll('input,select,button')) c.disabled = true;
        const result = await send(mode === 1 ? 'rx_guard_p2p' : 'rx_guard_relay', value);
        if (!result || !result.ok) throw Error(result && result.err || '保存失败，请重试');
        lastStatus = {en, ms, sources, outputs};
        dirty = false;
        message('已保存；' + (en ? '下一次射频发射结束后生效' : '已关闭保护'));
      } catch (e) { message(e.message); }
      finally { busy = false; for (const c of root.querySelectorAll('input,select,button')) c.disabled = !known; }
    };
    return {sync(st) {
      const rg = st.rx_guard, c = rg && rg[mode === 1 ? 'p2p' : 'relay'];
      known = !!(rg && rg.version === 1 && c);
      get('form').hidden = false;
      if (!busy) for (const control of root.querySelectorAll('input,select,button')) control.disabled = !known;
      get('state').textContent = !known ? (lastStatus ? '状态暂缺 · 保留上次设置' : rg && rg.version !== 1 ? '设置版本暂不兼容' : '等待保护配置') :
        st.intercom && st.intercom.mode === mode && rg.blocked ? '保护中 · 剩余约 ' + formatDelay(rg.remaining_ms, 's') + ' 秒' :
        c.en ? (st.bridge && st.bridge.active && st.intercom && st.intercom.mode === mode ? '已启用 · 等待发射结束' : '已启用 · 桥接未就绪') : '已关闭';
      if (known && !busy) {
        lastStatus = c;
        if (!dirty) fill(c);
      }
    }};
  }
  window.ORLRxGuard = {parseDelay, formatDelay, encode, mount};
})();
