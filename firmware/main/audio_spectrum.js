(() => {
  'use strict';
  const sampleRate = 16000;
  const fftSize = 2048;
  const hop = 640;
  const rowMs = 1000 * hop / sampleRate;
  const firstBin = 16;
  const bandCount = fftSize / 2 - firstBin + 1;
  const historyCount = 250;
  const ringSize = historyCount + 16;
  const minimumDb = -78;
  const maximumDb = -16;

  // The browser PTT preview analyses the actual outgoing microphone frames
  // off the UI thread and avoids HTTP polling during that PTT session.
  if (typeof document === 'undefined') {
    const bitCount = Math.log2(fftSize);
    const reverse = Uint16Array.from({ length: fftSize }, (_, index) => {
      let value = index;
      let bits = 0;
      for (let bit = 0; bit < bitCount; bit++) {
        bits = (bits << 1) | (value & 1);
        value >>= 1;
      }
      return bits;
    });
    const windowWeights = Float64Array.from({ length: fftSize }, (_, i) =>
      0.5 - 0.5 * Math.cos(2 * Math.PI * i / (fftSize - 1))
    );
    const windowSum = windowWeights.reduce((sum, value) => sum + value, 0);
    const cosine = Float64Array.from({ length: fftSize / 2 }, (_, i) => Math.cos(-2 * Math.PI * i / fftSize));
    const sine = Float64Array.from({ length: fftSize / 2 }, (_, i) => Math.sin(-2 * Math.PI * i / fftSize));
    const re = new Float64Array(fftSize);
    const im = new Float64Array(fftSize);

    function spectrum(pcm) {
      for (let index = 0; index < fftSize; index++) {
        re[reverse[index]] = pcm[index] / 32768 * windowWeights[index];
        im[index] = 0;
      }
      for (let span = 2; span <= fftSize; span *= 2) {
        const half = span / 2;
        const step = fftSize / span;
        for (let group = 0; group < fftSize; group += span) {
          for (let index = 0; index < half; index++) {
            const a = group + index;
            const b = a + half;
            const w = index * step;
            const tr = re[b] * cosine[w] - im[b] * sine[w];
            const ti = re[b] * sine[w] + im[b] * cosine[w];
            re[b] = re[a] - tr;
            im[b] = im[a] - ti;
            re[a] += tr;
            im[a] += ti;
          }
        }
      }
      return Uint8Array.from({ length: bandCount }, (_, index) => {
        const bin = firstBin + index;
        const normalization = bin === fftSize / 2 ? 1 : 2;
        const magnitude = normalization * Math.hypot(re[bin], im[bin]) / windowSum;
        const db = 20 * Math.log10(Math.max(1e-10, magnitude));
        return Math.round(255 * Math.min(1, Math.max(0, (db - minimumDb) / (maximumDb - minimumDb))));
      });
    }
    const headerBytes = 44;
    const maxSamples = 5120;
    const window = new Int16Array(fftSize);
    let fill = 0;
    let session = null;

    function rows(pcm, startAt, first) {
      const result = [];
      for (let pos = 0; pos < pcm.length;) {
        const take = Math.min(fftSize - fill, pcm.length - pos);
        window.set(pcm.subarray(pos, pos + take), fill);
        fill += take;
        pos += take;
        if (fill === fftSize) {
          result.push({at: startAt + pos / (sampleRate / 1000), sampleEnd: first + pos,
            buffer: spectrum(window).buffer});
          window.copyWithin(0, hop);
          fill -= hop;
        }
      }
      return result;
    }

    function reply(current, output, info = {}) {
      self.postMessage({epoch: current.epoch, rows: output, ...info}, output.map(row => row.buffer));
    }

    function decode(buffer, current, received, rtt) {
      if (buffer.byteLength < headerBytes) throw new Error('format');
      const view = new DataView(buffer);
      const boot = view.getUint32(4, true);
      const generation = view.getUint32(8, true);
      const mask = view.getUint32(12, true);
      const now = view.getUint32(16, true);
      const stamp = view.getUint32(20, true);
      const first = view.getUint32(24, true);
      const latest = view.getUint32(28, true);
      const count = view.getUint32(32, true);
      const path = view.getUint32(36, true);
      const contention = view.getUint32(40, true);
      if (view.getUint32(0, true) !== 0x01435041 || !generation || path > 16 ||
          count > maxSamples || first + count > latest || (count && !path) ||
          buffer.byteLength !== headerBytes + count * 2) throw new Error('format');

      const reboot = current.boot !== boot;
      const restart = reboot || current.generation !== generation;
      const gap = !restart && first !== current.after;
      const elapsed = reboot || current.boardNow === null ? now : current.boardElapsed + ((now - current.boardNow) >>> 0);
      current.boardNow = now;
      current.boardElapsed = elapsed;
      const offset = received - elapsed - rtt / 2;
      // Requests delayed in the server queue give a biased clock estimate.
      // Use the fastest observed exchange for this device boot.
      if (reboot || rtt < current.clockRtt) {
        current.clockRtt = rtt;
        current.clockOffset = offset;
      }
      if (restart || gap) {
        fill = 0;
        current.anchor = null;
      }
      let clockGap = false;
      if (count) {
        const measured = elapsed + current.clockOffset - ((now - stamp) >>> 0) - latest / (sampleRate / 1000);
        if (current.anchor === null) current.anchor = measured;
        else {
          const drift = measured - current.anchor;
          // A source can pause briefly without ending its route. Move forward
          // if those gaps accumulate, preserving the order of existing rows.
          if (drift > 80) {
            current.anchor = measured;
            clockGap = true;
          } else if (drift > 8) current.anchor += Math.min(4, drift / 8);
        }
      }
      const pcm = new Int16Array(buffer, headerBytes, count);
      const started = performance.now();
      const output = count ? rows(pcm, current.anchor + first / (sampleRate / 1000), first) : [];
      const fftMs = output.length ? (performance.now() - started) / output.length : 0;
      const reset = reboot || (current.path && path && current.path !== path);
      current.boot = boot;
      current.generation = generation;
      if (path) current.path = path;
      current.after = first + count;
      current.selection = null;
      reply(current, output, {boot, generation, mask, path, gap: clockGap || gap || (restart && !reset && !!count),
        samples: count, rtt, fftMs, contention,
        sourceLag: count ? ((now - stamp) >>> 0) + (latest - first - count) / (sampleRate / 1000) : 0});
      return {mask, behind: latest - current.after};
    }

    async function read(current) {
      if (session !== current) return;
      const started = performance.now();
      const controller = new AbortController();
      current.controller = controller;
      const timeout = setTimeout(() => controller.abort(), 900);
      const query = new URLSearchParams({after: current.after, generation: current.generation});
      if (current.selection !== null) query.set('select', current.selection);
      let interval = 100;
      try {
        const response = await fetch('/api/audio_spectrum?' + query, {cache:'no-store', signal:controller.signal});
        if (!response.ok) throw new Error(response.status === 503 ? 'busy' : 'network');
        const buffer = await response.arrayBuffer();
        if (session !== current) return;
        const elapsed = performance.now() - started;
        const {mask, behind} = decode(buffer, current, performance.timeOrigin + performance.now(), elapsed);
        current.errors = 0;
        current.lastGood = performance.now();
        interval = behind > hop ? 0 : mask ? 100 : 240;
      } catch (error) {
        if (session !== current) return;
        const reason = error.message === 'format' || error.message === 'busy' ? error.message : 'network';
        if (++current.errors > 1 && performance.now() - current.lastGood >= 1000) {
          reply(current, [], {problem: reason});
        }
        interval = Math.min(800, 200 * current.errors);
      } finally {
        clearTimeout(timeout);
        if (session === current) {
          current.timer = setTimeout(() => void read(current), Math.max(0, interval - (performance.now() - started)));
        }
      }
    }

    self.onmessage = ({data}) => {
      if (data.type === 'pcm') {
        if (session?.mode !== 'microphone' || session.epoch !== data.epoch) return;
        const pcm = new Int16Array(data.buffer);
        if (session.after !== data.first) {
          fill = 0;
          session.anchor = null;
        }
        if (session.anchor === null) {
          session.anchor = data.at - (data.first + pcm.length) / (sampleRate / 1000);
        }
        session.after = data.first + pcm.length;
        reply(session, rows(pcm, session.anchor + data.first / (sampleRate / 1000), data.first),
          {samples: pcm.length});
        return;
      }
      session?.controller?.abort();
      clearTimeout(session?.timer);
      session = null;
      fill = 0;
      if (data.type === 'stop') return;
      session = {mode: data.type, epoch: data.epoch, selection: data.selection,
        generation: 0, boot: null, boardNow: null, clockRtt: Infinity, clockOffset: 0, after: 0, anchor: null,
        errors: 0, lastGood: performance.now()};
      if (session.mode === 'radio') void read(session);
    };
    return;
  }

  const root = document.getElementById('audioSpectrum');
  if (!root) return;
  const canvas = root.querySelector('[data-spectrum-axes]');
  const output = canvas.getContext('2d');
  const frame = document.createElement('canvas');
  const ctx = frame.getContext('2d');
  if (!output || !ctx) return;
  const buttons = [...root.querySelectorAll('[data-view]')];
  const sourceLabel = root.querySelector('[data-spectrum-source]');
  const pathSelect = root.querySelector('[data-spectrum-path]');
  const pathNames = [
    '', '射频来话 · 本机喇叭', '射频来话 · APP／网页守听', '射频来话 · 转 P2P', '射频来话 · 转 MQTT',
    'P2P 来话 · 本机喇叭', 'P2P 来话 · APP／网页守听', 'P2P 来话 · 转射频',
    'MQTT 来话 · 本机喇叭', 'MQTT 来话 · APP／网页守听', 'MQTT 来话 · 转射频',
    '手台发话 · 本机射频', '手台发话 · P2P', '手台发话 · MQTT',
    '远程发话 · 本机射频', '远程发话 · P2P', '远程发话 · MQTT'
  ];

  const paletteStops = [
    [0, 4, 9, 25], [0.15, 15, 26, 81], [0.27, 59, 38, 164],
    [0.41, 38, 99, 233], [0.54, 21, 192, 240], [0.64, 41, 219, 164],
    [0.76, 193, 239, 73], [0.87, 255, 185, 49], [1, 252, 68, 53]
  ];
  const palette = Array.from({ length: 256 }, (_, index) => {
    const level = index / 255;
    const right = paletteStops.findIndex(stop => stop[0] >= level);
    const lower = paletteStops[Math.max(0, right - 1)];
    const upper = paletteStops[right];
    const mix = right === 0 ? 0 : (level - lower[0]) / (upper[0] - lower[0]);
    return [1, 2, 3].map(i => Math.round(lower[i] + mix * (upper[i] - lower[i])));
  });
  const rgb = value => 'rgb(' + palette[value].join(',') + ')';
  root.style.setProperty('--scope-ramp', 'linear-gradient(90deg, ' +
    paletteStops.map(([level, r, g, b]) =>
      'rgb(' + r + ',' + g + ',' + b + ') ' + (level * 100) + '%'
    ).join(', ') + ')');

  const layer = root.querySelector('[data-spectrum-gl]');
  const history = Array(ringSize).fill(null);
  const occupied = Array(historyCount).fill(null);
  let active = 'surface';
  let columns = 0;
  let mesh = null;
  let head = 0;
  let phase = 0;
  let slotNow = 0;
  const bitmap = document.createElement('canvas');
  const bitmapContext = bitmap.getContext('2d');
  const textColor = '#b3c4dd';
  const gridColor = 'rgba(151, 175, 219, 0.23)';
  bitmap.height = ringSize;
  let heatRow;

  const slot = step => ((-step % ringSize) + ringSize) % ringSize;

  function prepareRow(row, index) {
    if (columns === bandCount) row.pixels = row.bins;
    else {
      const pixels = new Uint8Array(columns);
      for (let col = 0; col < columns; ++col) {
        const start = Math.floor(col * bandCount / columns);
        const end = Math.floor((col + 1) * bandCount / columns);
        let peak = 0;
        for (let bin = start; bin < end; ++bin) peak = Math.max(peak, row.bins[bin]);
        pixels[col] = peak;
      }
      row.pixels = pixels;
    }
    row.shades = [];
    const shadeSize = Math.ceil(columns / 24);
    for (let x = 0; x < columns; x += shadeSize) {
      let peak = 0;
      for (let i = x; i < Math.min(columns, x + shadeSize); ++i) peak = Math.max(peak, row.pixels[i]);
      row.shades.push([x / (columns - 1), rgb(peak)]);
    }
    for (let x = 0; x < columns; ++x) {
      const [r, g, b] = palette[row.pixels[x]];
      const offset = 4 * x;
      heatRow.data[offset] = r;
      heatRow.data[offset + 1] = g;
      heatRow.data[offset + 2] = b;
      heatRow.data[offset + 3] = 255;
    }
    bitmapContext.putImageData(heatRow, 0, index);
    row.prepared = true;
  }

  let lastRow = null;

  function addRow(bins, at, sampleEnd, stream) {
    let step = Math.round(at / rowMs);
    if (lastRow?.stream === stream) {
      if (sampleEnd <= lastRow.sampleEnd) return false;
      step = Math.max(step, lastRow.step + Math.ceil((sampleEnd - lastRow.sampleEnd) / hop));
    }
    const index = slot(step);
    if (history[index]?.step >= step || (lastRow && step <= lastRow.step)) return false;
    let peak = 0;
    for (const value of bins) peak = Math.max(peak, value);
    const row = {bins, step, at: step * rowMs, sampleEnd, stream, peak, index};
    history[index] = row;
    lastRow = row;
    return true;
  }

  function prepare(now, count) {
    if (count !== columns) {
      columns = count;
      bitmap.width = columns;
      heatRow = bitmapContext.createImageData(columns, 1);
      for (const row of history) { if (row) row.prepared = false; }
    }
    slotNow = Math.floor(now / rowMs);
    head = slot(slotNow);
    phase = now / rowMs - slotNow;
    for (let age = 0; age < historyCount; ++age) {
      const row = history[(head + age) % ringSize];
      occupied[age] = row?.step === slotNow - age ? row : null;
      if (occupied[age] && !row.prepared) prepareRow(row, row.index);
    }
  }

  const historyAge = row => (row + phase) / (historyCount - 1);

  function surfaceRows(recent, spacing) {
    const spans = [];
    const ridges = [];
    for (let age = 0; age < historyCount;) {
      if (!occupied[age]) { ++age; continue; }
      const start = age++;
      while (occupied[age] && occupied[age].stream === occupied[age - 1].stream &&
          occupied[age].sampleEnd === occupied[age - 1].sampleEnd - hop) ++age;
      const stop = age;
      const selected = [];
      for (let first = start; first < stop;) {
        const key = Math.floor(occupied[first].sampleEnd / hop / spacing);
        let end = first + 1;
        let peak = first;
        while (end < stop && Math.floor(occupied[end].sampleEnd / hop / spacing) === key) {
          if (occupied[end].peak > occupied[peak].peak) peak = end;
          ++end;
        }
        // Choose by absolute sample sequence. All rows of a near group leave
        // the near region together, so a crest doesn't swap at every tick.
        for (let i = first; i < end; ++i) {
          if (first < recent || stop - start <= 6 || i === peak || i === start || i === stop - 1) {
            selected.push(i);
          }
        }
        first = end;
      }
      if (selected.length === 1) ridges.push(selected[0]);
      for (let i = 1; i < selected.length; ++i) spans.push([selected[i - 1], selected[i]]);
    }
    return {spans, ridges};
  }

  layer.addEventListener('webglcontextlost', event => {
    event.preventDefault();
    mesh = false;
    ++stats.contextLosses;
    redraw();
  });
  layer.addEventListener('webglcontextrestored', () => {
    mesh = null;
    redraw();
  });

  function begin(now) {
    const width = canvas.clientWidth;
    const height = canvas.clientHeight;
    if (!ctx || width < 130 || height < 70) return null;
    const scale = Math.min(2, devicePixelRatio || 1);
    const w = Math.round(width * scale);
    const h = Math.round(height * scale);
    if (frame.width !== w || frame.height !== h) {
      frame.width = w;
      frame.height = h;
    }
    ctx.setTransform(scale, 0, 0, scale, 0, 0);
    ctx.clearRect(0, 0, width, height);
    ctx.font = '11px system-ui, "Microsoft YaHei UI", sans-serif';
    ctx.textBaseline = 'middle';
    ctx.lineWidth = 1;
    const left = 42;
    const span = width - 91;
    prepare(now, Math.min(bandCount, Math.max(2, Math.floor(span * scale))));
    return { width, height, left, span, front: height - 30, depth: (height - 40) * 0.52,
      amplitude: (height - 40) * 0.44 };
  }

  function segment(x0, y0, x1, y1) {
    ctx.beginPath();
    ctx.moveTo(x0, y0);
    ctx.lineTo(x1, y1);
    ctx.stroke();
  }

  function makeMesh() {
    const gl = layer.getContext('webgl2', { alpha: true, antialias: false, preserveDrawingBuffer: true });
    if (!gl) return null;
    const info = gl.getExtension('WEBGL_debug_renderer_info');
    const software = info && /SwiftShader|llvmpipe|software/i.test(gl.getParameter(info.UNMASKED_RENDERER_WEBGL));
    const recent = software ? 12 : 50;
    const spacing = software ? 16 : 4;
    const vertex = [
      '#version 300 es',
      'precision highp float;',
      'uniform sampler2D levels;',
      'uniform highp usampler2D pairs;',
      'uniform int bins;',
      'uniform int rows;',
      'uniform int head;',
      'uniform int slots;',
      'uniform float phase;',
      'uniform vec2 viewSize;',
      'uniform vec4 projection;',
      'uniform float amplitude;',
      'out float level;',
      'out vec3 surface;',
      'void main() {',
      '  uvec2 pair = texelFetch(pairs, ivec2(gl_InstanceID, 0), 0).rg;',
      '  int slot = int(gl_VertexID % 2 == 0 ? pair.x : pair.y);',
      '  int bin = gl_VertexID / 2;',
      '  level = texelFetch(levels, ivec2(bin, slot), 0).r;',
      '  float f = float(bin) / float(bins - 1);',
      '  float t = (float((slot - head + slots) % slots) + phase) / float(rows - 1);',
      '  float x = projection.x + projection.y * (f * (1.0 - 0.28 * t) + 0.15 * t);',
      '  float y = projection.z - projection.w * t - amplitude * level;',
      '  gl_Position = vec4(x * 2.0 / viewSize.x - 1.0, 1.0 - y * 2.0 / viewSize.y,',
      '                     1.45 * t - 0.67 - 0.23 * level, 1.0);',
      '  surface = vec3(f, t * 0.70, level * 0.34);',
      '}'
    ].join('\n');
    const fragment = [
      '#version 300 es',
      'precision highp float;',
      'uniform sampler2D colorMap;',
      'in float level;',
      'in vec3 surface;',
      'out vec4 pixel;',
      'void main() {',
      '  vec3 normal = cross(dFdx(surface), dFdy(surface));',
      '  float contrast = abs(dot(normal / max(length(normal), 0.000001),',
      '                           normalize(vec3(-0.26, 0.52, 0.81))));',
      '  float light = 0.70 + 0.30 * contrast;',
      '  vec3 base = texture(colorMap, vec2(level, 0.5)).rgb;',
      '  pixel = vec4(base * light, 1.0);',
      '}'
    ].join('\n');

    function compile(type, source) {
      const shader = gl.createShader(type);
      gl.shaderSource(shader, source);
      gl.compileShader(shader);
      if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
        throw new Error(gl.getShaderInfoLog(shader));
      }
      return shader;
    }
    const program = gl.createProgram();
    gl.attachShader(program, compile(gl.VERTEX_SHADER, vertex));
    gl.attachShader(program, compile(gl.FRAGMENT_SHADER, fragment));
    gl.linkProgram(program);
    if (!gl.getProgramParameter(program, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(program));
    gl.useProgram(program);
    gl.bindVertexArray(gl.createVertexArray());
    const uniforms = Object.fromEntries(['viewSize','projection','amplitude','bins','rows','head','slots','phase','levels','colorMap','pairs']
      .map(name => [name, gl.getUniformLocation(program, name)]));
    gl.uniform1i(uniforms.levels, 0);
    gl.uniform1i(uniforms.colorMap, 1);
    gl.uniform1i(uniforms.pairs, 2);
    gl.uniform1i(uniforms.rows, historyCount);
    gl.uniform1i(uniforms.slots, ringSize);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
    const data = gl.createTexture();
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, data);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.activeTexture(gl.TEXTURE1);
    gl.bindTexture(gl.TEXTURE_2D, gl.createTexture());
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGB8, 256, 1, 0, gl.RGB, gl.UNSIGNED_BYTE, Uint8Array.from(palette.flat()));
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.enable(gl.DEPTH_TEST);
    const pairs = gl.createTexture();
    gl.activeTexture(gl.TEXTURE2);
    gl.bindTexture(gl.TEXTURE_2D, pairs);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.texStorage2D(gl.TEXTURE_2D, 1, gl.RG16UI, historyCount - 1, 1);

    const target = gl.createFramebuffer();
    const color = gl.createRenderbuffer();
    const depth = gl.createRenderbuffer();
    let lastWidth = 0;
    let targetWidth = 0;
    let targetHeight = 0;
    let spanCount = 0;
    const pairData = new Uint16Array((historyCount - 1) * 2);
    const uploaded = Array(ringSize).fill(null);

    return {
      draw(plot, spans) {
        if (gl.isContextLost()) return false;
        gl.bindFramebuffer(gl.FRAMEBUFFER, target);
        if (frame.width !== targetWidth || frame.height !== targetHeight) {
          targetWidth = frame.width;
          targetHeight = frame.height;
          gl.bindRenderbuffer(gl.RENDERBUFFER, color);
          gl.renderbufferStorage(gl.RENDERBUFFER, gl.RGBA8, targetWidth, targetHeight);
          gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.RENDERBUFFER, color);
          gl.bindRenderbuffer(gl.RENDERBUFFER, depth);
          gl.renderbufferStorage(gl.RENDERBUFFER, gl.DEPTH_COMPONENT16, targetWidth, targetHeight);
          gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.DEPTH_ATTACHMENT, gl.RENDERBUFFER, depth);
          if (gl.checkFramebufferStatus(gl.FRAMEBUFFER) !== gl.FRAMEBUFFER_COMPLETE) {
            throw new Error('spectrum framebuffer incomplete');
          }
          gl.viewport(0, 0, targetWidth, targetHeight);
        }
        gl.activeTexture(gl.TEXTURE0);
        if (columns !== lastWidth) {
          gl.uniform1i(uniforms.bins, columns);
          lastWidth = columns;
          uploaded.fill(null);
          gl.texImage2D(gl.TEXTURE_2D, 0, gl.R8, columns, ringSize, 0, gl.RED, gl.UNSIGNED_BYTE, null);
        }
        let changed = spanCount !== spans.length;
        spanCount = spans.length;
        for (let i = 0; i < spanCount; ++i) {
          for (let j = 0; j < 2; ++j) {
            const row = occupied[spans[i][j]];
            const index = row.index;
            if (uploaded[index] !== row) {
              gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, index, columns, 1, gl.RED, gl.UNSIGNED_BYTE, row.pixels);
              uploaded[index] = row;
            }
            const pairIndex = 2 * i + j;
            changed ||= pairData[pairIndex] !== index;
            pairData[pairIndex] = index;
          }
        }
        if (changed && spanCount) {
          gl.activeTexture(gl.TEXTURE2);
          gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, spanCount, 1,
            gl.RG_INTEGER, gl.UNSIGNED_SHORT, pairData.subarray(0, 2 * spanCount));
        }
        gl.uniform1i(uniforms.head, head);
        gl.uniform1f(uniforms.phase, phase);
        gl.uniform2f(uniforms.viewSize, plot.width, plot.height);
        gl.uniform4f(uniforms.projection, plot.left, plot.span, plot.front, plot.depth);
        gl.uniform1f(uniforms.amplitude, plot.amplitude);
        gl.clearColor(0, 0, 0, 0);
        gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
        gl.drawArraysInstanced(gl.TRIANGLE_STRIP, 0, columns * 2, spanCount);
        return !gl.isContextLost();
      },
      present() {
        if (layer.width !== targetWidth || layer.height !== targetHeight) {
          layer.width = targetWidth;
          layer.height = targetHeight;
        }
        gl.bindFramebuffer(gl.READ_FRAMEBUFFER, target);
        gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER, null);
        gl.blitFramebuffer(0, 0, targetWidth, targetHeight, 0, 0, targetWidth, targetHeight,
          gl.COLOR_BUFFER_BIT, gl.NEAREST);
      },
      select() { return surfaceRows(recent, spacing); }
    };
  }

  function project(plot, fraction, age, level = 0) {
    return [
      plot.left + plot.span * (fraction * (1 - 0.28 * age) + 0.15 * age),
      plot.front - plot.depth * age - plot.amplitude * level
    ];
  }

  function traceRow(plot, row) {
    const age = historyAge(row);
    const data = occupied[row].pixels;
    const left = plot.left + plot.span * 0.15 * age;
    const dx = plot.span * (1 - 0.28 * age) / (columns - 1);
    const y = plot.front - plot.depth * age;
    const db = plot.amplitude / 255;
    ctx.moveTo(left, y - db * data[0]);
    for (let x = 1; x < columns; ++x) ctx.lineTo(left + x * dx, y - db * data[x]);
  }

  function drawRidges(plot, selected = null) {
    for (let row = historyCount - 1; row >= 0; row--) {
      const item = occupied[row];
      if (!item || (selected ? !selected.has(row) :
          row >= 10 && Math.floor(item.sampleEnd / hop) % 9 &&
          occupied[row - 1]?.stream === item.stream && occupied[row + 1]?.stream === item.stream)) continue;
      const age = historyAge(row);
      const [left] = project(plot, 0, age);
      const [right] = project(plot, 1, age);
      const shade = ctx.createLinearGradient(left, 0, right, 0);
      for (const [x, color] of item.shades) shade.addColorStop(x, color);
      ctx.beginPath();
      traceRow(plot, row);
      ctx.lineWidth = 0.85;
      ctx.strokeStyle = shade;
      ctx.stroke();
    }
  }

  function labels(plot, y, top) {
    const ticks = plot.width < 390
      ? [[125, '125 Hz'], [4000, '4 kHz'], [8000, '8 kHz']]
      : [[125, '125 Hz'], [1000, '1 kHz'], [2000, '2 kHz'], [3000, '3 kHz'],
        [4000, '4 kHz'], [5000, '5 kHz'], [6000, '6 kHz'], [7000, '7 kHz'], [8000, '8 kHz']];
    ctx.strokeStyle = gridColor;
    ctx.fillStyle = textColor;
    ctx.textAlign = 'center';
    ctx.lineWidth = 1;
    for (const [frequency, label] of ticks) {
      const x = plot.left + plot.span * (frequency - 125) / (8000 - 125);
      segment(x, top ?? y, x, y + 4);
      ctx.fillText(label, x, y + 15);
    }
  }

  function timeLines(draw) {
    const offset = ((playhead % 500) + 500) % 500;
    ctx.strokeStyle = 'rgba(117, 166, 200, 0.21)';
    ctx.lineWidth = 1;
    for (let ms = offset; ms < historyCount * rowMs; ms += 500) {
      const age = ms / (historyCount * rowMs);
      draw(age, !!occupied[Math.floor(ms / rowMs)]);
    }
  }

  function drawSurface(plot) {
    const front = occupied.findIndex(Boolean);
    if (front >= 0 && mesh === null) {
      try { mesh = makeMesh() || false; }
      catch { mesh = false; }
    }
    let prepared = false;
    let geometry = null;
    if (front >= 0 && mesh) {
      geometry = mesh.select();
      if (geometry.spans.length) {
        try { prepared = mesh.draw(plot, geometry.spans); }
        catch (error) { mesh = false; throw error; }
      }
    }

    const corners = [project(plot, 0, 0), project(plot, 1, 0), project(plot, 1, 1), project(plot, 0, 1)];
    ctx.beginPath();
    ctx.moveTo(...corners[0]);
    for (const corner of corners.slice(1)) ctx.lineTo(...corner);
    ctx.closePath();
    if (!prepared) {
      ctx.fillStyle = '#101f3b';
      ctx.fill();
    }
    ctx.strokeStyle = gridColor;
    ctx.stroke();
    timeLines((age, hasData) => {
      const left = project(plot, 0, age);
      const right = project(plot, 1, age);
      if (!hasData) segment(...left, ...right);
      segment(...right, right[0] + 3, right[1]);
    });
    if (front >= 0) drawRidges(plot, prepared ? new Set(geometry.ridges) : null);

    if (front >= 0 && front < 3) {
      ctx.beginPath();
      traceRow(plot, front);
      ctx.strokeStyle = 'rgba(195, 236, 255, 0.86)';
      ctx.lineWidth = 0.85;
      ctx.stroke();
    }
    ctx.textAlign = 'right';
    ctx.fillStyle = textColor;
    ctx.strokeStyle = gridColor;
    segment(plot.left, plot.front, plot.left, plot.front - plot.amplitude);
    for (const db of [minimumDb, -64, -48, -32, maximumDb]) {
      const y = plot.front - plot.amplitude * (db - minimumDb) / (maximumDb - minimumDb);
      segment(plot.left - 3, y, plot.left, y);
      ctx.fillText(String(db).replace('-', '−'), plot.left - 6, y);
    }
    ctx.fillText('dBFS', plot.left - 2, plot.front - plot.amplitude - 15);
    ctx.textAlign = 'left';
    for (const [age, label] of [[0, '0 s'], [0.5, '−5 s'], [1, '−10 s']]) {
      const [x, y] = project(plot, 1, age);
      ctx.fillText(label, x + 7, y);
    }
    labels(plot, plot.front);
    return {kind: prepared ? 'mesh' : front < 0 ? 'idle' : 'ridges', prepared};
  }

  function drawWaterfall(plot) {
    const top = 16;
    const depth = plot.front - top;
    ctx.imageSmoothingEnabled = false;
    const height = depth / historyCount;
    ctx.save();
    ctx.beginPath();
    ctx.rect(plot.left, top, plot.span, depth);
    ctx.clip();
    timeLines(age => segment(plot.left, top + age * depth, plot.left + plot.span, top + age * depth));
    for (let age = 0; age < historyCount;) {
      if (!occupied[age]) { ++age; continue; }
      const start = age;
      const index = (head + age) % ringSize;
      const end = age + ringSize - index;
      while (age < historyCount && age < end && occupied[age]) ++age;
      ctx.drawImage(bitmap, 0, index, columns, age - start,
        plot.left, top + (start + phase) * height, plot.span, (age - start) * height);
    }
    ctx.restore();
    timeLines(age => segment(plot.left + plot.span, top + age * depth,
      plot.left + plot.span + 3, top + age * depth));
    ctx.strokeStyle = gridColor;
    ctx.strokeRect(plot.left, top, plot.span, depth);
    ctx.fillStyle = textColor;
    ctx.textAlign = 'left';
    for (const [age, label] of [[0, '0 s'], [0.5, '−5 s'], [1, '−10 s']]) {
      ctx.fillText(label, plot.left + plot.span + 7, top + depth * age);
    }
    labels(plot, plot.front, top);
    return {kind:'waterfall', prepared:false};
  }

  function render(now) {
    const plot = begin(now);
    if (!plot) return false;
    const {kind, prepared} = active === 'surface' ? drawSurface(plot) : drawWaterfall(plot);
    if (prepared) mesh.present();
    if (canvas.width !== frame.width || canvas.height !== frame.height) {
      canvas.width = frame.width;
      canvas.height = frame.height;
    }
    output.globalCompositeOperation = 'copy';
    output.drawImage(frame, 0, 0);
    layer.hidden = !prepared;
    root.dataset.rendering = kind;
    return true;
  }

  for (const button of buttons) {
    button.addEventListener('click', () => {
      active = button.dataset.view;
      for (const other of buttons) other.setAttribute('aria-pressed', String(other === button));
      canvas.setAttribute('aria-label', active === 'surface'
        ? '用 2048 点 FFT 绘制的 3D 频谱。横轴为频率，峰高为电平，时间向后延伸。'
        : '用 2048 点 FFT 绘制的彩色瀑布。横轴为频率，时间向下，颜色表示电平。');
      redraw();
    });
  }

  let onScreen = !('IntersectionObserver' in window);
  let worker = null;
  let watching = false;
  let mode = 'radio';
  let epoch = 0;
  let available = -1;
  let selection = null;
  let animation = 0;
  let idleTimer = 0;
  let lastDraw = 0;
  let playhead = -Infinity;
  let boardBoot = null;
  let boardPath = 0;
  let lead = 185;
  let forceDraw = true;
  let retries = 0;
  let workerRetry = 0;
  const errorLabel = root.querySelector('[data-spectrum-problem]');
  let streamProblem = '';
  let drawProblem = false;
  const stats = {samples:0, rows:0, drawn:0, idleDrawn:0, gaps:0, contention:0, fftMs:0, rtt:0, sourceLag:0,
    networkErrors:0, busyErrors:0, formatErrors:0, drawErrors:0, contextLosses:0, workerErrors:0};

  function visible() {
    return onScreen && !document.hidden && root.getClientRects().length > 0;
  }

  function updateProblem() {
    const message = streamProblem || (drawProblem ? '频谱绘制异常' : '');
    errorLabel.textContent = message;
    errorLabel.hidden = !message;
  }

  function source(mask, path, message) {
    if (mask !== available) {
      available = mask;
      const options = [];
      for (let p = 1; p < pathNames.length; ++p) {
        if (mask & (1 << p)) options.push(new Option(pathNames[p], String(p)));
      }
      pathSelect.replaceChildren(new Option('自动跟随', '0'), ...options);
      pathSelect.hidden = options.length < 2;
    }
    if (document.activeElement !== pathSelect) pathSelect.value = String(path || 0);
    sourceLabel.hidden = !pathSelect.hidden;
    if (!sourceLabel.hidden) sourceLabel.textContent = message || pathNames[path] || '等待语音';
  }

  function paint() {
    animation = 0;
    if (!visible()) return;
    const now = performance.now();
    const live = lastRow && now - lastRow.at < lead + 350;
    const interval = canvas.clientWidth < 600 || mesh === false ? 32 : 15;
    if (forceDraw || now - lastDraw >= (live ? interval : 44)) {
      forceDraw = false;
      lastDraw = now;
      playhead = Math.max(playhead, now - lead);
      try {
        if (render(playhead)) {
          ++stats.drawn;
          if (!live) ++stats.idleDrawn;
          if (drawProblem) { drawProblem = false; updateProblem(); }
        }
      } catch (error) {
        if (!drawProblem) console.warn('频谱绘制异常', error);
        drawProblem = true;
        ++stats.drawErrors;
        updateProblem();
      }
    }
    if (live) animation = requestAnimationFrame(paint);
    else idleTimer = setTimeout(() => {
      idleTimer = 0;
      redraw(false);
    }, Math.max(1, 45 - (performance.now() - lastDraw)));
  }

  function redraw(force = true) {
    forceDraw ||= force;
    clearTimeout(idleTimer);
    idleTimer = 0;
    if (!animation && visible()) animation = requestAnimationFrame(paint);
  }

  function clearHistory() {
    history.fill(null);
    lastRow = null;
    redraw();
  }

  function received({data}) {
    if (data.epoch !== epoch || !watching || !visible()) return;
    retries = 0;
    if (data.problem) {
      ++stats[data.problem + 'Errors'];
      streamProblem = {network:'频谱取流超时', busy:'板端频谱暂忙', format:'频谱帧格式异常'}[data.problem];
      updateProblem();
      return;
    }
    if (data.boot !== undefined) {
      if ((boardBoot !== null && boardBoot !== data.boot) || (boardPath && data.path && boardPath !== data.path)) {
        clearHistory();
        boardPath = 0;
      }
      boardBoot = data.boot;
      if (data.path) boardPath = data.path;
      source(data.mask, data.mask ? data.path : 0, data.mask ? '' : '守听中 · 暂无语音');
    }
    if (data.rtt !== undefined) {
      stats.rtt = data.rtt;
      stats.contention = data.contention;
      stats.sourceLag = data.sourceLag;
      if (data.rows.length) {
        stats.fftMs = data.fftMs;
        // Move the playout delay gradually so normal RTT jitter does not shift the time grid.
        const target = Math.max(155, Math.min(320, 119 + data.rtt * 0.8));
        lead += Math.max(-2, Math.min(4, target - lead));
      }
    }
    if (data.gap) ++stats.gaps;
    if (data.samples) stats.samples += data.samples;
    if (streamProblem) { streamProblem = ''; updateProblem(); }
    const stream = data.boot === undefined ? 'microphone' : [data.boot, data.path, data.generation].join('/');
    for (const row of data.rows) {
      const at = row.at - performance.timeOrigin;
      if (addRow(new Uint8Array(row.buffer), at, row.sampleEnd, stream)) {
        ++stats.rows;
      }
    }
    if (data.rows.length) redraw(false);
  }

  function ensureWorker() {
    if (worker) return true;
    try {
      worker = new Worker('/audio-spectrum.js');
      worker.onmessage = received;
      worker.onerror = () => {
        worker.terminate();
        worker = null;
        watching = false;
        ++stats.workerErrors;
        streamProblem = '频谱计算异常';
        updateProblem();
        if (visible() && retries++ < 3) workerRetry = setTimeout(sync, 400);
      };
      return true;
    } catch {
      ++stats.workerErrors;
      streamProblem = '浏览器无法启动频谱计算';
      updateProblem();
      return false;
    }
  }

  function stop() {
    clearTimeout(workerRetry);
    if (!watching) return;
    watching = false;
    worker?.postMessage({type:'stop', epoch:++epoch});
  }

  function sync() {
    if (!visible()) {
      stop();
      clearTimeout(idleTimer);
      cancelAnimationFrame(animation);
      idleTimer = animation = 0;
      return;
    }
    if (mode === 'releasing') { stop(); redraw(); return; }
    if (!watching && ensureWorker()) {
      watching = true;
      worker.postMessage({type: mode, epoch: ++epoch, selection});
    }
    redraw();
  }

  window.ORLAudioSpectrum = {
    microphone(pcm, first, stamp) {
      if (mode !== 'microphone' || !watching || !visible()) return;
      const buffer = pcm.buffer;
      worker.postMessage({type:'pcm', epoch, buffer, first, at:performance.timeOrigin + stamp}, [buffer]);
    },
    microphoneReady() { return mode === 'microphone' && watching && visible(); },
    diagnostics() { return {...stats}; },
    beginMicrophone() {
      stop();
      mode = 'microphone';
      clearHistory();
      source(0, 0, '浏览器麦克风预览');
      sync();
    },
    releaseMicrophone(confirm) {
      if (mode === 'radio') return;
      stop();
      mode = 'releasing';
      const token = ++epoch;
      source(0, 0, '正在确认松键');
      void Promise.resolve(confirm).then(ok => {
        if (epoch !== token) return;
        if (!ok) { source(0, 0, '等待松键确认'); return; }
        clearHistory();
        mode = 'radio';
        source(0, 0);
        sync();
      });
    }
  };

  pathSelect.onchange = () => {
    selection = pathSelect.value;
    stop();
    sync();
  };

  document.addEventListener('visibilitychange', sync);
  if ('IntersectionObserver' in window) {
    new IntersectionObserver(([entry]) => {
      onScreen = entry.isIntersecting;
      sync();
    }).observe(root);
  }
  if ('ResizeObserver' in window) new ResizeObserver(sync).observe(canvas);
  else window.addEventListener('resize', sync);
  sync();
})();
