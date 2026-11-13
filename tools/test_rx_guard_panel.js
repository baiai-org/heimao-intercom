/* Exercise production web settings with a small DOM double; no board access. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const source = fs.readFileSync(path.join(__dirname, '../firmware/main/rx_guard.js'), 'utf8');
const context = vm.createContext({window: {}, console});
vm.runInContext(source, context);
const api = context.window.ORLRxGuard;

class Element {
  constructor() { this.parts = {}; this.boxes = []; this.listeners = {}; this.value = ''; this.textContent = ''; }
  set innerHTML(html) {
    this.html = html;
    for (const m of html.matchAll(/data-rg="([^"]+)"/g)) this.parts[m[1]] = new Element();
    this.boxes = [...html.matchAll(/<input type="checkbox" value="(\d+)"/g)].map(m => Object.assign(new Element(), {value: m[1], checked: false}));
  }
  querySelector(selector) { return this.parts[selector.match(/"(.*?)"/)[1]]; }
  querySelectorAll(selector) {
    if (selector === 'input') return this.boxes;
    return [...Object.values(this.parts), ...Object.values(this.parts).flatMap(p => p.boxes)];
  }
  addEventListener(name, fn) { this.listeners[name] = fn; }
}
function fixture(mode = 1) {
  const root = new Element();
  const calls = [];
  let reply = async () => ({ok: true});
  const view = api.mount(root, mode, async (key, value) => { calls.push({key, value}); return reply(); });
  const get = name => root.parts[name];
  get('step').value = '1'; get('unit').value = 's';
  const state = {intercom: {mode}, bridge: {active: 1}, rx_guard: {version: 1, blocked: 0, remaining_ms: 0,
    p2p: {en: 1, ms: 1234, sources: 7, outputs: 7}, relay: {en: 0, ms: 2000, sources: 1, outputs: 3}}};
  return {root, view, get, calls, state, setReply: r => {reply = r;}};
}

(async () => {
  for (const [s, ms] of [['0.001', 1], ['0.037', 37], ['1.234', 1234], ['60.000', 60000]]) {
    assert.equal(api.parseDelay(s, 's'), ms); assert.equal(api.formatDelay(ms, 's'), s);
  }
  for (const s of ['0', '0.0001', '60.001', '1e2', '-1', '+1', '1.', '', 'NaN', '999999999999999999999']) assert.throws(() => api.parseDelay(s, 's'));
  for (const s of ['0', '1.5', '60001', '1e3']) assert.throws(() => api.parseDelay(s, 'ms'));
  assert.equal(api.encode(true, 1234, 5, 3), '1,1234,5,3');
  assert.throws(() => api.encode(true, 1, 0, 7)); assert.throws(() => api.encode(true, 1, 7, 0));
  const f = fixture(); f.view.sync(f.state);
  assert.equal(f.get('delay').value, '1.234'); assert.equal(f.get('form').hidden, false);
  f.get('plus').onclick(); assert.equal(f.get('delay').value, '1.235');
  f.state.rx_guard.p2p.ms = 4000; f.view.sync(f.state);
  assert.equal(f.get('delay').value, '1.235', 'poll must preserve draft');
  f.get('unit').value = 'ms'; f.get('unit').onchange(); assert.equal(f.get('delay').value, '1235');
  f.get('delay').value = '37'; f.get('save').onclick(); await Promise.resolve(); await Promise.resolve();
  assert.deepEqual(f.calls[0], {key: 'rx_guard_p2p', value: '1,37,7,7'});
  f.view.sync(f.state); f.get('reset').onclick(); assert.equal(f.get('delay').value, '4000');
  f.get('delay').value = '1.5'; await f.get('save').onclick(); assert.equal(f.calls.length, 1);
  f.get('delay').value = '2000';
  f.setReply(async () => ({ok: false, err: 'NVS failed'})); await f.get('save').onclick();
  assert.equal(f.get('message').textContent, 'NVS failed'); assert.equal(f.get('save').disabled, false);
  let finish; f.setReply(() => new Promise(resolve => {finish = resolve;}));
  const pending = f.get('save').onclick(); assert.equal(f.get('save').disabled, true);
  await f.get('save').onclick(); assert.equal(f.calls.length, 3, 'double save must not send twice');
  finish({ok: true}); await pending;
  f.state.rx_guard.blocked = 7; f.state.rx_guard.remaining_ms = 37; f.view.sync(f.state);
  assert.match(f.get('state').textContent, /0.037/);
  f.view.sync({}); assert.equal(f.get('form').hidden, false); assert.equal(f.get('save').disabled, true); assert.match(f.get('state').textContent, /状态暂缺/); await f.get('save').onclick(); assert.equal(f.calls.length, 3);
  const r = fixture(2); r.view.sync(r.state); await r.get('save').onclick();
  assert.deepEqual(r.calls[0], {key: 'rx_guard_relay', value: '0,2000,1,3'});
  const html = fs.readFileSync(path.join(__dirname, '../firmware/main/panel_index.html'), 'utf8');
  for (const m of html.matchAll(/<script\b[^>]*>([\s\S]*?)<\/script>/g)) new vm.Script(m[1]);
  console.log('PASS: ms/seconds precision; malformed input; atomic save/masks; draft polling; mode independence; failures; duplicate save; old firmware; panel syntax');
})().catch(e => { console.error(e); process.exitCode = 1; });
