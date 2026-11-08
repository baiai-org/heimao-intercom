/* Execute the production browser/AudioWorklet code with local stand-ins.
 * No microphone, network, or board is used. Run: node tools/test_sdr_panel.js */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const html = fs.readFileSync(path.join(__dirname, '../firmware/main/panel_index.html'), 'utf8');
const start = html.indexOf('/* ------------------------------------------------------------------ SDR --- */');
const end = html.indexOf('/* Mobile Section Filter Navigation */', start);
assert(start >= 0 && end > start);
const code = html.slice(start, end);
const deferred = () => {
  let resolve, reject;
  const promise = new Promise((a, b) => { resolve = a; reject = b; });
  return {promise, resolve, reject};
};
const flush = async () => { for (let i = 0; i < 16; i++) await Promise.resolve(); };
function runtime() {
  const state = {now: 0, timers: new Map(), id: 0, requests: [], logs: [], nodes: [], sockets: [], micOptions: []};
  const elements = new Map([...html.matchAll(/\bid="([^"]+)"/g)].map(m =>
    [m[1], {value: '', style: {}, textContent: '', className: '', disabled: false,
      addEventListener() {}, appendChild() {}}]));
  elements.get('secs').value = '30';
  const node = () => ({connect() {}, disconnect() {}, gain: {value: 1}});
  class AudioContext {
    constructor() {
      this.sampleRate = 48000; this.currentTime = 0; this.state = 'running'; this.destination = {};
      this.audioWorklet = {addModule: async () => {}};
    }
    createGain() { return node(); }
    createMediaStreamSource() { return node(); }
    async resume() { this.state = 'running'; }
    async close() { this.state = 'closed'; }
  }
  class AudioWorkletNode {
    constructor(ctx, kind) {
      Object.assign(this, node()); this.kind = kind; this.sent = [];
      this.port = {postMessage: msg => this.sent.push(msg), onmessage: null};
      state.nodes.push(this);
    }
  }
  class WebSocket {
    constructor() { this.readyState = 0; this.bufferedAmount = 0; this.sent = []; state.sockets.push(this); }
    send(data) { if (this.readyState !== 1) throw Error('closed'); this.sent.push(data); }
    close() { this.readyState = 3; }
    ack(id, ptt) {
      const msg = this.sent.find(v => typeof v === 'string' && v.endsWith(':' + id));
      assert(msg, 'ACK must correspond to a real wire command');
      const [, sid, tx] = msg.split(':');
      this.onmessage({data: JSON.stringify({id, sid: +sid, tx: +tx, ptt, ok: true})});
    }
  }
  const TestURL = class extends URL {};
  TestURL.createObjectURL = () => 'blob:test'; TestURL.revokeObjectURL = () => {};
  state.stream = () => {
    const track = {label: 'local mock', stopped: 0, stop() { this.stopped++; }};
    return {track, getTracks: () => [track], getAudioTracks: () => [track]};
  };
  state.getMic = async () => state.stream();
  state.response = url => {
    const u = new URL(url, 'https://board'), q = u.searchParams;
    const data = {ok: true, sid: +(q.get('sid') || 4000000001),
      tx: +(q.get('tx') || state.SDR?.activeTx || 0), id: +(q.get('id') || 0),
      on: q.get('on') !== '0', ptt: q.get('on') === '1', reason: 'test'};
    return {ok: true, json: async () => data, arrayBuffer: async () => new ArrayBuffer(0)};
  };
  state.fetch = async url => state.response(url);
  const context = vm.createContext({
    window: {isSecureContext: true, AudioContext, _radio: {ready: true}, addEventListener() {}},
    document: {hidden: false, addEventListener() {}, createElement: () => ({})},
    navigator: {mediaDevices: {getUserMedia: args => {state.micOptions.push(args); return state.getMic(args);},
      enumerateDevices: async () => []}},
    location: {protocol: 'https:', host: 'board', hostname: 'board', port: ''},
    localStorage: {getItem() {}, setItem() {}}, URL: TestURL, URLSearchParams, Blob,
    AudioWorkletNode, WebSocket, AbortController, Int16Array, Float32Array, Uint8Array, ArrayBuffer,
    performance: {now: () => state.now},
    setTimeout: (fn, ms) => {const id = ++state.id; state.timers.set(id, {fn, ms}); return id;},
    clearTimeout: id => state.timers.delete(id),
    setInterval: (fn, ms) => {const id = ++state.id; state.timers.set(id, {fn, ms, interval: true}); return id;},
    clearInterval: id => state.timers.delete(id),
    fetch: (url, options) => {state.requests.push({url, options}); return state.fetch(url, options);},
    log: (...args) => state.logs.push(args),
    $: id => {assert(elements.has(id), id); return elements.get(id);},
  });
  vm.runInContext(code + '\nglobalThis.api = {SDR, SdrPlayback, sdrWorkletSource, sdrRxStart, sdrPttDown, sdrPttUp, sdrConnect, sdrSendAudio, sdrStop, sdrToggle, sdrHttpStart, sdrRequest, sdrApplyState};', context);
  Object.assign(state, context.api); state.elements = elements;
  state.ready = async (http = true) => {
    await state.sdrRxStart(); state.SDR.on = true; state.SDR.sid = 4000000001; state.SDR.http = http;
    if (!http) {
      state.sdrConnect(); const ws = state.SDR.ws; ws.readyState = 1; ws.onopen();
    }
  };
  return state;
}
async function audioTests() {
  const r = runtime();
  // The module string that is actually loaded by the browser must be executable.
  for (const rate of [48000, 44100, 16000]) {
    const processors = new Map(), messages = [];
    class Processor { constructor() {this.port = {postMessage: msg => messages.push(msg)};} }
    const worker = vm.createContext({AudioWorkletProcessor: Processor, sampleRate: rate,
      currentTime: 0, registerProcessor: (name, cls) => processors.set(name, cls)});
    vm.runInContext(r.sdrWorkletSource(), worker);
    const rec = new (processors.get('radio-recorder'))({processorOptions: {wire: 16000}});
    rec.port.onmessage({data: {on: true, epoch: 7}});
    for (let pos = 0; pos < rate; pos += 128) {
      const input = new Float32Array(Math.min(128, rate - pos)).fill(0.1);
      const out = new Float32Array(input.length).fill(1); worker.currentTime = pos / rate;
      rec.process([[input]], [[out]]);
      assert(out.every(v => v === 0), 'microphone never feeds speaker');
    }
    assert.equal(messages.length, 25, rate + ' must produce exactly 25 packets/s');
    assert.equal(messages.reduce((n, m) => n + m.samples.length, 0), 16000);
    assert(messages.every(m => m.epoch === 7 && m.samples.byteLength === 1280));
    assert(messages[24].samples.every(v => v >= 3275 && v <= 3278));
    rec.port.onmessage({data: {on: false}});
    rec.process([[new Float32Array(128)]], [[new Float32Array(128)]]);
    assert.equal(messages.length, 25);
    const player = new (processors.get('radio-player'))({processorOptions: {wire: 16000}});
    player.port.onmessage({data: {type: 'push', samples: new Int16Array(16000).fill(10000)}});
    const output = new Float32Array(128); player.process([], [[output]]);
    assert(output.some(v => v > 0.1));
  }
  const p = new r.SdrPlayback(16000, 48000);
  const out = new Float32Array(128);
  p.push(new Int16Array(14000).fill(10000)); p.render(out);
  assert(out.every(v => v === 0)); // Initial buffering is preserved.
  p.push(new Int16Array(2000).fill(10000));
  for (let i = 0; i < 400; i++) p.render(out);
  assert.equal(p.underruns, 1); assert.equal(p.primed, false);
  p.push(new Int16Array(320).fill(10000)); p.render(out);
  assert.equal(p.primed, false); assert(out.every(v => v === 0));
  p.push(new Int16Array(4800).fill(10000)); p.render(out);
  assert.equal(p.primed, true); assert(out.some(v => v > 0.1));
  p.clear(); assert.equal(p.n, 0); assert.equal(p.pos, 0);
  p.push(new Int16Array(33000)); assert.equal(p.n, 32000); assert.equal(p.dropped, 1000);
}

