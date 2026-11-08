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
  [m[1], {value: '', dataset: {}, style: {}, textContent: '', innerHTML: '', disabled: false}]));
const steps = [...html.matchAll(/data-fstep="([^"]+)"/g)].map(m => ({dataset: {fstep: m[1]}}));
const calls = [], logs = [];
const channels = [
  {i: 0, name: 'UHF', tx: 438.5, rx: 438.5, band: 'UHF', compatible: false, txt: '0000', rxt: '0000'},
  {i: 1, name: 'VHF', tx: 145.0125, rx: 145.6125, band: 'VHF', compatible: true, txt: '0000', rxt: '0000'},
];
const context = vm.createContext({
  window: {}, document: {activeElement: null, querySelectorAll: selector => {
    assert.equal(selector, '[data-fstep]'); return steps;
  }},
  $: id => {assert(elements.has(id), 'unknown element ' + id); return elements.get(id);},
  cmd: async (k, v) => { calls.push({k, v}); return {ok: true}; },
  log: (...args) => logs.push(args), touchInput: () => {}, last: '',
  loadChannels: async () => {}, syncBridge: () => {}, refresh: async () => {},
  readChannels: async () => channels,
  escHtml: v => String(v).replace(/[&<>"']/g, '_'),
});
vm.runInContext(part('function radioFrequency(', "document.querySelectorAll('[data-sec]')"), context);
vm.runInContext(part('/* Atomic dual-frequency step', 'let tonesReady'), context);
vm.runInContext(part("$('fset').onclick", "$('fSync').onclick"), context);
vm.runInContext(part('async function loadBridgeChans()', 'function syncBridge('), context);
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
context.renderRadio({...vhf, compatible:true, ready:true, power_pending:true});
assert.equal(elements.get('pwrT').disabled, false);
assert.match(elements.get('pwrV').textContent, /待松开 PTT/);
context.renderRadio({band:'UNSET', min:0, max:0, ready:false, compatible:false});
assert.equal(elements.get('fset').disabled, true);
assert.equal(context.radioFrequency('145'), null);
(async () => {
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
