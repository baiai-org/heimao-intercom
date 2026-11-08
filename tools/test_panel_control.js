/* The panel bar no longer owns or asks for control: it only shows identity and a
 * link to /setup.  This test pins that contract (no /api/control traffic). */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const source = fs.readFileSync(path.join(__dirname, '../firmware/main/app_control.js'), 'utf8');

const flush = async () => { for (let i = 0; i < 40; i++) await Promise.resolve(); };

function runtime() {
  const elements = [];
  const make = () => ({style: {}, textContent: '', children: [], append(...x) { this.children = x; }});
  const document = { createElement: () => { const e = make(); elements.push(e); return e; }, body: {prepend() {}} };
  const calls = [];
  const json = (v) => ({ok: true, status: 200, json: async () => v});
  const fetch = async (url) => {
    const p = new URL(String(url), 'https://board/').pathname;
    calls.push(p);
    if (p === '/api/device') return json({device_id: 'orl-test', api_version: 2});
    return json({ok: true});
  };
  const window = { fetch, WebSocket: class { constructor(u) { this.url = u; } }, addEventListener() {}, dispatchEvent() {} };
  const context = vm.createContext({ window, document, location: {href: 'https://board/', origin: 'https://board'}, URL, Headers,
    crypto: {getRandomValues: (a) => { a.fill(1); return a; }}, Event: class {}, setInterval: () => 1, clearInterval: () => {},
    console, Promise, JSON, Error, Object, Array });
  vm.runInContext(source, context);
  return {elements, calls, context};
}

(async () => {
  const r = runtime();
  await flush();
  assert.equal(r.context.window.ORLControl.token, '', 'no lease token any more');
  assert.equal(r.context.window.ORLControl.device, 'orl-test');
  await r.context.window.ORLControl.ensure();
  assert.equal(r.calls.filter(p => p === '/api/control').length, 0, 'the bar must not talk to /api/control');
  const bar = r.elements[0];
  assert(bar && bar.children.length === 2, 'bar has status + setup link');
  console.log('PASS: passive panel bar, identity only, no control traffic');
})().catch(e => { console.error(e); process.exitCode = 1; });
