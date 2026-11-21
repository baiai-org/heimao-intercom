(() => {
  'use strict';
  const sampleRate = 16000;
  const fftSize = 2048;
  const hop = 2000;
  const rowMs = 1000 * hop / sampleRate;
  const firstBin = 16;
  const bandCount = fftSize / 2 - firstBin + 1;
  const historyCount = 80;
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
    self.onmessage = ({data}) => {
      const result = spectrum(new Int16Array(data.buffer));
      self.postMessage({epoch: data.epoch, at: data.at, buffer: result.buffer}, [result.buffer]);
    };
    return;
  }

  const root = document.getElementById('audioSpectrum');
  if (!root) return;
  const canvas = root.querySelector('canvas');
  const ctx = canvas.getContext('2d');
  if (!ctx) return;
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

  const history = [];
  const occupied = Array(historyCount).fill(false);
  let active = 'surface';
  let columns = 0;
  let framePixels = null;
  let heatImage = null;
  let mesh = null;
  const bitmap = document.createElement('canvas');
  const bitmapContext = bitmap.getContext('2d');
  const textColor = '#b3c4dd';
  const gridColor = 'rgba(151, 175, 219, 0.23)';

  function collectForScreen(count) {
    if (count !== columns) {
      columns = count;
      framePixels = new Uint8Array(columns * historyCount);
      bitmap.width = columns;
      bitmap.height = historyCount;
      heatImage = bitmapContext.createImageData(columns, historyCount);
    }
    framePixels.fill(0);
    occupied.fill(false);
    const now = performance.now();
    for (const frame of history) {
      const row = Math.max(0, Math.floor((now - frame.at) / rowMs));
      if (row >= historyCount) continue;
      occupied[row] = true;
      const source = frame.bins;
      const target = row * columns;
      if (columns === bandCount) {
        framePixels.set(source, target);
        continue;
      }
      for (let col = 0; col < columns; col++) {
        const start = Math.floor(col * bandCount / columns);
        const end = Math.floor((col + 1) * bandCount / columns);
        let highest = source[start];
        for (let bin = start + 1; bin < end; bin++) highest = Math.max(highest, source[bin]);
        framePixels[target + col] = highest;
      }
    }
  }

  function begin() {
    const width = canvas.clientWidth;
    const height = canvas.clientHeight;
    if (!ctx || width < 130 || height < 70) return null;
    const scale = Math.min(2, devicePixelRatio || 1);
    const w = Math.round(width * scale);
    const h = Math.round(height * scale);
    if (canvas.width !== w || canvas.height !== h) {
      canvas.width = w;
      canvas.height = h;
    }
    ctx.setTransform(scale, 0, 0, scale, 0, 0);
    ctx.clearRect(0, 0, width, height);
    ctx.font = '11px system-ui, "Microsoft YaHei UI", sans-serif';
    ctx.textBaseline = 'middle';
    ctx.lineWidth = 1;
    const left = 42;
    const span = width - 91;
    collectForScreen(Math.min(bandCount, Math.max(2, Math.floor(span * scale))));
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
    const layer = document.createElement('canvas');
    const gl = layer.getContext('webgl2', { alpha: true, antialias: true, preserveDrawingBuffer: true });
    if (!gl) return null;
    const vertex = [
      '#version 300 es',
      'precision highp float;',
      'uniform sampler2D levels;',
      'uniform int bins;',
      'uniform int rows;',
      'uniform vec2 viewSize;',
      'uniform vec4 projection;',
      'uniform float amplitude;',
      'out float level;',
      'out vec3 surface;',
      'void main() {',
      '  ivec2 cell = ivec2(gl_VertexID % bins, gl_VertexID / bins);',
      '  level = texelFetch(levels, cell, 0).r;',
      '  float f = float(cell.x) / float(bins - 1);',
      '  float t = float(cell.y) / float(rows - 1);',
      '  float x = projection.x + projection.y * (f * (1.0 - 0.28 * t) + 0.15 * t);',
      '  float y = projection.z - projection.w * t - amplitude * level;',
      '  gl_Position = vec4(x * 2.0 / viewSize.x - 1.0, 1.0 - y * 2.0 / viewSize.y,',
      '                     1.40 * t - 0.80 - 0.25 * level, 1.0);',
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
    const indexBuffer = gl.createBuffer();
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, indexBuffer);
    const uniforms = Object.fromEntries(['viewSize','projection','amplitude','bins','rows','levels','colorMap']
      .map(name => [name, gl.getUniformLocation(program, name)]));
    gl.uniform1i(uniforms.levels, 0);
    gl.uniform1i(uniforms.colorMap, 1);
    gl.uniform1i(uniforms.rows, historyCount);
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
    let lastWidth = 0;

    return {
      draw(plot) {
        if (gl.isContextLost()) return false;
        if (layer.width !== canvas.width || layer.height !== canvas.height) {
          layer.width = canvas.width;
          layer.height = canvas.height;
          gl.viewport(0, 0, layer.width, layer.height);
        }
        if (columns !== lastWidth) {
          const indices = new Uint32Array((columns - 1) * (historyCount - 1) * 6);
          let i = 0;
          for (let row = 0; row < historyCount - 1; row++) {
            for (let x = 0; x < columns - 1; x++) {
              const p = row * columns + x;
              indices[i++] = p;
              indices[i++] = p + 1;
              indices[i++] = p + columns;
              indices[i++] = p + 1;
              indices[i++] = p + columns + 1;
              indices[i++] = p + columns;
            }
          }
          gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, indices, gl.STATIC_DRAW);
          gl.uniform1i(uniforms.bins, columns);
          lastWidth = columns;
          gl.activeTexture(gl.TEXTURE0);
          gl.texImage2D(gl.TEXTURE_2D, 0, gl.R8, columns, historyCount, 0, gl.RED, gl.UNSIGNED_BYTE, null);
        }
        gl.activeTexture(gl.TEXTURE0);
        gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, columns, historyCount, gl.RED, gl.UNSIGNED_BYTE, framePixels);
        gl.uniform2f(uniforms.viewSize, plot.width, plot.height);
        gl.uniform4f(uniforms.projection, plot.left, plot.span, plot.front, plot.depth);
        gl.uniform1f(uniforms.amplitude, plot.amplitude);
        gl.clearColor(0, 0, 0, 0);
        gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
        // Fill only adjacent real rows. Lost time slots remain empty.
        const strip = (columns - 1) * 6;
        for (let row = 0; row < historyCount - 1;) {
          if (!occupied[row] || !occupied[row + 1]) { ++row; continue; }
          const start = row++;
          while (row < historyCount - 1 && occupied[row + 1]) ++row;
          gl.drawElements(gl.TRIANGLES, (row - start) * strip, gl.UNSIGNED_INT, start * strip * 4);
        }
        ctx.drawImage(layer, 0, 0, plot.width, plot.height);
        return true;
      }
    };
  }

  function project(plot, fraction, age, level = 0) {
    return [
      plot.left + plot.span * (fraction * (1 - 0.28 * age) + 0.15 * age),
      plot.front - plot.depth * age - plot.amplitude * level
    ];
  }

  function traceRow(plot, row) {
    const age = row / (historyCount - 1);
    for (let x = 0; x < columns; x++) {
      const [px, py] = project(plot, x / (columns - 1), age, framePixels[row * columns + x] / 255);
      if (x === 0) ctx.moveTo(px, py);
      else ctx.lineTo(px, py);
    }
  }

  function drawRidges(plot, isolated) {
    for (let row = historyCount - 1; row >= 0; row--) {
      if (!occupied[row] || (isolated && (occupied[row - 1] || occupied[row + 1]))) continue;
      const age = row / (historyCount - 1);
      const [left] = project(plot, 0, age);
      const [right] = project(plot, 1, age);
      const shade = ctx.createLinearGradient(left, 0, right, 0);
      for (let x = 0; x < columns; x += 4) {
        const peak = Math.max(...framePixels.subarray(row * columns + x, row * columns + Math.min(x + 4, columns)));
        shade.addColorStop(x / (columns - 1), rgb(peak));
      }
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

  function drawSurface(plot) {
    const corners = [project(plot, 0, 0), project(plot, 1, 0), project(plot, 1, 1), project(plot, 0, 1)];
    ctx.beginPath();
    ctx.moveTo(...corners[0]);
    for (const corner of corners.slice(1)) ctx.lineTo(...corner);
    ctx.closePath();
    ctx.fillStyle = '#101f3b';
    ctx.fill();
    ctx.strokeStyle = gridColor;
    ctx.stroke();

    if (mesh === null) {
      try { mesh = makeMesh() || false; }
      catch { mesh = false; }
    }
    const rendered = mesh && mesh.draw(plot);
    drawRidges(plot, rendered);
    root.dataset.rendering = rendered ? 'mesh' : 'ridges';

    const front = occupied.indexOf(true);
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
  }

  function drawWaterfall(plot) {
    const top = 16;
    const depth = plot.front - top;
    for (let index = 0; index < framePixels.length; index++) {
      const [r, g, b] = palette[framePixels[index]];
      const offset = 4 * index;
      heatImage.data[offset] = r;
      heatImage.data[offset + 1] = g;
      heatImage.data[offset + 2] = b;
      heatImage.data[offset + 3] = 255;
    }
    bitmapContext.putImageData(heatImage, 0, 0);
    ctx.imageSmoothingEnabled = false;
    ctx.drawImage(bitmap, plot.left, top, plot.span, depth);
    ctx.strokeStyle = gridColor;
    ctx.strokeRect(plot.left, top, plot.span, depth);
    ctx.fillStyle = textColor;
    ctx.textAlign = 'left';
    for (const [age, label] of [[0, '0 s'], [0.5, '−5 s'], [1, '−10 s']]) {
      ctx.fillText(label, plot.left + plot.span + 7, top + depth * age);
    }
    labels(plot, plot.front, top);
    root.dataset.rendering = 'waterfall';
  }

  function render() {
    const plot = begin();
    if (!plot) return;
    if (active === 'surface') drawSurface(plot);
    else drawWaterfall(plot);
  }

  for (const button of buttons) {
    button.addEventListener('click', () => {
      active = button.dataset.view;
      for (const other of buttons) other.setAttribute('aria-pressed', String(other === button));
      canvas.setAttribute('aria-label', active === 'surface'
        ? '用 2048 点 FFT 绘制的 3D 频谱。横轴为频率，峰高为电平，时间向后延伸。'
        : '用 2048 点 FFT 绘制的彩色瀑布。横轴为频率，时间向下，颜色表示电平。');
      render();
    });
  }

  let onScreen = !('IntersectionObserver' in window);
  let timer = 0;
  let inflight = null;
  let after = 0;
  let generation = null;
  let available = -1;
  let requested = null;
  let mode = 'radio';
  let epoch = 0;
  let micWorker = null;
  let micPending = false;
  const micBuffer = new Int16Array(fftSize);
  let micHead = 0;
  let micCount = 0;
  let micNew = 0;

  function visible() {
    return onScreen && !document.hidden && root.getClientRects().length > 0;
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
    if (document.activeElement !== pathSelect) pathSelect.value = String(path);
    sourceLabel.hidden = !pathSelect.hidden;
    if (!sourceLabel.hidden) sourceLabel.textContent = message || pathNames[path] || '等待语音';
  }

  function addRow(bins, at) {
    history.push({bins, at});
    while (history.length > historyCount || history[0].at < performance.now() - rowMs * historyCount) {
      history.shift();
      if (!history.length) break;
    }
  }

  function acceptFrame(buffer) {
    if (buffer.byteLength < 20) throw Error('频谱帧长度无效');
    const view = new DataView(buffer);
    const count = view.getUint8(17);
    if (view.getUint32(0, true) !== 0x01505341 || view.getUint16(18, true) !== bandCount ||
        count > 2 || buffer.byteLength !== 20 + count * (8 + bandCount)) {
      throw Error('频谱帧格式无效');
    }
    const nextGeneration = view.getUint32(4, true);
    if (generation !== nextGeneration) {
      generation = nextGeneration;
      after = 0;
      history.length = 0;
    }
    const now = performance.now();
    const boardTime = view.getUint32(12, true);
    for (let offset = 20; offset < buffer.byteLength; offset += 8 + bandCount) {
      const sequence = view.getUint32(offset, true);
      const age = (boardTime - view.getUint32(offset + 4, true)) >>> 0;
      if (after && (sequence - after | 0) <= 0) continue;
      after = sequence;
      if (age < rowMs * historyCount) addRow(new Uint8Array(buffer, offset + 8, bandCount), now - age);
    }
    source(view.getUint32(8, true), view.getUint8(16));
    render();
  }

  function schedule(delay) {
    clearTimeout(timer);
    if (visible() && mode === 'radio') timer = setTimeout(() => void read(), delay);
  }

  async function read() {
    if (!visible() || mode !== 'radio' || inflight) return;
    const cursor = epoch;
    const controller = new AbortController();
    inflight = controller;
    const selected = requested;
    const query = new URLSearchParams({after});
    if (selected !== null) query.set('select', selected);
    const timeout = setTimeout(() => controller.abort(), 1600);
    let delay = 250;
    try {
      const response = await fetch('/api/audio_spectrum?' + query, {cache:'no-store', signal:controller.signal});
      if (!response.ok) throw Error('读取频谱失败');
      const frame = await response.arrayBuffer();
      if (cursor === epoch && visible() && mode === 'radio') {
        acceptFrame(frame);
        if (selected === requested) requested = null;
        if (requested !== null) delay = 0;
        else if (!available && !history.some(row => row.at >= performance.now() - rowMs * historyCount)) delay = 850;
      }
    } catch (error) {
      if (cursor === epoch && visible() && mode === 'radio') {
        source(0, 0, '频谱暂不可用');
        render();
        delay = 850;
      }
    } finally {
      clearTimeout(timeout);
      inflight = null;
      schedule(delay);
    }
  }

  function initWorker() {
    if (micWorker || typeof Worker !== 'function') return;
    try {
      micWorker = new Worker('/audio-spectrum.js');
      micWorker.onmessage = ({data}) => {
        micPending = false;
        if (data.epoch !== epoch || mode !== 'microphone' || !visible()) return;
        addRow(new Uint8Array(data.buffer), data.at);
        render();
      };
      micWorker.onerror = () => {
        micPending = false;
        micWorker.terminate();
        micWorker = null;
      };
    } catch {
      micWorker = null;
    }
  }

  const microphone = pcm => {
    if (mode !== 'microphone' || !visible() || !micWorker) return;
    if (pcm.length > fftSize) pcm = pcm.subarray(pcm.length - fftSize);
    const first = Math.min(pcm.length, fftSize - micHead);
    micBuffer.set(pcm.subarray(0, first), micHead);
    micBuffer.set(pcm.subarray(first), 0);
    micHead = (micHead + pcm.length) % fftSize;
    micCount = Math.min(fftSize, micCount + pcm.length);
    micNew = Math.min(fftSize, micNew + pcm.length);
    if (micCount < fftSize || micNew < hop || micPending) return;
    const ordered = new Int16Array(fftSize);
    ordered.set(micBuffer.subarray(micHead));
    ordered.set(micBuffer.subarray(0, micHead), fftSize - micHead);
    micNew -= hop;
    micPending = true;
    micWorker.postMessage({epoch, at:performance.now(), buffer:ordered.buffer}, [ordered.buffer]);
  };

  window.ORLAudioSpectrum = {
    microphone,
    beginMicrophone() {
      ++epoch;
      mode = 'microphone';
      clearTimeout(timer);
      inflight?.abort();
      history.length = 0;
      micHead = micCount = micNew = 0;
      source(0, 0, '浏览器麦克风预览');
      if (visible()) requestAnimationFrame(render);
    },
    releaseMicrophone(confirm) {
      if (mode === 'radio') return;
      const cursor = ++epoch;
      mode = 'releasing';
      source(0, 0, '正在确认松键');
      void Promise.resolve(confirm).then(ok => {
        if (epoch !== cursor) return;
        if (!ok) { source(0, 0, '等待松键确认'); return; }
        history.length = 0;
        after = 0;
        generation = null;
        mode = 'radio';
        source(0, 0);
        schedule(0);
      });
    }
  };

  pathSelect.onchange = () => {
    requested = pathSelect.value;
    schedule(0);
  };

  function visibilityChanged() {
    if (!visible()) {
      clearTimeout(timer);
      inflight?.abort();
      return;
    }
    if (mode === 'radio') initWorker();
    render();
    schedule(0);
  }
  document.addEventListener('visibilitychange', visibilityChanged);
  if ('IntersectionObserver' in window) {
    new IntersectionObserver(([entry]) => {
      onScreen = entry.isIntersecting;
      visibilityChanged();
    }).observe(root);
  }
  if ('ResizeObserver' in window) new ResizeObserver(visibilityChanged).observe(canvas);
  else window.addEventListener('resize', visibilityChanged);
  initWorker();
  visibilityChanged();
})();
