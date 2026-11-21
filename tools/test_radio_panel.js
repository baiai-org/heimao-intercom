/* Host checks of production panel functions, with DOM/network stand-ins.
 * No network requests leave this process. Run: node tools/test_radio_panel.js */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const html = fs.readFileSync(path.join(__dirname, '../firmware/main/panel_index.html'), 'utf8');
const scripts = [...html.matchAll(/<script\b[^>]*>([\s\S]*?)<\/script>/g)].map(m => m[1]);
for (const code of scripts) new vm.Script(code); // Parse every inline script.
const source = scripts.join('\n');
function part(start, end) {
  const a = source.indexOf(start), b = source.indexOf(end, a + start.length);
  assert(a >= 0 && b > a, start);
  return source.slice(a, b);
}
const elements = new Map([...html.matchAll(/\bid="([^"]+)"/g)].map(m =>
  [m[1], {id: m[1], value: '', dataset: {}, style: {}, textContent: '', innerHTML: '', disabled: false,
    addEventListener() {}, removeAttribute() {}}]));
const steps = [...html.matchAll(/data-fstep="([^"]+)"/g)].map(m => ({dataset: {fstep: m[1]}}));
const calls = [], logs = [];
const device = {device_id: 'orl-test', api_version: 2, capabilities: {tx_timeout: 1}};
let storedTimeout = 180, write = 'ok', readDevice = device.device_id, readCount = 0;
const channels = [
  {i: 0, name: 'UHF', tx: 438.5, rx: 438.5, band: 'UHF', compatible: false, txt: '0000', rxt: '0000'},
  {i: 1, name: 'VHF', tx: 145.0125, rx: 145.6125, band: 'VHF', compatible: true, txt: '0000', rxt: '0000'},
];
const context = vm.createContext({
  txLive: false, talkBusy: false, SDR: {ptt:false}, panelDirty: new Map(), panelRadioAt: 0,
  AbortController, setTimeout, clearTimeout,
  window: {ORLControl: {identity: Promise.resolve(device)}}, document: {activeElement: null, querySelectorAll: selector => {
    assert.equal(selector, '[data-fstep]'); return steps;
  }},
  $: id => {assert(elements.has(id), 'unknown element ' + id); return elements.get(id);},
  panelEditing: el => context.panelDirty.has(el.id),
  cmd: async (k, v) => { calls.push({k, v}); return {ok: true}; },
  fetch: async (url, request) => {
    assert.equal(url, '/api/set'); assert.equal(request.method, 'POST');
    assert.equal(request.headers['X-ORL-Device'], device.device_id);
    const data = JSON.parse(request.body); calls.push(data);
    if (write === 'busy') return {json: async () => ({ok:false, err:'请结束发射或转发后保存'})};
    if (write !== 'unchanged') storedTimeout = Number(data.v);
    if (write === 'unknown') throw Error('connection dropped after write');
    return {json: async () => ({ok:true, tx_timeout_s:storedTimeout})};
  },
  panelReadJsonImpl: async url => {
    assert.equal(url, '/api/radio'); readCount++;
    return {device_id:readDevice, tx_timeout_s:storedTimeout};
  },
  panelReadJson: async url => { assert.equal(url, '/api/device'); return device; },
  log: (...args) => logs.push(args), touchInput: () => {}, last: '',
  loadChannels: async () => {}, syncBridge: () => {}, refresh: async () => {},
  readChannels: async () => channels,
  escHtml: v => String(v).replace(/[&<>"']/g, '_'),
});
vm.runInContext(part('function radioGridHz(', '/* Atomic dual-frequency step'), context);
vm.runInContext(part('/* Atomic dual-frequency step', 'let tonesReady'), context);
vm.runInContext(part("$('fset').onclick", "$('fSync').onclick"), context);
vm.runInContext(part('async function loadBridgeChans()', 'const rxGuardViews'), context);
vm.runInContext('const CH_MAX = 99; let _chAll = [];\n' + part('function renderChannels()', "if ($('chSearch'))"), context);
const uhf = {band:'UHF', model:'SA818S-U', min:400, max:480, compatible:true, ready:true, power_applied:0};
context.renderRadio(uhf);
assert.equal(elements.get('rfBand').value, 'UHF');
assert.equal(elements.get('ftx').min, 400);
assert.equal(context.radioFrequency('438.5000'), 438.5);
for (const text of ['', 'nan', '438.5x', '438.5001', '145', '481', '4.385e2'])
  assert.equal(context.radioFrequency(text), null, text);
// An edited band selection survives polling until the explicit confirm.
elements.get('rfBand').value = 'VHF'; elements.get('rfBand').onchange();
context.renderRadio(uhf); assert.equal(elements.get('rfBand').value, 'VHF');
elements.get('rfBand').dataset.dirty = '';
// Changing the physical band clears mismatched frequency inputs, with no set request.
elements.get('ftx').value = '438.5000'; elements.get('frx').value = '438.5000';
const vhf = {band:'VHF', model:'SA818S-V', min:134, max:174, compatible:false, ready:false, power_applied:0};
context.renderRadio(vhf);
assert.equal(elements.get('ftx').value, '');
assert.equal(elements.get('frx').value, '');
assert.equal(elements.get('rfBand').value, 'VHF');
assert.equal(elements.get('pwrT').disabled, true);
assert.equal(elements.get('chSave').disabled, true);
assert.equal(calls.length, 0);
const up = steps.find(b => b.dataset.fstep === '0.0125');
up.onclick(); assert.equal(calls.length, 0); // Empty input never tunes a default.
elements.get('ftx').value = '174.0000'; elements.get('frx').value = '145.0000';
up.onclick(); assert.equal(calls.length, 0); // Crossing the edge is refused, never clamped.
assert.equal(elements.get('ftx').value, '174.0000');
elements.get('ftx').value = '145.0000'; elements.get('frx').value = '145.6000';
up.onclick(); assert.deepEqual(calls.pop(), {k:'freq_pair', v:'145.0125,145.6125'});
elements.get('fset').onclick();
assert.deepEqual(calls.pop(), {k:'freq_pair', v:'145.0125,145.6125'});
elements.get('ftx').value = '145.0001'; elements.get('fset').onclick();
assert.equal(calls.length, 0);
// Capability negotiation keeps old firmware at 12.5 kHz. Only the requested
// 1/2.5/5 kHz fine-step buttons are present; 6.25 kHz is rejected.
assert.equal(steps.find(b=>b.dataset.fstep==='0.00625'),undefined);
const fineUp = steps.find(b => b.dataset.fstep === '0.001');
assert.equal(fineUp.disabled, true);
context.renderRadio({...vhf, compatible:true, ready:true, freq_grid_hz:500, freq_decimals:4});
assert.equal(fineUp.disabled, false);
for(const [delta, tx, rx] of [['0.001','145.0010','145.6010'],['0.0025','145.0025','145.6025'],
  ['0.005','145.0050','145.6050']]){
  elements.get('ftx').value='145.0000';elements.get('frx').value='145.6000';
  steps.find(b=>b.dataset.fstep===delta).onclick();
  assert.deepEqual(calls.pop(),{k:'freq_pair',v:tx+','+rx});
  assert.equal(elements.get('ftx').value,tx);
}
for(const text of ['145.0001','145.00625','145.0062','145.0063'])assert.equal(context.radioFrequency(text),null,text);
assert.equal(context.radioDisplayFrequency(145.0050),'145.0050');
assert.equal(context.radioDisplayFrequency(145.0125),'145.0125');
elements.get('fset').onclick();assert.deepEqual(calls.pop(),{k:'freq_pair',v:'145.0050,145.6050'});
context.renderRadio({...vhf, compatible:true, ready:true, power_pending:true});
assert.equal(elements.get('pwrT').disabled, false);
assert.match(elements.get('pwrV').textContent, /待松开 PTT/);
context.renderRadio({band:'UNSET', min:0, max:0, ready:false, compatible:false});
assert.equal(elements.get('fset').disabled, true);
assert.equal(context.radioFrequency('145'), null);
(async () => {
  const timeout = elements.get('txTimeout'), saveTimeout = elements.get('txTimeoutSet');
  const submit = () => elements.get('txTimeoutForm').onsubmit({preventDefault() {}});
  const status = elements.get('txTimeoutStatus');
  const sync = async (r = {tx_timeout_s: storedTimeout}, s = device, failed = false) => {
    await context.syncTxTimeout(s, r, failed, vm.runInContext('txTimeoutEpoch', context));
    context.renderTxTimeout();
  };
  const draft = value => {
    timeout.value = value;
    context.panelDirty.set('txTimeout', value);
    context.renderTxTimeout();
  };
  context.renderTxTimeout();
  assert.equal(saveTimeout.disabled,true);
  await sync();
  assert.equal(timeout.value,'180');
  assert.equal(timeout.disabled,false);
  assert.equal(status.hidden,true);
  const hint = elements.get('txTimeoutHint');
  assert.equal(hint.hidden,true);
  context.document.activeElement = timeout;
  context.renderTxTimeout();
  assert.equal(hint.hidden,false);
  context.document.activeElement = null;
  for (const bad of ['4','601','180.5','5e1','-5','']) {
    draft(bad);
    await submit();
    assert.equal(calls.length,0);
    assert.match(status.textContent,/请输入 5–600/);
  }
  draft('600');
  await submit();
  assert.deepEqual(calls.pop(),{k:'tx_timeout_s',v:'600'});
  assert.match(status.textContent,/已保存 600 秒/);
  draft('5');
  await sync();
  assert.equal(timeout.value,'5');
  await sync(undefined, {...device, tx_timeout_busy:true});
  assert.equal(saveTimeout.disabled,true);
  await submit(); assert.equal(calls.length,0);
  await sync();
  write = 'busy'; await submit(); calls.pop();
  assert.match(status.textContent,/请结束发射或转发/); assert.equal(timeout.value,'5');
  assert.equal(readCount,1);
  write = 'unchanged'; await submit(); calls.pop();
  assert.match(status.textContent,/保存未确认.*未保存.*当前生效 600 秒/);
  write = 'unknown'; await submit(); calls.pop();
  assert.equal(readCount,3); assert.match(status.textContent,/已保存 5 秒/);
  draft('120'); readDevice = 'orl-other'; await submit(); calls.pop();
  assert.match(status.textContent,/设备身份已变化/); assert.equal(saveTimeout.disabled,true);
  assert.equal(timeout.value,'120');
  await sync({}, device, true);
  assert.match(status.textContent,/读取失败/); assert.equal(elements.get('txTimeoutRetry').hidden,false);
  device.capabilities.tx_timeout = 0;
  await sync(); assert.match(status.textContent,/手台固件不支持/);
  console.log('PASS: shared TX timeout identity, integral bounds, busy, read failure, lost reply and draft');
  await context.loadBridgeChans();
  assert.match(elements.get('brChan1').innerHTML, /value="1" disabled/);
  assert.match(elements.get('brChan1').innerHTML, /value="2">/);
  context.testChannels = channels;
  vm.runInContext('_chAll = testChannels; renderChannels();', context);
  const list = elements.get('chList').innerHTML;
  assert.match(list, /data-ch="load" data-i="0" class="pri" disabled/);
  assert.match(list, /data-ch="load" data-i="1" class="pri">/);
  assert.match(list, /data-ch="del" data-i="0">/);
  console.log('PASS: complete panel syntax, UHF/VHF ranges, explicit band selection, empty/edge inputs, deferred power, incompatible channels');
})().catch(error => { console.error(error); process.exitCode = 1; });