async function lifecycleTests() {
  let r = runtime(); await r.ready();
  const permission = deferred(), stream = r.stream(); r.getMic = () => permission.promise;
  const pending = r.sdrPttDown(); await flush();
  assert.equal(r.micOptions.length, 1); assert.equal(r.requests.length, 0);
  r.sdrPttUp(); permission.resolve(stream); await pending;
  assert.equal(stream.track.stopped, 1); assert.equal(r.requests.length, 0);
  assert.equal(r.SDR.micStream, null);

  r = runtime(); await r.ready(); r.getMic = async () => {throw Error('denied');};
  await r.sdrPttDown(); assert.equal(r.requests.length, 0); assert.equal(r.SDR.ptt, false);
  assert.deepEqual(JSON.parse(JSON.stringify(r.micOptions[0].audio)),
    {channelCount: 1, echoCancellation: false, noiseSuppression: false, autoGainControl: false});

  r = runtime(); await r.ready(false);
  let down = r.sdrPttDown(); await flush();
  let ws = r.SDR.ws, recorder = r.nodes.find(n => n.kind === 'radio-recorder');
  assert.equal(ws.sent[0], 'ptt1:4000000001:1:1'); assert.equal(r.SDR.keyed, false);
  assert.equal(recorder.sent.length, 0);
  // An ACK from an older page/TX cannot enable capture.
  ws.onmessage({data: JSON.stringify({ok:true,sid:4000000000,tx:1,id:1,ptt:true})});
  assert.equal(r.SDR.keyed, false);
  ws.ack(1, true); await down;
  assert.equal(r.SDR.keyed, true); assert.equal(recorder.sent[0].on, true);
  const samples = new Int16Array(3200).map((_, i) => i*17-25000);
  r.sdrSendAudio(samples, 0);
  const binary = ws.sent.filter(v => v instanceof ArrayBuffer);
  assert.equal(binary.reduce((n, b) => n+b.byteLength-8, 0), samples.byteLength);
  let pos=0;
  for (const frame of binary) {
    assert(frame.byteLength<=2048);
    const view=new DataView(frame);
    assert.equal(view.getUint32(0,true),4000000001); assert.equal(view.getUint32(4,true),1);
    const data=new Int16Array(frame,8);
    assert.deepEqual(data,samples.subarray(pos,pos+data.length));pos+=data.length;
  }
  r.sdrPttUp(); assert.equal(r.SDR.micStream, null);
  assert.equal(ws.sent.at(-1),'ptt0:4000000001:1:2');
  ws.ack(2, false); assert.equal(await r.SDR.releasePending, true);
  assert.equal(r.SDR.ctx.state, 'running');
  down=r.sdrPttDown();await flush();assert.equal(ws.sent.at(-1),'ptt1:4000000001:2:3');
  ws.ack(3,true);await down;
  r.sdrApplyState({sid:4000000001,on:true,tx:1,ptt:false,reason:'pcm_timeout'});
  assert.equal(r.SDR.keyed,true); // delayed state from previous TX
  const count=ws.sent.length;
  r.sdrApplyState({sid:4000000001,on:true,tx:2,ptt:false,reason:'pcm_timeout'});
  assert.equal(r.SDR.keyed,false);assert.equal(r.SDR.micStream,null);
  assert.equal(ws.sent.length,count); // board already ended: no new unscoped release
  assert(r.logs.some(log=>String(log[0]).includes('pcm_timeout')));

  r = runtime(); await r.ready(false);
  down = r.sdrPttDown(); await flush();
  r.sdrPttUp(); r.SDR.ws.ack(1, true); await down;
  assert.equal(r.SDR.keyed, false);
  r.SDR.ws.ack(2, false); assert.equal(await r.SDR.releasePending, true);

  r = runtime(); await r.ready();
  const key=deferred(),release=deferred();
  r.fetch=url => new URL(url,'https://board').searchParams.get('on')==='1' ? key.promise:release.promise;
  down=r.sdrPttDown();await flush();r.sdrPttUp();await flush();
  assert.equal(r.requests.length,1);
  key.resolve(r.response(r.requests[0].url));await down;await flush();
  assert.equal(r.requests.length,2);
  const q0=new URL(r.requests[0].url,'https://board').searchParams;
  const q1=new URL(r.requests[1].url,'https://board').searchParams;
  assert.equal(q0.get('tx'),q1.get('tx'));assert.equal(q0.get('sid'),q1.get('sid'));
  assert.equal(q1.get('on'),'0');
  release.resolve(r.response(r.requests[1].url));
  assert.equal(await r.SDR.releasePending,true);assert.equal(r.SDR.keyed,false);

  r=runtime();await r.ready(false);
  down=r.sdrPttDown();await flush();r.SDR.ws.readyState=3;r.SDR.ws.onclose({code:1006});
  await down;await r.SDR.releasePending;
  assert.equal(r.SDR.ptt,false);assert.equal(r.SDR.micStream,null);assert.equal(r.SDR.http,true);
  assert(r.requests.some(q=>q.url.includes('/api/sdr_ptt?on=0&sid=4000000001&tx=1')));

  r=runtime();await r.ready(false);
  down=r.sdrPttDown();await flush();r.SDR.ws.ack(1,true);await down;
  const blocked=r.SDR.ws;blocked.bufferedAmount=9000;
  r.sdrSendAudio(new Int16Array(640),0);await r.SDR.releasePending;
  assert.equal(blocked.readyState,3);assert.equal(r.SDR.http,true);assert.equal(r.SDR.ptt,false);

  // A new page taking over stale SDR stops old local audio only. It never
  // issues a close/off against the new page's session.
  r=runtime();await r.ready(false);
  down=r.sdrPttDown();await flush();r.SDR.ws.ack(1,true);await down;
  const writes=r.requests.length;
  r.sdrApplyState({sid:4000000002,on:true,tx:1,ptt:true});await flush();
  assert.equal(r.SDR.on,false);assert.equal(r.SDR.micStream,null);assert.equal(r.SDR.sid,0);
  assert.equal(r.requests.length,writes);

  r=runtime();await r.ready();
  const off=deferred();
  r.fetch=url=>url.startsWith('/api/sdr?on=0')?off.promise:Promise.resolve(r.response(url));
  const stop=r.sdrStop(),reopen=r.sdrToggle();await flush();
  assert.equal(r.requests.length,1);assert(r.requests[0].url.startsWith('/api/sdr?on=0&sid=4000000001'));
  off.resolve(r.response(r.requests[0].url));await stop;await reopen;
  assert(r.requests[1].url.startsWith('/api/sdr?on=1&client='));assert.equal(r.SDR.on,true);
}

