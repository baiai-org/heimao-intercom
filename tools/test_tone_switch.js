/* The tone-type select used to send "0000" (关闭亚音) when it was switched to
 * CTCSS while the tone list still sat on 0000, so the board stored "off" and the
 * next poll reverted the select.  This pins the fixed decision logic. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const source = fs.readFileSync(path.join(__dirname, '../firmware/main/panel_index.html'), 'utf8');

const start = source.indexOf('function firstToneOf(');
const end = source.indexOf("$('ttx').onchange");
assert(start > 0 && end > start, 'tone decision block present');
const block = source.slice(start, end);

function run(state) {
  const elements = {};
  const mk = (value, values) => ({value, options: values.map(v => ({value: v}))});
  const calls = [];
  const context = {
    $: (id) => elements[id],
    cmd: (k, v) => { calls.push([k, v]); return Promise.resolve(); },
    Date: { now: () => 1000 },
    console,
  };
  for (const [id, spec] of Object.entries(state)) elements[id] = mk(spec.value, spec.options || []);
  const fn = new Function('$', 'cmd', 'Date', block + '; return {pushTx, pushRx, toneToSendTx, toneToSendRx};');
  const api = fn(context.$, context.cmd, context.Date);
  return {api, calls, elements};
}

// 1. Switching the type to CTCSS while the tone list is still "0000" must send a real tone.
let r = run({
  ttx: {value: 'ctcss'}, atx: {value: '0000', options: ['0000', '0001', '0008']},
  trx: {value: 'off'}, arx: {value: '0000', options: ['0000', '0001', '0008']},
  dtx: {value: '', options: []}, dtxp: {value: 'N'}, drx: {value: '', options: []}, drxp: {value: 'N'},
});
r.api.pushTx();
assert.deepEqual(r.calls.at(-1), ['ctcss_tx', '0001'], 'a real tone must be sent, not 0000');
assert.equal(r.elements.atx.value, '0001');

// 2. Turning it off still sends 0000.
r = run({ttx: {value: 'off'}, atx: {value: '0008', options: ['0000', '0008']},
  trx: {value: 'off'}, arx: {value: '0000', options: []}, dtx: {value: ''}, dtxp: {value: 'N'}, drx: {value: ''}, drxp: {value: 'N'}});
r.api.pushTx();
assert.deepEqual(r.calls.at(-1), ['ctcss_tx', '0000']);

// 3. A chosen tone is respected.
r = run({ttx: {value: 'ctcss'}, atx: {value: '0021', options: ['0000', '0001', '0021']},
  trx: {value: 'off'}, arx: {value: '0000', options: []}, dtx: {value: ''}, dtxp: {value: 'N'}, drx: {value: ''}, drxp: {value: 'N'}});
r.api.pushTx();
assert.deepEqual(r.calls.at(-1), ['ctcss_tx', '0021']);

// 4. RX uses the same rule.
r = run({ttx: {value: 'off'}, atx: {value: '0000', options: []},
  trx: {value: 'ctcss'}, arx: {value: '0000', options: ['0000', '0003']}, dtx: {value: ''}, dtxp: {value: 'N'}, drx: {value: ''}, drxp: {value: 'N'}});
r.api.pushRx();
assert.deepEqual(r.calls.at(-1), ['ctcss_rx', '0003']);

console.log('PASS: tone type switches send a real tone instead of 0000, off still zeroes, chosen tones kept');
