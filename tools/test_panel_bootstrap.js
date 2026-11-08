/* Exercise production panel reads, startup and reconnects with local failures.
 * No requests reach a board. Run: node tools/test_panel_bootstrap.js */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const html = fs.readFileSync(path.join(__dirname, '../firmware/main/panel_index.html'), 'utf8');
const source = [...html.matchAll(/<script\b[^>]*>([\s\S]*?)<\/script>/g)].map(m => m[1]).join('\n').replace(/\r\n/g, '\n');
function part(start, end) {
  const a = source.indexOf(start), b = source.indexOf(end, a + start.length);
  assert(a >= 0 && b > a, start);
  return source.slice(a, b);
}
const flush = async () => { for (let i = 0; i < 30; i++) await Promise.resolve(); };
function deferred() {
  let resolve;
  const promise = new Promise(done => { resolve = done; });
  return {promise, resolve};
}
function runtime() {
  const state = {requests: [], logs: [], timers: new Map(), id: 0, renders: 0};
  const elements = new Map([...html.matchAll(/\bid="([^"]+)"/g)].map(m =>
    [m[1], {value: '', style: {}, textContent: '', innerHTML: '', className: ''}]));
  state.status = {ptt: 'IDLE', wifi: {connected: true, rssi: -50, ssid: 'test', ip: '192.0.2.178', host: 'orl2'},
    mode: 'monitor', uptime: 10, heap: 100000, psram: 1000000, freq: {tx: 435, rx: 435, sq: 8, vol: 8, bw: 1},
    intercom: {mode: 0, state: 'idle', peers: 0, tx: 0, rx: 0}, bridge: {}, gain: 80,
    eq_spk: [0, 0, 0, 0, 0, 0], eq_mic: [0, 0, 0, 0, 0, 0]};
  state.radio = {band: 'UHF', compatible: true, ready: true};
  state.values = {'/api/status': state.status, '/api/radio': state.radio,
    '/api/ctcss': [{code: '0008', label: '88.5 Hz'}], '/api/dcs': [{code: '023', label: 'D023'}],
    '/api/channels': [{i: 0, name: 'saved', band: 'UHF', compatible: true, tx: 435, rx: 435}]};
  state.response = url => ({ok: true, status: 200, json: async () => state.values[url]});
  state.fetch = async url => state.response(url);
  const context = vm.createContext({AbortController, Set, window: {},
    document: {activeElement: null},
    $: id => {assert(elements.has(id), id); return elements.get(id);},
    escHtml: text => String(text ?? '').replace(/[&<>"']/g, '_'),
    log: (...args) => state.logs.push(args), wifiIcon: () => '', setSegMeter() {},
    renderChannels: () => {state.renders++;}, syncBridge() {}, applyMode() {},
    renderRadio: r => {context.window._radio = r;}, renderNp2p() {}, tailModesRestore() {},
    holdSyncUntil: Infinity, txOn: false, txEnd: 0, monOn: false, SDR: {on: false}, last: '',
    fetch: (url, options) => {state.requests.push({url, options}); return state.fetch(url, options);},
    setTimeout: (fn, ms) => {const id = ++state.id; state.timers.set(id, {fn, ms}); return id;},
    clearTimeout: id => state.timers.delete(id),
  });
  vm.runInContext(part('const PANEL_READ_PATHS', '/*\n * Factory reset:') +
    part('let tonesReady', 'function showSub()') +
    part('async function loadBridgeChans()', 'function syncBridge(') +
    part('const CH_MAX = 99;', '/* Filtered client-side:') +
    part('async function refresh() {', 'setInterval(refreshMeter,') +
    '\nglobalThis.api = {panelReadJson, loadPanelLists, loadChannels, loadBridgeChans, refresh,' +
    'flags: () => ({tonesReady,dcsReady,channelsReady,panelOnline,panelRefreshBusy})};', context);
  Object.assign(state, context.api); state.elements = elements; state.window = context.window;
  state.finish = async promise => {
    let done = false, value, error;
    promise.then(v => {done = true; value = v;}, e => {done = true; error = e;});
    for (let i = 0; i < 100 && !done; i++) {
      await flush();
      if (done) break;
      const next = [...state.timers.entries()].sort((a, b) => a[1].ms - b[1].ms)[0];
      if (next) { state.timers.delete(next[0]); next[1].fn(); }
    }
    assert(done, 'request/retry was not bounded');
    if (error) throw error;
    return value;
  };
  return state;
}

(async () => {
  let r = runtime();
  const status = deferred(), radio = deferred();
  r.fetch = url => url === '/api/status' ? status.promise : url === '/api/radio' ? radio.promise : Promise.resolve(r.response(url));
  const initial = r.refresh(); await flush();
  assert.equal(r.logs.length, 0, 'no connected claim before a response');
  assert.equal(await r.refresh(), false, 'slow refreshes cannot overlap');
  assert.equal(r.requests.length, 2);
  status.resolve(r.response('/api/status')); await flush();
  assert.equal(r.logs.length, 0, 'radio response is still required');
  radio.resolve(r.response('/api/radio')); assert.equal(await initial, true, r.window._lastErr);
  await r.finish(r.loadPanelLists());
  assert.deepEqual(r.requests.map(x => x.url), ['/api/status', '/api/radio', '/api/ctcss', '/api/dcs', '/api/channels']);
  assert(r.flags().tonesReady && r.flags().dcsReady && r.flags().channelsReady);
  assert.match(r.elements.get('brChan1').innerHTML, /saved/);
  assert.match(r.elements.get('brChan2').innerHTML, /saved/);
  assert.equal(r.renders, 1); assert.equal(r.timers.size, 0);
  assert.equal(r.logs.filter(x => x[0].includes('已连接')).length, 1);

  r = runtime(); let attempts = 0;
  r.fetch = async url => {
    if (url === '/api/ctcss' && ++attempts < 3) throw new TypeError('Failed to fetch');
    return r.response(url);
  };
  await r.finish(r.loadPanelLists());
  assert.equal(attempts, 3); assert.equal(r.flags().tonesReady, true);
  assert.equal(r.logs.length, 0, 'recovered transient failures are not final errors');
  assert(r.requests.every(x => x.options.method === 'GET' && x.options.cache === 'no-store'));

  r = runtime();
  r.fetch = async url => url === '/api/ctcss' ? {ok: false, status: 503} : r.response(url);
  await r.finish(r.loadPanelLists());
  assert.equal(r.requests.filter(x => x.url === '/api/ctcss').length, 3);
  assert.equal(r.flags().tonesReady, false); assert.equal(r.flags().dcsReady, true);
  assert.equal(r.flags().channelsReady, true);
  assert(r.logs.some(x => x[0].includes('/api/ctcss: HTTP 503')));
  r.fetch = async url => r.response(url);
  await r.finish(r.loadPanelLists());
  assert.equal(r.flags().tonesReady, true);
  assert.equal(r.requests.filter(x => x.url === '/api/dcs').length, 1, 'only missing tables recover');

  r = runtime();
  r.fetch = async (url, options) => url !== '/api/ctcss' ? r.response(url) : {
    ok: true, json: () => new Promise((resolve, reject) => {
      options.signal.addEventListener('abort', () => reject(new Error('body timeout')), {once: true});
    }),
  };
  await r.finish(r.loadPanelLists());
  assert.equal(r.requests.filter(x => x.url === '/api/ctcss').length, 3);
  assert.equal(r.flags().tonesReady, false); assert.equal(r.timers.size, 0);
  assert(r.logs.some(x => x[0].includes('body timeout')));

  r = runtime(); const channels = deferred(); r.fetch = () => channels.promise;
  const one = r.loadChannels(), two = r.loadBridgeChans(); await flush();
  assert.equal(r.requests.length, 1, 'channel consumers share an in-flight read');
  channels.resolve(r.response('/api/channels')); await Promise.all([one, two]);
  assert.equal(r.flags().channelsReady, true);
  for (const url of ['/api/set?k=sq&v=1', '/api/talk?on=1', '/api/sdr_ptt?on=1'])
    await assert.rejects(r.panelReadJson(url), /不是面板只读接口/);
  assert.equal(r.requests.length, 1, 'write/PTT URLs are never sent or retried');

  r = runtime(); r.fetch = async () => {throw new TypeError('offline');};
  assert.equal(await r.refresh(), false); assert.equal(r.flags().panelOnline, false);
  assert.equal(r.logs.length, 0);
  r.fetch = async url => r.response(url);
  assert.equal(await r.refresh(), true); await r.finish(r.loadPanelLists());
  assert.equal(await r.refresh(), true);
  assert.equal(r.logs.filter(x => x[0].includes('已连接')).length, 1);
  r.fetch = async () => {throw new TypeError('offline');};
  assert.equal(await r.refresh(), false);
  r.fetch = async url => r.response(url);
  assert.equal(await r.refresh(), true); await r.finish(r.loadPanelLists());
  assert.equal(r.logs.filter(x => x[0].includes('已连接')).length, 2);
  assert.equal(r.requests.filter(x => x.url === '/api/channels').length, 2);
  assert.equal(r.requests.filter(x => x.url === '/api/ctcss').length, 1);
  console.log('PASS: truthful connection, serial table startup, bounded retries/body timeout, shared channel read, reconnect recovery and no PTT/write replay');
})().catch(error => {console.error(error); process.exitCode = 1;});