async function transportTests() {
  let r=runtime();await r.ready();
  r.SDR.ptt=r.SDR.keyed=true;r.SDR.activeTx=1;
  const post=deferred();let active=0,max=0;
  r.fetch=async()=>{active++;max=Math.max(max,active);const v=await post.promise;active--;return v;};
  r.sdrSendAudio(new Int16Array(640),0);
  for(let i=0;i<25;i++) {r.SDR.ctx.currentTime=i*0.04;r.sdrSendAudio(new Int16Array(640),r.SDR.ctx.currentTime);}
  assert(r.SDR.txQueued<=8000);assert(r.SDR.txDrop>0);assert.equal(r.requests.length,1);assert.equal(max,1);
  r.SDR.ctx.currentTime+=1;post.resolve(r.response(r.requests[0].url));await r.SDR.txIdle;
  assert.equal(r.requests.length,1);assert.equal(r.SDR.txQueued,0);
  assert(r.requests[0].url.includes('sid=4000000001&tx=1'));
  r.SDR.ctx.currentTime=2;
  const batch=deferred();r.fetch=()=>batch.promise;
  r.sdrSendAudio(new Int16Array(640),2);
  for(let i=0;i<10;i++)r.sdrSendAudio(new Int16Array(640),2);
  batch.resolve(r.response(r.requests[0].url));await r.SDR.txIdle;
  assert(r.requests.every(q=>q.options.body.byteLength<=8192));
  assert(r.requests.some(q=>q.options.body.byteLength>1280));
  for(const [samples,elapsed,delay] of [[5120,180,0],[640,100,0],[640,10,40]]) {
    r=runtime();r.SDR.on=true;r.SDR.sid=4000000001;
    r.fetch=async()=>({ok:true,arrayBuffer:async()=>{r.now+=elapsed;return new Int16Array(samples).buffer;}});
    r.sdrHttpStart();await flush();
    const timers=[...r.timers.values()].filter(t=>!t.interval);
    assert.equal(timers.length,1);assert.equal(timers[0].ms,delay);
  }
  r=runtime();r.fetch=async(url,options)=>({ok:true,arrayBuffer:()=>new Promise((a,reject)=>{
    options.signal.addEventListener('abort',()=>reject(Error('aborted')));
  })});
  const read=r.sdrRequest('/body-stalls',{},'pcm');await flush();
  [...r.timers.values()].find(t=>t.ms===3000).fn();await assert.rejects(read,/aborted/);
}
(async()=>{
  await audioTests();await lifecycleTests();await transportTests();
  console.log('PASS: production worklets at 48/44.1/16 kHz; sid/TX ACK and PCM framing; cancellation, old state, watchdog UI, HTTP queues/throughput and session replacement');
})().catch(e=>{console.error(e);process.exitCode=1;});
