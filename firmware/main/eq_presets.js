(() => {
  'use strict';
  const module = $('eqUserPresets');
  const form = $('eqPresetForm');
  const input = $('eqPresetName');
  const feedback = $('eqPresetFeedback');
  const endpoint = '/api/eq_presets';
  let saved = null, editing = null, loading = false, busy = false, checkedAt = 0, epoch = 0;
  let openMenu = null;

  const equal = (a, b) => a.length === b.length && a.every((n, i) => n === b[i]);
  const bands = (a, max = 6) => Array.isArray(a) && a.length === 6 &&
    a.every(n => Number.isInteger(n) && n >= -12 && n <= max);
  const validName = value => typeof value === 'string' && value.trim() && [...value].length <= 24 && new TextEncoder().encode(value).length <= 96 &&
    !/[\u0000-\u001F\u007F-\u009F\uD800-\uDFFF]/u.test(value);

  function accept(raw, device) {
    if (!raw?.ok || !device || raw.device_id !== device || window._st?.device_id !== device ||
        !/^[a-f0-9]{8}$/i.test(raw.boot_id) ||
        !Number.isInteger(raw.revision) || raw.revision < 0 || raw.revision > 0xFFFFFFFF ||
        !bands(raw.active_spk, 12) || !bands(raw.active_mic, 12) ||
        !Array.isArray(raw.slots) || raw.slots.length !== 3 ||
        raw.slots.some(slot => slot !== null && (!validName(slot?.name) || !bands(slot.spk) || !bands(slot.mic)))) {
      throw Error('EQ 保存栏回读无效');
    }
    saved = raw;
    return raw;
  }

  function actionButton(label, handler) {
    const button = document.createElement('button');
    button.type = 'button';
    button.textContent = label;
    button.addEventListener('click', handler);
    return button;
  }

  function closeMenu(returnFocus = false) {
    if (!openMenu) { return; }
    const {menu, more} = openMenu;
    openMenu = null;
    menu.hidden = true;
    more.setAttribute('aria-expanded', 'false');
    if (returnFocus) { more.focus(); }
  }

  function placeMenu(menu, more) {
    const anchor = more.getBoundingClientRect();
    const footer = document.querySelector(document.body.dataset.panelLayout === 'simple' ? '.panel-nav' : '.mobile-dock');
    const bottom = footer?.offsetHeight && getComputedStyle(footer).position === 'fixed'
      ? footer.getBoundingClientRect().top : window.innerHeight;
    const headerBottom = Math.max(0, document.querySelector('.topbar').getBoundingClientRect().bottom);
    const below = bottom - anchor.bottom - 4;
    const above = anchor.top - headerBottom - 4;
    menu.style.maxHeight = '';
    const opensAbove = menu.offsetHeight > below && above > below;
    menu.classList.toggle('opens-above', opensAbove);
    menu.style.maxHeight = Math.max(0, opensAbove ? above : below) + 'px';
  }

  const rows = [1, 2, 3].map(slot => {
    const row = document.createElement('div');
    row.className = 'eq-user-slot'; row.dataset.slot = slot;
    const title = document.createElement('strong');
    const actions = document.createElement('div'); actions.className = 'eq-user-actions';
    const menu = document.createElement('div');
    menu.className = 'eq-user-menu';
    menu.id = 'eqSlotMenu' + slot;
    menu.hidden = true;
    const menuName = document.createElement('span');
    menu.append(menuName);
    const save = actionButton('保存', () => beginEdit(slot, 'save'));
    const apply = actionButton('应用', () => change(slot, 'apply'));
    const more = actionButton('⋯', () => {
      const wasOpen = openMenu?.more === more;
      closeMenu();
      if (wasOpen) { return; }
      openMenu = {menu, more};
      menu.hidden = false;
      placeMenu(menu, more);
      more.setAttribute('aria-expanded', 'true');
    });
    more.className = 'eq-user-more';
    more.setAttribute('aria-label', '槽位 ' + slot + ' 更多操作');
    more.setAttribute('aria-controls', menu.id);
    more.setAttribute('aria-expanded', 'false');
    const menuButtons = [
      ['重命名', () => beginEdit(slot, 'rename')],
      ['覆盖', () => beginEdit(slot, 'save')],
      ['删除', () => change(slot, 'delete')]
    ].map(([label, handler]) => actionButton(label, () => {
      closeMenu(true);
      handler();
    }));
    menu.append(...menuButtons);
    actions.append(save, apply, more, menu);
    row.append(title, actions); $('eqPresetList').append(row);
    return {slot, title, save, apply, more, menuName, menuButtons};
  });

  document.addEventListener('click', event => {
    if (openMenu && !openMenu.more.contains(event.target) && !openMenu.menu.contains(event.target)) { closeMenu(); }
  });
  document.addEventListener('keydown', event => {
    if (openMenu && event.key === 'Escape') {
      event.preventDefault();
      closeMenu(true);
    }
  });
  window.addEventListener('resize', () => closeMenu());

  function render() {
    const available = Boolean(saved) && !busy && !editing;
    for (const {slot,title,save,apply,more,menuName,menuButtons} of rows) {
      const item = saved?.slots[slot - 1];
      const text = '槽位 ' + slot + (item ? ' · ' + item.name : saved ? ' · 空槽' : '');
      if (title.textContent !== text) {
        title.textContent = text;
        title.title = text;
        menuName.textContent = item?.name || '';
      }
      save.hidden = Boolean(item);
      apply.hidden = more.hidden = !item;
      for (const button of [save, apply, more, ...menuButtons]) { button.disabled = !available; }
      if ((!available || !item) && openMenu?.more === more) { closeMenu(); }
    }
    $('eqPresetSubmit').disabled = busy || !saved;
    $('eqPresetCancel').disabled = busy;
    input.readOnly = busy;
  }

  async function read(device) {
    if (loading || busy || !device || !module.getClientRects().length || document.hidden ||
        Date.now() - checkedAt < 3000) { return; }
    checkedAt = Date.now();
    loading = true;
    const started = epoch;
    try {
      const raw = await panelReadJson(endpoint);
      if (started !== epoch) { return; }
      const wasEmpty = !saved;
      accept(raw, device);
      if (wasEmpty) { feedback.textContent = ''; }
    } catch (error) {
      if (started === epoch && !saved) { feedback.textContent = error.message || '读取 EQ 保存栏失败'; }
    } finally {
      loading = false;
      render();
    }
  }

  function capture(base) {
    if (!bands(base.active_spk) || !bands(base.active_mic)) {
      throw Error('现有曲线超出 −12 到 +6 dB，请先调整并保存当前 EQ');
    }
    return {
      spk: spkIds.map(id => Number($(id).value)),
      mic: micIds.map(id => Number($(id).value))
    };
  }

  function beginEdit(slot, op) {
    if (!saved || busy || editing) { return; }
    const item = saved.slots[slot - 1];
    if (op === 'save' && item && !confirm('覆盖「' + item.name + '」的 EQ 曲线？')) { return; }
    try {
      editing = {slot, op, base:saved, values:op === 'save' ? capture(saved) : {}};
    } catch (error) {
      feedback.textContent = error.message;
      return;
    }
    $('eqPresetEditTitle').textContent = (op === 'rename' ? '重命名槽位 ' : '保存到槽位 ') + slot;
    input.value = item?.name || '';
    feedback.textContent = '';
    form.hidden = false; render(); input.focus();
  }

  function closeEditor() {
    editing = null; form.hidden = true; render();
  }

  function confirmed(raw, request, base) {
    const item = raw.slots[request.slot - 1];
    if (raw.boot_id !== base.boot_id) { return false; }
    if (request.op === 'apply') {
      const wanted = base.slots[request.slot - 1];
      return raw.revision === base.revision && equal(wanted.spk,raw.active_spk) && equal(wanted.mic,raw.active_mic);
    }
    if (raw.revision !== base.revision + 1) { return false; }
    if (request.op === 'delete') { return item === null; }
    const wanted = request.op === 'save' ? request : base.slots[request.slot - 1];
    return item && item.name === request.name && equal(item.spk,wanted.spk) && equal(item.mic,wanted.mic);
  }

  async function write(slot, op, base, fields = {}) {
    if (busy || !base || window._st?.device_id !== base.device_id) { return false; }
    ++epoch;
    busy = true; render(); feedback.textContent = '正在写入…';
    const body = {op, slot, revision:base.revision, boot_id:base.boot_id, ...fields};
    if (op === 'apply') {
      body.expected_spk = base.active_spk;
      body.expected_mic = base.active_mic;
    }
    let conflict = false;
    try {
      const controller = new AbortController();
      const timeout = setTimeout(() => controller.abort(), 5000);
      let result;
      try {
        const response = await fetch(endpoint, {method:'POST', cache:'no-store', signal:controller.signal,
          headers:{'Content-Type':'application/json','X-ORL-Device':base.device_id}, body:JSON.stringify(body)});
        const reply = await response.json();
        conflict = response.status === 409;
        if (!response.ok || !reply.ok) { throw Error(reply.err || '写入未确认'); }
        result = accept(reply, base.device_id);
      } catch (error) {
        // A lost reply can follow a completed commit. Read once, never replay a write.
        await panelReadPending.get(endpoint)?.catch(() => {});
        try { result = accept(await panelReadJson(endpoint),base.device_id); } catch (_) {}
        if (editing && saved?.device_id === base.device_id) { editing.base = saved; }
        if (conflict || !result || !confirmed(result,body,base)) { throw error; }
      } finally {
        clearTimeout(timeout);
      }
      if (!confirmed(result,body,base)) { throw Error('设备回读未确认，请核对后重试'); }
      if (op === 'apply') {
        await panelReadPending.get('/api/status')?.catch(() => {});
        for (const [ids,values] of [[spkIds,result.active_spk],[micIds,result.active_mic]]) {
          ids.forEach((id,i) => { $(id).value = values[i]; $(id + 'V').textContent = fmtSlider(id,values[i]); });
        }
        updateEqCurve(); last = ''; void refresh();
      }
      feedback.textContent = {save:'已保存到槽位',rename:'已重命名',delete:'已删除',apply:'已应用接收和发射 EQ'}[op];
      return true;
    } catch (error) {
      feedback.textContent = conflict ? '预设或当前 EQ 已变化。请核对当前值后重试。' :
        error.name === 'AbortError' ? 'EQ 写入超时，请核对后重试' : (error.message || '写入未确认');
      return false;
    } finally {
      busy = false; checkedAt = Date.now(); render();
    }
  }

  function change(slot, op) {
    const item = saved?.slots[slot - 1];
    if (!item || editing || busy) { return; }
    if (op === 'delete' && !confirm('删除「' + item.name + '」？')) { return; }
    void write(slot,op,saved);
  }
  $('eqPresetCancel').addEventListener('click', closeEditor);
  form.addEventListener('submit', async event => {
    event.preventDefault();
    if (!editing || busy) { return; }
    const name = input.value.trim();
    if (!validName(name)) {
      feedback.textContent = '请输入名称，最多 24 字，不可包含控制字符';
      return;
    }
    const {slot,op,base,values} = editing;
    if (await write(slot,op,base,{...values,name})) { closeEditor(); }
  });

  window.HMEqPresets = {
    observe(status) {
      if (saved && saved.device_id !== status.device_id) {
        saved = null; checkedAt = 0; closeEditor();
      }
      void read(status.device_id);
    }
  };
  if (window._st) { window.HMEqPresets.observe(window._st); }
  render();
})();
