// Exercise production Worker code with sequenced PCM and a deterministic clock.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const source = fs.readFileSync(path.join(__dirname, '../firmware/main/audio_spectrum.js'), 'utf8');
const replies = [];
const requests = [];
const pending = [];
const timers = new Map();
let clock = 1000;
let lastTimer = 0;
const worker = {postMessage: value => replies.push(value)};
vm.runInNewContext(source, {
  self: worker,
  performance: {timeOrigin: 0, now: () => clock},
  AbortController,
  URLSearchParams,
  setTimeout(fn, delay) {
    timers.set(++lastTimer, {fn, delay});
    return lastTimer;
  },
  clearTimeout(id) { timers.delete(id); },
  fetch(url) {
    requests.push(new URL(url, 'http://fixture'));
    assert(pending.length, 'unexpected poll');
    const buffer = pending.shift();
    return Promise.resolve({ok:true, arrayBuffer: () => Promise.resolve(buffer)});
  }
});

function packet(first, count, generation = 1, pathId = 1) {
  const buffer = new ArrayBuffer(44 + 2 * count);
  const view = new DataView(buffer);
  const fields = [0x01435041, 1, generation, 1 << pathId, clock - 100, clock - 100,
    first, first + count, count, pathId, 0];
  fields.forEach((value, index) => view.setUint32(4 * index, value, true));
  for (let i = 0; i < count; i++) {
    const t = (i + first) / 16000;
    const pcm = 6000 * Math.sin(2 * Math.PI * 1000 * t) +
                4200 * Math.sin(2 * Math.PI * 1046.875 * t);
    view.setInt16(44 + 2 * i, Math.round(pcm), true);
  }
  return buffer;
}

const settle = () => new Promise(resolve => setImmediate(resolve));

async function poll(buffer) {
  assert.equal(timers.size, 1);
  const [id, timer] = timers.entries().next().value;
  timers.delete(id);
  clock += timer.delay;
  pending.push(buffer);
  timer.fn();
  await settle();
}

(async () => {
  pending.push(packet(0, 1600));
  worker.onmessage({data:{type:'radio', epoch:7, selection:null}});
  await settle();
  assert.equal(replies.at(-1).rows.length, 0);
  for (let i = 1; i < 11; i++) await poll(packet(i * 1600, 1600));
  const rows = replies.flatMap(item => item.rows);
  assert.equal(rows.length, 25);
  for (let i = 0; i < rows.length; i++) {
    const bins = new Uint8Array(rows[i].buffer);
    assert.equal(bins.length, 1009);
    if (i) assert.equal(rows[i].at - rows[i-1].at, 40);
    for (const peak of [112,118]) assert(bins[peak] > Math.max(bins[peak-1], bins[peak+1]));
  }
  assert.equal(requests.at(-1).searchParams.get('after'), '16000');
  const seen = replies.length;
  await poll(new ArrayBuffer(12));
  assert.equal(replies.length, seen, 'isolated malformed response changed status');
  await poll(packet(17600, 1600));
  assert.equal(requests.at(-1).searchParams.get('after'), '17600');
  await poll(packet(19600, 1280));
  assert(replies.at(-1).gap);
  assert.equal(replies.at(-1).rows.length, 0);
  await poll(packet(0, 1000, 2, 2));
  assert(replies.at(-1).reset);
  assert.equal(replies.at(-1).rows.length, 0);
  worker.onmessage({data:{type:'stop', epoch:8}});
  assert.equal(timers.size, 0);
  console.log('PASS: 25 real windows/s, two narrow peaks, gap isolation, source change and malformed reply recovery');
})().catch(error => { console.error(error); process.exitCode = 1; });
