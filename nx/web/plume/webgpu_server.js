// reOblivion web (2026-11-18): the WebGPU server, on the page's thread (an Emscripten --pre-js).
//
// plume_webgpu.cpp (the game's threads, workers) records WebGPU work as command words in wasm memory and posts each
// block here (WebGpuServer.process). The page's thread owns the GPUDevice and the canvas; WebGPU's promises
// (adapter, device, mapAsync, onSubmittedWorkDone) resolve here while the workers block on futexes in shared memory
// (fences, the init status). Object ids are the client's (never reused); the protocol is the OP table below and
// plume_webgpu.cpp's WebGpuOp, which must match.
//
// GPL-3.0 with the rest of the port.

var WebGpuServer = (function () {
  'use strict';

  const OP = {
    CREATE_BUFFER: 1, DESTROY: 2, CREATE_TEXTURE: 3, CREATE_VIEW: 4, CREATE_SAMPLER: 5, CREATE_SHADER: 6,
    CREATE_BGL: 7, CREATE_PIPELINE_LAYOUT: 8, CREATE_RENDER_PIPELINE: 9, CREATE_COMPUTE_PIPELINE: 10,
    CREATE_BIND_GROUP: 11, WRITE_BUFFER: 12, SUBMIT: 13, PRESENT: 14, CREATE_FRAMEBUFFER: 15, WRITE_TEXTURE: 16,
    SIGNAL: 17, CONFIGURE: 18, LOG: 19,
    // command list ops (inside SUBMIT)
    SET_FRAMEBUFFER: 100, CLEAR_COLOR: 101, CLEAR_DS: 102, SET_PIPELINE: 103, SET_BIND_GROUP: 104,
    SET_VERTEX_BUFFER: 105, SET_INDEX_BUFFER: 106, SET_VIEWPORT: 107, SET_SCISSOR: 108, SET_BLEND_CONSTANT: 109,
    SET_STENCIL_REF: 110, DRAW: 111, DRAW_INDEXED: 112, DISPATCH: 113, COPY_B2B: 114, COPY_B2T: 115, COPY_T2B: 116,
    COPY_T2T: 117, RESOLVE: 118, BARRIER: 119, SET_COMPUTE_PIPELINE: 120, SET_COMPUTE_BIND_GROUP: 121, SET_PUSH: 122,
    SET_COMPUTE_PUSH: 123,
  };

  // plume::RenderFormat -> GPUTextureFormat (null: none in WebGPU).
  const TEX_FORMAT = [
    null, 'rgba32float', 'rgba32float', 'rgba32uint', 'rgba32sint', null, null, null, null,
    'rgba16float', 'rgba16float', 'rgba16unorm', 'rgba16uint', 'rgba16snorm', 'rgba16sint',
    'rg32float', 'rg32float', 'rg32uint', 'rg32sint',
    'rgba8unorm', 'rgba8unorm', 'rgba8uint', 'rgba8snorm', 'rgba8sint', 'bgra8unorm',
    'rg16float', 'rg16float', 'rg16unorm', 'rg16uint', 'rg16snorm', 'rg16sint',
    'r32float', 'depth32float', 'depth32float-stencil8', 'r32float', 'r32uint', 'r32sint',
    'rg8unorm', 'rg8unorm', 'rg8uint', 'rg8snorm', 'rg8sint',
    'r16float', 'r16float', 'depth16unorm', 'r16unorm', 'r16uint', 'r16snorm', 'r16sint',
    'r8unorm', 'r8unorm', 'r8uint', 'r8snorm', 'r8sint',
    'bc1-rgba-unorm', 'bc1-rgba-unorm', 'bc1-rgba-unorm-srgb', 'bc2-rgba-unorm', 'bc2-rgba-unorm', 'bc2-rgba-unorm-srgb',
    'bc3-rgba-unorm', 'bc3-rgba-unorm', 'bc3-rgba-unorm-srgb', 'bc4-r-unorm', 'bc4-r-unorm', 'bc4-r-snorm',
    'bc5-rg-unorm', 'bc5-rg-unorm', 'bc5-rg-snorm', 'bc6h-rgb-ufloat', 'bc6h-rgb-ufloat', 'bc6h-rgb-float',
    'bc7-rgba-unorm', 'bc7-rgba-unorm', 'bc7-rgba-unorm-srgb', 'stencil8', 'depth24plus-stencil8',
  ];
  // plume::RenderFormat -> GPUVertexFormat.
  const VTX_FORMAT = {
    2: 'float32x4', 3: 'uint32x4', 4: 'sint32x4', 6: 'float32x3', 7: 'uint32x3', 8: 'sint32x3',
    10: 'float16x4', 11: 'unorm16x4', 12: 'uint16x4', 13: 'snorm16x4', 14: 'sint16x4',
    16: 'float32x2', 17: 'uint32x2', 18: 'sint32x2', 20: 'unorm8x4', 21: 'uint8x4', 22: 'snorm8x4', 23: 'sint8x4',
    24: 'unorm8x4-bgra', 26: 'float16x2', 27: 'unorm16x2', 28: 'uint16x2', 29: 'snorm16x2', 30: 'sint16x2',
    34: 'float32', 35: 'uint32', 36: 'sint32', 38: 'unorm8x2', 39: 'uint8x2', 40: 'snorm8x2', 41: 'sint8x2',
    43: 'float16', 45: 'unorm16', 46: 'uint16', 47: 'snorm16', 48: 'sint16', 50: 'unorm8', 51: 'uint8', 52: 'snorm8',
    53: 'sint8',
  };
  const BLEND = [null, 'zero', 'one', 'src', 'one-minus-src', 'src-alpha', 'one-minus-src-alpha', 'dst-alpha',
    'one-minus-dst-alpha', 'dst', 'one-minus-dst', 'src-alpha-saturated', 'constant', 'one-minus-constant', 'src1',
    'one-minus-src1', 'src1-alpha', 'one-minus-src1-alpha'];
  const BLEND_OP = [null, 'add', 'subtract', 'reverse-subtract', 'min', 'max'];
  const COMPARE = [null, 'never', 'less', 'equal', 'less-equal', 'greater', 'not-equal', 'greater-equal', 'always'];
  const STENCIL_OP = [null, 'keep', 'zero', 'replace', 'increment-clamp', 'decrement-clamp', 'invert', 'increment-wrap',
    'decrement-wrap'];
  const TOPOLOGY = [null, 'point-list', 'line-list', 'line-strip', 'triangle-list', 'triangle-strip', 'triangle-list'];
  const CULL = [null, 'none', 'front', 'back'];
  const ADDRESS = [null, 'repeat', 'mirror-repeat', 'clamp-to-edge', 'clamp-to-edge', 'mirror-repeat'];
  const FILTER = [null, 'nearest', 'linear'];
  const VIEW_DIM = ['2d', '1d', '2d', '3d', 'cube', '2d-array', 'cube-array'];
  const ASPECT = ['all', 'depth-only', 'stencil-only'];
  const SAMPLE_TYPE = ['float', 'unfilterable-float', 'uint', 'sint', 'depth'];

  let device = null, adapter = null, context = null, canvas = null, canvasFormat = 'bgra8unorm';
  let statusAddr = 0;  // the client's status block (int32 words, see plume_webgpu.cpp WebGpuStatus)
  const objects = [];  // id -> object
  const errors = { count: 0 };
  let blit = null;  // the present pipeline
  let clearRing = null;
  let clearRingOffset = 0;
  let presentCount = 0;
  let sizeCanvas = null;  // the canvas's drawing size from the page's layout (init)

  // Views of the wasm memory, made again when it grows (a new SharedArrayBuffer).
  let views = null;
  function heap8() {
    const buf = wasmMemory.buffer;
    if (!views || views.buffer !== buf) views = { buffer: buf, u8: new Uint8Array(buf), i32: new Int32Array(buf) };
    return views.u8;
  }
  function heap32() {
    heap8();
    return views.i32;
  }
  function log(text) {
    console.log('[webgpu] ' + text);
    if (Module.print) Module.print('[webgpu] ' + text);
  }
  function warnOnce(key, text) {
    warnOnce.seen = warnOnce.seen || new Map();
    const n = (warnOnce.seen.get(key) || 0) + 1;
    warnOnce.seen.set(key, n);
    if (n <= 3) log(text + (n === 3 ? ' (no more of these)' : ''));
  }
  function status(i, v) {
    const h = heap32();
    Atomics.store(h, (statusAddr / 4) + i, v);
    Atomics.notify(h, (statusAddr / 4) + i);
  }

  // ---- init -------------------------------------------------------------------------------------------------------

  // Status block words: 0 state (0 pending, 1 ready, -1 failed), 1 features (bit 0 bc, 1 float32-filterable,
  // 2 depth32float-stencil8, 3 dual-source, 4 unorm8x4-bgra vertex format, 5 rg11b10 renderable), 2 canvas width,
  // 3 canvas height, 4 presents done, 5 max texture 2D, 6 max storage binding (MB), 7 max inter-stage variables,
  // 8 max vertex buffers, 9 max color attachment bytes per sample, 10 error count.
  async function init(addr, selector) {
    statusAddr = addr;
    try {
      if (!navigator.gpu) throw new Error('this browser has no WebGPU (navigator.gpu); a secure context (https or localhost) is needed');
      adapter = await navigator.gpu.requestAdapter({ powerPreference: 'high-performance' });
      if (!adapter) throw new Error('no WebGPU adapter');
      const want = ['texture-compression-bc', 'float32-filterable', 'depth32float-stencil8', 'dual-source-blending',
        'rg11b10ufloat-renderable', 'texture-formats-tier1'];
      const features = want.filter((f) => adapter.features.has(f));
      const L = adapter.limits;
      const requiredLimits = {};
      for (const k of ['maxStorageBufferBindingSize', 'maxBufferSize', 'maxTextureDimension2D', 'maxTextureDimension3D',
        'maxTextureArrayLayers', 'maxInterStageShaderVariables', 'maxVertexAttributes', 'maxVertexBuffers',
        'maxColorAttachmentBytesPerSample', 'maxSampledTexturesPerShaderStage', 'maxSamplersPerShaderStage',
        'maxUniformBufferBindingSize', 'maxBindingsPerBindGroup', 'maxDynamicUniformBuffersPerPipelineLayout',
        'maxStorageBuffersPerShaderStage', 'maxComputeWorkgroupStorageSize', 'maxComputeInvocationsPerWorkgroup',
        'maxComputeWorkgroupSizeX', 'maxComputeWorkgroupSizeY']) {
        if (L[k] !== undefined) requiredLimits[k] = L[k];
      }
      device = await adapter.requestDevice({ requiredFeatures: features, requiredLimits });
      device.lost.then((info) => { log('device lost: ' + info.message); status(0, -2); });
      device.onuncapturederror = (e) => {
        errors.count++;
        status(10, errors.count);
        warnOnce('err:' + e.error.message.slice(0, 80), 'error: ' + e.error.message);
      };
      canvas = document.querySelector(selector) || Module.canvas;
      sizeCanvas = () => {
        // A canvas not laid out (a hidden tab or pane, display:none) is 0x0: draw at 1280x720 until it has a size.
        const dpr = window.devicePixelRatio || 1;
        let w = Math.round(canvas.clientWidth * dpr), h = Math.round(canvas.clientHeight * dpr);
        if (w < 64 || h < 64) { w = 1280; h = 720; }
        if (canvas.width !== w || canvas.height !== h) { canvas.width = w; canvas.height = h; }
        sizeCanvas.w = w;
        sizeCanvas.h = h;
        status(2, canvas.width);
        status(3, canvas.height);
      };
      new ResizeObserver(sizeCanvas).observe(canvas);
      sizeCanvas();
      context = canvas.getContext('webgpu');
      canvasFormat = navigator.gpu.getPreferredCanvasFormat();
      context.configure({ device, format: canvasFormat, alphaMode: 'opaque' });
      let bits = 0;
      features.forEach((f) => { bits |= 1 << want.indexOf(f); });
      // unorm8x4-bgra: a vertex format of newer browsers; test it with a throwaway pipeline-less check.
      status(1, bits);
      status(5, device.limits.maxTextureDimension2D);
      status(6, Math.floor(device.limits.maxStorageBufferBindingSize / (1 << 20)));
      status(7, device.limits.maxInterStageShaderVariables);
      status(8, device.limits.maxVertexBuffers);
      status(9, device.limits.maxColorAttachmentBytesPerSample);
      const info = adapter.info || {};
      log(`adapter ${info.vendor || '?'} ${info.architecture || ''} ${info.description || ''}; features ${features.join(' ')}; ` +
        `canvas ${canvasFormat} ${canvas.width}x${canvas.height}`);
      makeBlit();
      clearRing = device.createBuffer({ size: 1 << 16, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
      makeReserved();
      status(0, 1);
    } catch (e) {
      log('WebGPU init failed: ' + e.message);
      Module.setStatus && Module.setStatus('WebGPU: ' + e.message);
      status(0, -1);
    }
  }

  // The objects plume_webgpu.cpp names by fixed ids (ReservedId): the push constant ring and the stand-ins for
  // textures, samplers and storage a draw's words do not name.
  const PUSH_RING_SIZE = 4 << 20;
  let pushScratch = null;
  function makeReserved() {
    objects[1] = { id: 1, gpu: device.createBuffer({ size: PUSH_RING_SIZE, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST }), size: PUSH_RING_SIZE };
    pushScratch = new Uint8Array(PUSH_RING_SIZE);
    const usage = GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST;
    const tex = (format, layers, dimension) => device.createTexture({ size: { width: 1, height: 1, depthOrArrayLayers: layers }, format, usage, dimension: dimension || '2d' });
    const white = new Uint8Array([255, 255, 255, 255]);
    const t2d = tex('rgba8unorm', 1), tcube = tex('rgba8unorm', 6), tarr = tex('rgba8unorm', 2), t3d = tex('rgba8unorm', 1, '3d');
    for (const [t, layers] of [[t2d, 1], [tcube, 6], [tarr, 2], [t3d, 1]]) {
      for (let l = 0; l < layers; l++) device.queue.writeTexture({ texture: t, origin: { x: 0, y: 0, z: l } }, white, { bytesPerRow: 4 }, { width: 1, height: 1 });
    }
    objects[2] = t2d.createView();
    objects[3] = tcube.createView({ dimension: 'cube' });
    objects[4] = tarr.createView({ dimension: '2d-array' });
    objects[5] = t3d.createView({ dimension: '3d' });
    objects[6] = device.createSampler({ magFilter: 'linear', minFilter: 'linear', mipmapFilter: 'linear', addressModeU: 'repeat', addressModeV: 'repeat' });
    objects[7] = tex('r32uint', 1).createView();
    objects[8] = tex('r32sint', 1).createView();
    objects[9] = { id: 9, gpu: device.createBuffer({ size: 256, usage: GPUBufferUsage.STORAGE }), size: 256 };
    objects[10] = tex('r32float', 1).createView();
  }

  function textOf(w, at, bytes) {
    const u8 = new Uint8Array(w.buffer, w.byteOffset + at * 4, bytes);
    return new TextDecoder().decode(u8.slice());
  }

  // ---- shaders and linking ------------------------------------------------------------------------------------

  // A shader: its WGSL and override ids. Game vertex shaders write 26 interpolators at fixed locations (0-25) and
  // WebGPU allows 16 (and the fragment shader's interpolation qualifiers must match), so each vertex + fragment pair is
  // linked when its pipeline is made: the vertex outputs the fragment shader does not read go, both sides are
  // renumbered 0.., and the vertex outputs take the fragment inputs' @interpolate.
  function overrideIds(text) {
    const ids = new Set();
    for (const m of text.matchAll(/@id\((\d+)u?\)/g)) ids.add(+m[1]);
    return ids;
  }

  // The entry point of a stage: its parameters (balanced parentheses: @location(0u) and @interpolate(...) are inside)
  // and its return type.
  function findEntry(text, stage) {
    const m = new RegExp('@' + stage + '\\s*\\n\\s*fn (\\w+)\\(').exec(text);
    if (!m) return null;
    const open = m.index + m[0].length - 1;
    let depth = 0, close = -1;
    for (let k = open; k < text.length; k++) {
      if (text[k] === '(') depth++;
      else if (text[k] === ')') { if (--depth === 0) { close = k; break; } }
    }
    if (close < 0) return null;
    const brace = text.indexOf('{', close);
    const after = text.slice(close + 1, brace).trim();
    return { name: m[1], open, close, params: text.slice(open + 1, close), ret: after.startsWith('->') ? after.slice(2).trim() : '' };
  }

  function structBody(text, name) {
    const m = new RegExp('struct ' + name + ' \\{\\n([\\s\\S]*?)\\n\\}').exec(text);
    if (!m) return null;
    // members: "  @builtin(position)\n  gl_Position : vec4<f32>," or "  @location(0u) @interpolate(flat)\n  name : type,"
    const members = [];
    const lines = m[1].split('\n').map((l) => l.trim()).filter((l) => l);
    let attrs = '';
    for (const l of lines) {
      if (l.startsWith('@') && !/:\s*[\w<>]/.test(l.replace(/@\w+\([^)]*\)/g, ''))) { attrs += (attrs ? ' ' : '') + l; continue; }
      // "attrs name : type," possibly on one line
      const mm = /^((?:@\w+(?:\([^)]*\))?\s*)*)(\w+)\s*:\s*(.+?),?$/.exec(l);
      if (mm) {
        members.push({ attr: (attrs + ' ' + mm[1]).trim(), name: mm[2], type: mm[3].trim() });
        attrs = '';
      }
    }
    return { index: m.index, length: m[0].length, members };
  }

  // f32 / vecN<f32> component counts, for vertex outputs whose fragment input is wider or narrower.
  function comps(type) {
    if (type === 'f32') return 1;
    const m = /^vec(\d)<f32>$/.exec(type);
    return m ? +m[1] : 0;
  }
  function convertExpr(expr, from, to) {
    if (from === to) return expr;
    const a = comps(from), b = comps(to);
    if (!a || !b) return `${to}()`;
    if (b === 1) return `(${expr})${a > 1 ? '.x' : ''}`;
    if (a === 1) return `${to}(${expr}${', 0.0'.repeat(b - 1)})`;
    if (a > b) return `(${expr}).${'xyzw'.slice(0, b)}`;
    return `${to}(${expr}${', 0.0'.repeat(b - a)})`;
  }

  // A call's arguments, split at the top-level commas.
  function splitArgs(s) {
    const out = [];
    let depth = 0, start = 0;
    for (let i = 0; i < s.length; i++) {
      const c = s[i];
      if (c === '(' || c === '<' || c === '[') depth++;
      else if (c === ')' || c === '>' || c === ']') depth--;
      else if (c === ',' && depth === 0) { out.push(s.slice(start, i).trim()); start = i + 1; }
    }
    if (s.slice(start).trim()) out.push(s.slice(start).trim());
    return out;
  }

  function linkStages(vsText, psText) {
    const fs = findEntry(psText, 'fragment');
    // fragment inputs: @location(N) [@interpolate(...)] name : type
    const inputs = [];
    let ps = psText;
    if (fs) {
      for (const m of fs.params.matchAll(/@location\((\d+)u?\)\s*((?:@interpolate\([^)]*\)\s*)?)(\w+)\s*:\s*([\w<>]+)/g)) {
        inputs.push({ loc: +m[1], interp: m[2].trim(), name: m[3], type: m[4] });
      }
      inputs.sort((x, y) => x.loc - y.loc);
    }
    const newLoc = new Map(inputs.map((x, i) => [x.loc, i]));
    if (fs) {
      const params = fs.params.replace(/@location\((\d+)u?\)/g, (t, n) => `@location(${newLoc.get(+n)})`);
      ps = psText.slice(0, fs.open + 1) + params + psText.slice(fs.close);
    }
    const ve = findEntry(vsText, 'vertex');
    if (!ve || !/^\w+$/.test(ve.ret)) return { vs: vsText, ps };
    const sname = ve.ret;
    const st = structBody(vsText, sname);
    if (!st) return { vs: vsText, ps };
    const keep = [];
    const written = new Set();
    st.members.forEach((m, i) => {
      const lm = /@location\((\d+)u?\)/.exec(m.attr);
      if (!lm) { keep.push({ i, text: `  ${m.attr}\n  ${m.name} : ${m.type},`, from: m.type, to: m.type }); return; }
      const loc = +lm[1];
      if (!newLoc.has(loc)) return;
      written.add(loc);
      const inp = inputs[newLoc.get(loc)];
      keep.push({ i, text: `  @location(${newLoc.get(loc)}) ${inp.interp}\n  ${m.name} : ${inp.type},`, from: m.type, to: inp.type });
    });
    // fragment inputs no vertex output writes: zero-valued members
    const extra = inputs.filter((x) => !written.has(x.loc));
    let body = keep.map((k) => k.text).join('\n');
    for (const x of extra) body += `\n  @location(${newLoc.get(x.loc)}) ${x.interp}\n  web_unwritten_${x.loc} : ${x.type},`;
    let vs = vsText.slice(0, st.index) + `struct ${sname} {\n${body}\n}` + vsText.slice(st.index + st.length);
    // the constructor: return NAME(a, b, ...)
    const cm = new RegExp('return ' + sname + '\\(').exec(vs);
    if (cm) {
      const open = cm.index + cm[0].length - 1;
      let depth = 0, close = open;
      for (let k = open; k < vs.length; k++) {
        if (vs[k] === '(') depth++;
        else if (vs[k] === ')') { if (--depth === 0) { close = k; break; } }
      }
      const args = splitArgs(vs.slice(open + 1, close));
      const kept = keep.map((k) => convertExpr(args[k.i], k.from, k.to));
      for (const x of extra) kept.push(`${x.type}()`);
      vs = vs.slice(0, open + 1) + kept.join(', ') + vs.slice(close);
    }
    return { vs, ps };
  }

  // The colour outputs (locations) a fragment shader writes.
  function fragmentOutputs(text) {
    const fs = findEntry(text, 'fragment');
    const out = new Set();
    if (!fs) return out;
    if (fs.ret.startsWith('@location')) {
      const m = /@location\((\d+)u?\)/.exec(fs.ret);
      if (m) out.add(+m[1]);
      return out;
    }
    const st = /^\w+$/.test(fs.ret) ? structBody(text, fs.ret) : null;
    if (st) for (const m of st.members) {
      const lm = /@location\((\d+)u?\)/.exec(m.attr);
      if (lm) out.add(+lm[1]);
    }
    return out;
  }

  const moduleCache = new Map();
  function shaderModule(text, label) {
    let m = moduleCache.get(text);
    if (!m) {
      m = device.createShaderModule({ code: text, label });
      moduleCache.set(text, m);
      if (moduleCache.size > 4096) moduleCache.delete(moduleCache.keys().next().value);
      m.getCompilationInfo().then((info) => {
        for (const msg of info.messages) {
          if (msg.type === 'error') warnOnce('wgsl:' + label, `${label}: ${msg.lineNum}:${msg.linePos} ${msg.message}`);
        }
      });
    }
    return m;
  }

  // A depth range the other way round (MinZ above MaxZ: Oblivion's own viewport, 1 to 0, its depth reversed) is Vulkan's
  // and D3D12's but not WebGPU's, which wants minDepth <= maxDepth (the range had been made 1 to 1: every draw's depth
  // 1, and with the game's GREATEREQUAL the last draw over a pixel won; the walls drawn after the actors covered them,
  // the player's head and body). So every vertex shader maps its position's depth itself, with two overrides that are
  // the identity by default: z' = min * w + z * (max - min), which after the viewport's 0 to 1 is the window depth the
  // reversed range gives (min + z / w * (max - min)), clipped where it was (z' in [0, w] exactly when z is).
  const DEPTH_MAP_WGSL = `
override web_depth_min : f32 = 0.0;
override web_depth_max : f32 = 1.0;
fn web_depth_map(p : vec4<f32>) -> vec4<f32> {
  return vec4<f32>(p.xy, web_depth_min * p.w + p.z * (web_depth_max - web_depth_min), p.w);
}
`;
  function injectDepthMap(text) {
    const ve = findEntry(text, 'vertex');
    if (!ve) return null;
    const body = text.slice(ve.close);
    const re = /return (\w+)\((v_\d+\.gl_Position)([,)])/;
    if (!re.test(body)) return null;
    return text.slice(0, ve.close) + body.replace(re, 'return $1(web_depth_map($2)$3') + DEPTH_MAP_WGSL;
  }

  function constantsFor(ids, constants) {
    const out = {};
    let any = false;
    for (const [id, v] of constants) {
      if (ids.has(id)) { out[String(id)] = v; any = true; }
    }
    return any ? out : undefined;
  }

  // ---- the present blit ---------------------------------------------------------------------------------------

  function makeBlit() {
    const code = `
@group(0) @binding(0) var src : texture_2d<f32>;
@group(0) @binding(1) var smp : sampler;
struct V { @builtin(position) pos : vec4<f32>, @location(0) uv : vec2<f32> };
@vertex fn vs(@builtin(vertex_index) i : u32) -> V {
  let p = vec2<f32>(f32((i << 1u) & 2u), f32(i & 2u));
  var o : V; o.pos = vec4<f32>(p * vec2<f32>(2.0, -2.0) + vec2<f32>(-1.0, 1.0), 0.0, 1.0); o.uv = p; return o;
}
@fragment fn fs(v : V) -> @location(0) vec4<f32> { return vec4<f32>(textureSample(src, smp, v.uv).rgb, 1.0); }`;
    const module = device.createShaderModule({ code, label: 'present blit' });
    blit = {
      pipeline: device.createRenderPipeline({
        layout: 'auto', label: 'present blit',
        vertex: { module, entryPoint: 'vs' },
        fragment: { module, entryPoint: 'fs', targets: [{ format: canvasFormat }] },
        primitive: { topology: 'triangle-list' },
      }),
      sampler: device.createSampler({ magFilter: 'linear', minFilter: 'linear' }),
      groups: new Map(),
    };
  }

  // Clear rectangles that do not cover the target: a quad per rectangle with a pipeline that writes the colour to one
  // target (or the depth/stencil), scissored to the rectangle.
  const clearPipelines = new Map();
  function clearPipeline(fb, target, depth, stencil) {
    const key = fb.key + '|' + target + '|' + depth + stencil;
    let p = clearPipelines.get(key);
    if (p) return p;
    const code = `
struct C { color : vec4<f32>, depth : f32 };
@group(0) @binding(0) var<uniform> c : C;
@vertex fn vs(@builtin(vertex_index) i : u32) -> @builtin(position) vec4<f32> {
  let p = vec2<f32>(f32((i << 1u) & 2u), f32(i & 2u));
  return vec4<f32>(p * 2.0 - 1.0, c.depth, 1.0);
}
struct O { ${fb.colorFormats.map((f, i) => `@location(${i}) o${i} : vec4<f32>`).join(', ') || '@location(0) unused : vec4<f32>'} };
@fragment fn fs() -> O { var o : O; ${fb.colorFormats.map((f, i) => `o.o${i} = c.color;`).join(' ')} return o; }`;
    const module = device.createShaderModule({ code, label: 'clear' });
    const desc = {
      layout: 'auto', label: 'clear rects',
      vertex: { module, entryPoint: 'vs' },
      primitive: { topology: 'triangle-list' },
      multisample: { count: fb.samples },
    };
    if (fb.colorFormats.length) {
      desc.fragment = {
        module, entryPoint: 'fs',
        targets: fb.colorFormats.map((f, i) => ({ format: f, writeMask: i === target ? GPUColorWrite.ALL : 0 })),
      };
    }
    if (fb.depthFormat) {
      const hasStencil = fb.depthFormat.includes('stencil') || fb.depthFormat === 'stencil8';
      desc.depthStencil = {
        format: fb.depthFormat, depthWriteEnabled: !!depth, depthCompare: 'always',
      };
      if (hasStencil && stencil) {
        const face = { compare: 'always', passOp: 'replace', failOp: 'replace', depthFailOp: 'replace' };
        desc.depthStencil.stencilFront = face;
        desc.depthStencil.stencilBack = face;
        desc.depthStencil.stencilWriteMask = 0xFF;
      }
      if (fb.depthReadOnly) { desc.depthStencil.depthWriteEnabled = false; }
    }
    p = device.createRenderPipeline(desc);
    clearPipelines.set(key, p);
    return p;
  }

  // ---- command lists ------------------------------------------------------------------------------------------

  // One submit's recording state (render passes begun lazily, their state re-applied in each new pass).
  function Recorder() {
    this.enc = device.createCommandEncoder();
    this.pass = null;
    this.cpass = null;
    this.fb = null;
    this.loads = null;  // per colour attachment: null (load) or [r,g,b,a]; depth/stencil: clear values or null
    this.depthClear = null;
    this.stencilClear = null;
    this.pipelineObj = null;  // the game's pipeline; the draws bind what pipelineFor makes of it
    this.bound = null;
    this.groups = [null, null, null, null];
    this.vbufs = [];
    this.ibuf = null;
    this.viewport = null;
    this.scissor = null;
    this.blendConstant = null;
    this.stencilRef = 0;
    this.cpipeline = null;
    this.cgroups = [null, null, null, null];
    this.readbacks = new Map();  // buffer id -> [lo, hi]
    this.pushOff = 0;  // bytes of the push ring this submit uses (written before it runs)
    this.pushWritten = 0;
    this.rawScissor = null;
    this.drewInPass = false;
  }

  // The push constants so far to the ring, then the commands so far to the queue (before a queue write the next
  // commands must see: a texture upload whose rows are not 256-byte multiples goes through queue.writeTexture).
  Recorder.prototype.split = function () {
    this.flushClears();
    this.writePush();
    device.queue.submit([this.enc.finish()]);
    this.enc = device.createCommandEncoder();
  };

  Recorder.prototype.writePush = function () {
    if (this.pushOff > this.pushWritten) {
      device.queue.writeBuffer(objects[1].gpu, this.pushWritten, pushScratch, this.pushWritten, this.pushOff - this.pushWritten);
      this.pushWritten = this.pushOff;
    }
  };

  Recorder.prototype.endPass = function () {
    if (this.pass) { this.pass.end(); this.pass = null; }
    if (this.cpass) { this.cpass.end(); this.cpass = null; }
  };

  // Clears still pending on the framebuffer (no draw after them yet): a pass that only clears.
  Recorder.prototype.flushClears = function () {
    if (!this.pass && this.fb && this.hasPendingClear()) { this.beginPass(); }
    this.endPass();
  };

  Recorder.prototype.hasPendingClear = function () {
    return (this.loads && this.loads.some((x) => x)) || this.depthClear !== null || this.stencilClear !== null;
  };

  Recorder.prototype.beginPass = function () {
    if (this.cpass) { this.cpass.end(); this.cpass = null; }
    const fb = this.fb;
    const desc = { colorAttachments: [] };
    fb.colors.forEach((view, i) => {
      const c = this.loads[i];
      desc.colorAttachments.push({
        view, loadOp: c ? 'clear' : 'load', storeOp: 'store', clearValue: c ? { r: c[0], g: c[1], b: c[2], a: c[3] } : undefined,
      });
    });
    if (fb.depth) {
      const d = { view: fb.depth };
      if (fb.hasDepth) {
        if (fb.depthReadOnly) d.depthReadOnly = true;
        else { d.depthLoadOp = this.depthClear !== null ? 'clear' : 'load'; d.depthStoreOp = 'store'; d.depthClearValue = this.depthClear ?? 1; }
      }
      if (fb.hasStencil) {
        if (fb.depthReadOnly) d.stencilReadOnly = true;
        else { d.stencilLoadOp = this.stencilClear !== null ? 'clear' : 'load'; d.stencilStoreOp = 'store'; d.stencilClearValue = this.stencilClear ?? 0; }
      }
      desc.depthStencilAttachment = d;
    }
    this.pass = this.enc.beginRenderPass(desc);
    this.loads = fb.colors.map(() => null);
    this.depthClear = null;
    this.stencilClear = null;
    const p = this.pass;
    if (this.viewport) p.setViewport(...this.viewport);
    if (this.scissor) p.setScissorRect(...this.scissor);
    if (this.blendConstant) p.setBlendConstant(this.blendConstant);
    p.setStencilReference(this.stencilRef);
    this.bound = null;  // the next draw binds its pipeline
    this.groups.forEach((g, i) => { if (g) p.setBindGroup(i, g[0], g[1]); });
    this.vbufs.forEach((v, i) => { if (v) p.setVertexBuffer(i, v[0], v[1], v[2]); });
    if (this.ibuf) p.setIndexBuffer(this.ibuf[0], this.ibuf[1], this.ibuf[2], this.ibuf[3]);
  };

  Recorder.prototype.renderPass = function () {
    if (!this.pass) {
      if (!this.fb) return null;
      this.beginPass();
    }
    return this.pass;
  };

  Recorder.prototype.computePass = function () {
    if (this.pass) this.flushClears();
    if (!this.cpass) {
      this.flushClears();
      this.cpass = this.enc.beginComputePass();
      if (this.cpipeline) this.cpass.setPipeline(this.cpipeline);
      this.cgroups.forEach((g, i) => { if (g) this.cpass.setBindGroup(i, g[0], g[1]); });
    }
    return this.cpass;
  };

  // The rectangles cover the whole target (or there are none).
  function coversAll(fb, rects) {
    if (!rects.length) return true;
    return rects.some((r) => r[0] <= 0 && r[1] <= 0 && r[2] >= fb.width && r[3] >= fb.height);
  }

  Recorder.prototype.clearRects = function (rects, target, color, depth, stencil, dv, sv) {
    const fb = this.fb;
    const pipeline = clearPipeline(fb, target, depth, stencil);
    const off = clearRingOffset;
    clearRingOffset = (clearRingOffset + 256) % (1 << 16);
    device.queue.writeBuffer(clearRing, off, new Float32Array([color[0], color[1], color[2], color[3], dv, 0, 0, 0]));
    const group = device.createBindGroup({ layout: pipeline.getBindGroupLayout(0), entries: [{ binding: 0, resource: { buffer: clearRing, offset: off, size: 32 } }] });
    const p = this.renderPass();
    p.setPipeline(pipeline);
    p.setBindGroup(0, group);
    p.setStencilReference(sv);
    p.setViewport(0, 0, fb.width, fb.height, 0, 1);
    for (const r of rects) {
      const x = Math.max(0, r[0]), y = Math.max(0, r[1]);
      const w = Math.min(fb.width, r[2]) - x, h = Math.min(fb.height, r[3]) - y;
      if (w <= 0 || h <= 0) continue;
      p.setScissorRect(x, y, w, h);
      p.draw(3);
    }
    // the draw state back
    if (this.viewport) p.setViewport(...this.viewport);
    if (this.scissor) p.setScissorRect(...this.scissor); else p.setScissorRect(0, 0, fb.width, fb.height);
    p.setStencilReference(this.stencilRef);
    this.bound = null;
    this.groups.forEach((g, i) => { if (g) p.setBindGroup(i, g[0], g[1]); });
  };

  // ---- decoding -----------------------------------------------------------------------------------------------

  function f32(u) {
    f32.buf = f32.buf || new DataView(new ArrayBuffer(4));
    f32.buf.setUint32(0, u, true);
    return f32.buf.getFloat32(0, true);
  }
  function u64(lo, hi) { return lo + hi * 4294967296; }

  function fbClampScissor(fb, x, y, w, h) {
    const x0 = Math.min(Math.max(0, x), fb.width), y0 = Math.min(Math.max(0, y), fb.height);
    const x1 = Math.min(Math.max(0, x + w), fb.width), y1 = Math.min(Math.max(0, y + h), fb.height);
    return [x0, y0, x1 - x0, y1 - y0];
  }


  // A trace of the next submits' draws on the console (WebGpuServer.trace(n) from the page's console): what each draw
  // drew into, with what pipeline, viewport and scissor.
  let traceLeft = 0, traceCap = 200, traceFilter = '', traceConstants = null, traceVertexData = null;
  // WebGpuServer.debug('solid'): the game's blended draws in magenta without depth or stencil tests (where geometry
  // lands); 'opaque': their own colours without blending (what they write); 'mark:<label>': the draws of the pipelines
  // whose label has <label> (e.g. 'mark:shader 645 +') in magenta, blended or not, both sides. Off with debug('').
  let debugMode = '';
  function applyDebug(d, fb) {
    d.fragment = Object.assign({}, d.fragment, { targets: d.fragment.targets.map((t) => ({ format: t.format, writeMask: 15 })) });
    if (debugMode === 'solid' || debugMode.startsWith('mark:')) {
      d.fragment.module = device.createShaderModule({ code: '@fragment fn main() -> @location(0) vec4f { return vec4f(1.0, 0.0, 1.0, 1.0); }' });
      d.fragment.constants = {};
      d.fragment.targets = [d.fragment.targets[0]].concat(d.fragment.targets.slice(1).map((t) => ({ format: t.format, writeMask: 0 })));
    }
    // marked draws write their depth (where the pass may), so the draws after them do not cover them
    if (d.depthStencil) d.depthStencil = { format: d.depthStencil.format, depthCompare: 'always',
      depthWriteEnabled: debugMode.startsWith('mark:') && !(fb && fb.depthReadOnly) };
    if (debugMode.startsWith('mark:')) d.primitive = Object.assign({}, d.primitive, { cullMode: 'none' });
  }

  // WebGPU wants a pipeline's attachments to be its pass's exactly (Vulkan and D3D12 take a depth-only pipeline in a
  // pass with colour targets, or one without the pass's depth), and an indexed strip needs its index format in the
  // pipeline. So a draw's pipeline is made for the framebuffer it draws into and its index format: the game's pipeline
  // itself when they fit, else a variant of it (cached on the pipeline).
  function pipelineFor(rec, indexFormat) {
    const pl = rec.pipelineObj, fb = rec.fb;
    if (!pl || !pl.gpu || !fb) return null;
    const strip = indexFormat && pl.topo.endsWith('-strip') ? indexFormat : '';
    const depthMap = rec.depthMap && pl.depthMapped ? rec.depthMap : null;
    const key = fb.key + '|' + strip + '|' + debugMode + (depthMap ? '|z' + depthMap.join(',') : '');
    if (!pl.fits) pl.fits = new Map();
    let g = pl.fits.get(key);
    if (g === undefined) {
      g = makeVariant(pl, fb, strip, depthMap);
      pl.fits.set(key, g);
    }
    return g;
  }

  function makeVariant(pl, fb, strip, depthMap) {
    const base = pl.desc;
    const fragTargets = base.fragment ? base.fragment.targets : [];
    const colorsFit = fragTargets.length === fb.colorFormats.length && fragTargets.every((t, i) => t.format === fb.colorFormats[i]);
    const ds = base.depthStencil;
    const writesDS = ds && (ds.depthWriteEnabled || (ds.stencilFront && (ds.stencilWriteMask ?? 0xFF) !== 0));
    const dsFits = (ds ? ds.format : null) === (fb.depthFormat || null) && !(fb.depthReadOnly && writesDS);
    const samplesFit = (base.multisample.count || 1) === fb.samples;
    const debug = debugMode.startsWith('mark:') ? !!base.fragment && (base.label || '').includes(debugMode.slice(5))
      : debugMode && base.fragment && fragTargets.some((t) => t.blend);
    if (colorsFit && dsFits && samplesFit && !strip && !debug && !depthMap) return pl.gpu;
    const d = Object.assign({}, base);
    d.primitive = Object.assign({}, base.primitive);
    if (depthMap) {
      d.vertex = Object.assign({}, base.vertex, {
        constants: Object.assign({}, base.vertex.constants || {}, { web_depth_min: depthMap[0], web_depth_max: depthMap[1] }) });
    }
    if (strip) d.primitive.stripIndexFormat = strip;
    if (!colorsFit) {
      // the pass's colour targets; one the pipeline does not have takes nothing
      const targets = fb.colorFormats.map((f, i) => (fragTargets[i] && fragTargets[i].format === f ? fragTargets[i] : { format: f, writeMask: 0 }));
      d.fragment = base.fragment ? Object.assign({}, base.fragment, { targets })
        : { module: shaderModule('@fragment\nfn main() {}', 'no colour'), entryPoint: 'main', targets };
    }
    if (!dsFits) {
      if (!fb.depthFormat) {
        delete d.depthStencil;
      } else {
        const n = ds ? Object.assign({}, ds, { format: fb.depthFormat }) : { format: fb.depthFormat, depthWriteEnabled: false, depthCompare: 'always' };
        if (!fb.hasStencil) { delete n.stencilFront; delete n.stencilBack; delete n.stencilReadMask; delete n.stencilWriteMask; }
        if (!fb.hasDepth) { n.depthWriteEnabled = false; n.depthCompare = 'always'; delete n.depthBias; delete n.depthBiasSlopeScale; delete n.depthBiasClamp; }
        if (fb.depthReadOnly) { n.depthWriteEnabled = false; if (n.stencilFront) n.stencilWriteMask = 0; }
        d.depthStencil = n;
      }
    }
    if (!samplesFit) d.multisample = { count: fb.samples, alphaToCoverageEnabled: fb.samples > 1 && !!base.multisample.alphaToCoverageEnabled };
    if (debug) applyDebug(d, fb);
    try {
      device.pushErrorScope('validation');
      const g = device.createRenderPipeline(d);
      device.popErrorScope().then((err) => { if (err) warnOnce('variant:' + err.message.slice(0, 60), `pipeline variant ${base.label} for ${fb.key}: ${err.message}`); });
      return g;
    } catch (e) {
      warnOnce('variant', 'pipeline variant: ' + e.message);
      return pl.gpu;
    }
  }

  // The pipeline for a draw, bound if it is not already.
  function bindForDraw(rec, pass, indexFormat) {
    const g = pipelineFor(rec, indexFormat);
    if (g && g !== rec.bound) { pass.setPipeline(g); rec.bound = g; }
    return g;
  }
  function traceDraw(rec, kind, count, inst, args) {
    if (!traceLeft) return;
    const fb = rec.fb, pl = rec.pipelineObj;
    const lines = (rec.traceLines = rec.traceLines || []);
    rec.traceSeen = (rec.traceSeen || 0) + 1;
    if (lines.length > traceCap) return;
    if (traceFilter && !(pl && (pl.label || '').includes(traceFilter))) return;
    // the draw's first four vertex constants (c0..c3), copied when the submit is (WebGpuServer.trace)
    if (args && rec.groups[1] && rec.groups[1][0].traceUbo0) {
      const [buf, base] = rec.groups[1][0].traceUbo0;
      (rec.traceCaps = rec.traceCaps || []).push([lines.length, buf, base + (rec.groups[1][1][0] || 0), traceFilter ? 4096 : 64]);
    }
    // and the shared constants (binding 2) as the draw had them
    if (traceFilter && args && rec.groups[1] && rec.groups[1][0].traceUbo2) {
      const [buf, base] = rec.groups[1][0].traceUbo2;
      (rec.traceVbs = rec.traceVbs || []).push([-1 - lines.length, buf, base + (rec.groups[1][1][2] || 0), 1360]);
    }
    // a filtered trace also keeps the first 4 KB of each draw's vertex buffer 0 as the GPU had it (traceVertices())
    if (traceFilter && rec.vbufs[0]) {
      const [vbuf, voff, vsize] = rec.vbufs[0];
      const n = Math.min(vsize || vbuf.size - voff, 4096) & ~3;
      if (n > 0) (rec.traceVbs = rec.traceVbs || []).push([lines.length, vbuf, voff, n]);
    }
    lines.push(`${kind} ${count}x${inst} fb ${fb ? fb.width + 'x' + fb.height + ' [' + fb.colorFormats.join(',') + ']' + (fb.depthFormat ? ' ' + fb.depthFormat : '') : 'none'} ` +
      `pipe ${pl ? pl.label + ' ' + pl.topo + ' ' + pl.targets.join(',') + ' ' + pl.ds + (pl.gpu ? '' : ' (failed)') : 'none'} ` +
      `vp ${rec.viewport ? rec.viewport.map((v) => Math.round(v * 100) / 100).join(',') : '-'} sc ${rec.scissor ? rec.scissor.join(',') : '-'} ` +
      `groups ${[0, 1, 2, 3].map((g) => (rec.groups[g] ? 'y' : '-')).join('')} vb ${rec.vbufs.filter(Boolean).length}` +
      (args && rec.groups[1] ? ` g1 [${rec.groups[1][0].traceText}] offs ${rec.groups[1][1].join(',')}` : '') +
      (args ? ` args ${args.join(',')} vbs ${rec.vbufs.map((v) => (v ? '#' + v[3] + '@' + v[1] + '+' + v[2] : '-')).join(' ')}` +
        (rec.ibuf ? ` ib ${rec.ibuf[1]} ${rec.ibuf[2]}+${rec.ibuf[3]}` : '') : '') +
      (rec.blendConstant ? ` bc ${rec.blendConstant.a.toFixed(2)}` : ''));
  }

  function recordList(rec, w, p, end) {
    while (p < end) {
      const op = w[p], n = w[p + 1];
      const a = p + 2;
      p = a + n;
      // an unfiltered trace also lists the target changes, clears, copies and resolves among the draws
      if (traceLeft && !traceFilter && (op === OP.SET_FRAMEBUFFER || op === OP.CLEAR_COLOR || op === OP.CLEAR_DS ||
          (op >= OP.COPY_B2B && op <= OP.RESOLVE))) {
        const lines = (rec.traceLines = rec.traceLines || []);
        if (lines.length <= traceCap) {
          const name = Object.keys(OP).find((k) => OP[k] === op);
          const fbo = op === OP.SET_FRAMEBUFFER ? objects[w[a]] : null;
          lines.push(`-- ${name} ${Array.from(w.subarray(a, a + Math.min(n, 8))).join(',')}` +
            (fbo ? ` (fb #${w[a]} ${fbo.width}x${fbo.height})` : ''));
        }
      }
      switch (op) {
        case OP.SET_FRAMEBUFFER: {
          const fb = objects[w[a]] || null;
          if (fb !== rec.fb) {
            rec.flushClears();
            rec.fb = fb;
            rec.loads = fb ? fb.colors.map(() => null) : null;
            rec.depthClear = null;
            rec.stencilClear = null;
            // the viewport and scissor carry over (Vulkan's dynamic state does); the scissor is clamped to the new target
            if (fb && rec.rawScissor) rec.scissor = fbClampScissor(fb, ...rec.rawScissor);
          }
          break;
        }
        case OP.CLEAR_COLOR: {
          if (!rec.fb) break;
          const target = w[a], color = [f32(w[a + 1]), f32(w[a + 2]), f32(w[a + 3]), f32(w[a + 4])];
          const count = w[a + 5];
          const rects = [];
          for (let k = 0; k < count; k++) rects.push([w[a + 6 + k * 4] | 0, w[a + 7 + k * 4] | 0, w[a + 8 + k * 4] | 0, w[a + 9 + k * 4] | 0]);
          if (target >= rec.fb.colors.length) break;
          if (coversAll(rec.fb, rects)) {
            if (rec.pass) rec.endPass();
            rec.loads[target] = color;
          } else {
            rec.clearRects(rects, target, color, false, false, 0, 0);
          }
          break;
        }
        case OP.CLEAR_DS: {
          if (!rec.fb || !rec.fb.depth) break;
          const flags = w[a], dv = f32(w[a + 1]), sv = w[a + 2], count = w[a + 3];
          const rects = [];
          for (let k = 0; k < count; k++) rects.push([w[a + 4 + k * 4] | 0, w[a + 5 + k * 4] | 0, w[a + 6 + k * 4] | 0, w[a + 7 + k * 4] | 0]);
          const cd = (flags & 1) && rec.fb.hasDepth, cs = (flags & 2) && rec.fb.hasStencil;
          if (!cd && !cs) break;
          if (coversAll(rec.fb, rects)) {
            if (rec.pass) rec.endPass();
            if (cd) rec.depthClear = dv;
            if (cs) rec.stencilClear = sv;
          } else {
            rec.clearRects(rects, -1, [0, 0, 0, 0], cd, cs, dv, sv);
          }
          break;
        }
        case OP.SET_PIPELINE: {
          const pl = objects[w[a]];
          rec.pipelineObj = pl || null;  // bound at the next draw, made for its pass (pipelineFor)
          break;
        }
        case OP.SET_BIND_GROUP: {
          const g = w[a], bg = objects[w[a + 1]], cnt = w[a + 2];
          const offs = [];
          for (let k = 0; k < cnt; k++) offs.push(w[a + 3 + k]);
          rec.groups[g] = bg ? [bg, offs] : null;
          if (rec.pass && bg) rec.pass.setBindGroup(g, bg, offs);
          break;
        }
        case OP.SET_PUSH:
        case OP.SET_COMPUTE_PUSH: {
          const g = w[a], bg = objects[w[a + 1]], size = w[a + 2];
          if (rec.pushOff + size > PUSH_RING_SIZE) { warnOnce('pushfull', 'push ring full'); rec.split(); rec.pushOff = rec.pushWritten = 0; }
          const off = rec.pushOff;
          pushScratch.set(new Uint8Array(w.buffer, w.byteOffset + (a + 3) * 4, size), off);
          rec.pushOff += (size + 255) & ~255;
          if (op === OP.SET_PUSH) {
            rec.groups[g] = bg ? [bg, [off]] : null;
            if (rec.pass && bg) rec.pass.setBindGroup(g, bg, [off]);
          } else {
            rec.cgroups[g] = bg ? [bg, [off]] : null;
            if (rec.cpass && bg) rec.cpass.setBindGroup(g, bg, [off]);
          }
          break;
        }
        case OP.SET_COMPUTE_PIPELINE: {
          const pl = objects[w[a]];
          rec.cpipeline = pl ? pl.gpu : null;
          if (rec.cpass && rec.cpipeline) rec.cpass.setPipeline(rec.cpipeline);
          break;
        }
        case OP.SET_COMPUTE_BIND_GROUP: {
          const g = w[a], bg = objects[w[a + 1]], cnt = w[a + 2];
          const offs = [];
          for (let k = 0; k < cnt; k++) offs.push(w[a + 3 + k]);
          rec.cgroups[g] = bg ? [bg, offs] : null;
          if (rec.cpass && bg) rec.cpass.setBindGroup(g, bg, offs);
          break;
        }
        case OP.SET_VERTEX_BUFFER: {
          const slot = w[a], b = objects[w[a + 1]];
          const v = b ? [b.gpu, u64(w[a + 2], w[a + 3]), w[a + 4], w[a + 1]] : null;
          rec.vbufs[slot] = v;
          if (rec.pass && v) rec.pass.setVertexBuffer(slot, v[0], v[1], v[2]);
          break;
        }
        case OP.SET_INDEX_BUFFER: {
          const b = objects[w[a]];
          const v = b ? [b.gpu, w[a + 1] ? 'uint32' : 'uint16', u64(w[a + 2], w[a + 3]), w[a + 4]] : null;
          rec.ibuf = v;
          if (rec.pass && v) rec.pass.setIndexBuffer(v[0], v[1], v[2], v[3]);
          break;
        }
        case OP.SET_VIEWPORT: {
          const x = f32(w[a]), y = f32(w[a + 1]), vw = f32(w[a + 2]), vh = f32(w[a + 3]);
          let mn = Math.min(Math.max(f32(w[a + 4]), 0), 1), mx = Math.min(Math.max(f32(w[a + 5]), 0), 1);
          if (!(vw > 0) || !(vh > 0)) break;
          // reversed (min above max): the viewport 0 to 1 and the vertex shader maps the depth (injectDepthMap)
          rec.depthMap = mn > mx ? [mn, mx] : null;
          rec.viewport = rec.depthMap ? [x, y, vw, vh, 0, 1] : [x, y, vw, vh, mn, mx];
          if (rec.pass) rec.pass.setViewport(...rec.viewport);
          break;
        }
        case OP.SET_SCISSOR: {
          rec.rawScissor = [w[a] | 0, w[a + 1] | 0, w[a + 2] | 0, w[a + 3] | 0];
          rec.scissor = rec.fb ? fbClampScissor(rec.fb, ...rec.rawScissor) : null;
          if (rec.pass && rec.scissor) rec.pass.setScissorRect(...rec.scissor);
          break;
        }
        case OP.SET_BLEND_CONSTANT: {
          rec.blendConstant = { r: f32(w[a]), g: f32(w[a + 1]), b: f32(w[a + 2]), a: f32(w[a + 3]) };
          if (rec.pass) rec.pass.setBlendConstant(rec.blendConstant);
          break;
        }
        case OP.SET_STENCIL_REF: {
          rec.stencilRef = w[a];
          if (rec.pass) rec.pass.setStencilReference(rec.stencilRef);
          break;
        }
        case OP.DRAW: {
          const pass = rec.renderPass();
          if (pass && bindForDraw(rec, pass, null)) pass.draw(w[a], w[a + 1], w[a + 2], w[a + 3]);
          traceDraw(rec, 'draw', w[a], w[a + 1]);
          break;
        }
        case OP.DRAW_INDEXED: {
          const pass = rec.renderPass();
          if (pass && rec.ibuf && bindForDraw(rec, pass, rec.ibuf[1])) pass.drawIndexed(w[a], w[a + 1], w[a + 2], w[a + 3] | 0, w[a + 4]);
          traceDraw(rec, 'drawIndexed' + (rec.ibuf ? '' : '(no index buffer)'), w[a], w[a + 1], [w[a + 2], w[a + 3] | 0, w[a + 4]]);
          break;
        }
        case OP.DISPATCH: {
          const pass = rec.computePass();
          if (rec.cpipeline) pass.dispatchWorkgroups(w[a], w[a + 1], w[a + 2]);
          break;
        }
        case OP.BARRIER:
          rec.flushClears();
          break;
        case OP.COPY_B2B: {
          rec.flushClears();
          const dst = objects[w[a]], src = objects[w[a + 3]];
          if (!dst || !src) break;
          const size = u64(w[a + 6], w[a + 7]);
          rec.enc.copyBufferToBuffer(src.gpu, u64(w[a + 4], w[a + 5]), dst.gpu, u64(w[a + 1], w[a + 2]), size);
          if (dst.readback) noteReadback(rec, dst, u64(w[a + 1], w[a + 2]), size);
          break;
        }
        case OP.COPY_B2T: {
          rec.flushClears();
          const src = objects[w[a]], tex = objects[w[a + 5]];
          if (!src || !tex) break;
          if ((w[a + 3] & 255) && src.shadow) {
            // rows WebGPU cannot copy from a buffer: from the CPU copy, between the commands before and after
            rec.split();
            device.queue.writeTexture(
              { texture: tex.gpu, mipLevel: w[a + 6], origin: { x: w[a + 7], y: w[a + 8], z: w[a + 9] }, aspect: ASPECT[w[a + 13]] },
              heap8(), { offset: src.shadow + u64(w[a + 1], w[a + 2]), bytesPerRow: w[a + 3], rowsPerImage: w[a + 4] },
              { width: w[a + 10], height: w[a + 11], depthOrArrayLayers: w[a + 12] });
            break;
          }
          if (w[a + 3] & 255) { warnOnce('b2t', 'texture copy rows of ' + w[a + 3] + ' bytes from a GPU buffer'); break; }
          rec.enc.copyBufferToTexture(
            { buffer: src.gpu, offset: u64(w[a + 1], w[a + 2]), bytesPerRow: w[a + 3], rowsPerImage: w[a + 4] },
            { texture: tex.gpu, mipLevel: w[a + 6], origin: { x: w[a + 7], y: w[a + 8], z: w[a + 9] }, aspect: ASPECT[w[a + 13]] },
            { width: w[a + 10], height: w[a + 11], depthOrArrayLayers: w[a + 12] });
          break;
        }
        case OP.COPY_T2B: {
          rec.flushClears();
          const tex = objects[w[a]], dst = objects[w[a + 9]];
          if (!tex || !dst) break;
          if (w[a + 12] & 255) { warnOnce('t2b', 'readback rows of ' + w[a + 12] + ' bytes (WebGPU copies 256-byte multiples)'); break; }
          rec.enc.copyTextureToBuffer(
            { texture: tex.gpu, mipLevel: w[a + 1], origin: { x: w[a + 2], y: w[a + 3], z: w[a + 4] }, aspect: ASPECT[w[a + 8]] },
            { buffer: dst.gpu, offset: u64(w[a + 10], w[a + 11]), bytesPerRow: w[a + 12], rowsPerImage: w[a + 13] },
            { width: w[a + 5], height: w[a + 6], depthOrArrayLayers: w[a + 7] });
          if (dst.readback) noteReadback(rec, dst, u64(w[a + 10], w[a + 11]), w[a + 12] * w[a + 13] * w[a + 7]);
          break;
        }
        case OP.COPY_T2T: {
          rec.flushClears();
          const src = objects[w[a]], dst = objects[w[a + 5]];
          if (!src || !dst) break;
          rec.enc.copyTextureToTexture(
            { texture: src.gpu, mipLevel: w[a + 1], origin: { x: w[a + 2], y: w[a + 3], z: w[a + 4] }, aspect: ASPECT[w[a + 13]] },
            { texture: dst.gpu, mipLevel: w[a + 6], origin: { x: w[a + 7], y: w[a + 8], z: w[a + 9] }, aspect: ASPECT[w[a + 13]] },
            { width: w[a + 10], height: w[a + 11], depthOrArrayLayers: w[a + 12] });
          break;
        }
        case OP.RESOLVE: {
          rec.flushClears();
          const src = objects[w[a]], dst = objects[w[a + 1]];
          if (!src || !dst) break;
          const pass = rec.enc.beginRenderPass({ colorAttachments: [{ view: src, resolveTarget: dst, loadOp: 'load', storeOp: 'store' }] });
          pass.end();
          break;
        }
        default:
          warnOnce('op' + op, 'unknown list op ' + op);
      }
    }
    rec.flushClears();
  }

  function noteReadback(rec, buf, off, size) {
    const r = rec.readbacks.get(buf.id);
    const lo = off, hi = Math.min(off + size, buf.size);
    if (!r) rec.readbacks.set(buf.id, [lo, hi]);
    else { r[0] = Math.min(r[0], lo); r[1] = Math.max(r[1], hi); }
  }

  function textureUsage(flags, format) {
    // flags (plume_webgpu.cpp): 1 render target, 2 depth target, 4 storage
    let u = GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_SRC | GPUTextureUsage.COPY_DST;
    if (flags & 3) u |= GPUTextureUsage.RENDER_ATTACHMENT;
    if (flags & 4) u |= GPUTextureUsage.STORAGE_BINDING;
    return u;
  }

  function bglEntry(w, a) {
    // binding, visibility, kind, x, y, z
    const e = { binding: w[a], visibility: w[a + 1] };
    const kind = w[a + 2];
    switch (kind) {
      case 1: e.buffer = { type: 'uniform', hasDynamicOffset: !!w[a + 3], minBindingSize: 0 }; break;
      case 2: e.buffer = { type: 'read-only-storage', hasDynamicOffset: false }; break;
      case 3: e.buffer = { type: 'storage', hasDynamicOffset: false }; break;
      case 4: e.sampler = { type: 'filtering' }; break;
      case 5: e.sampler = { type: 'non-filtering' }; break;
      case 6: e.texture = { sampleType: SAMPLE_TYPE[w[a + 3]], viewDimension: VIEW_DIM[w[a + 4]], multisampled: !!w[a + 5] }; break;
    }
    return e;
  }

  // ---- device-level ops ---------------------------------------------------------------------------------------

  // A block of commands at ptr; the client frees it once status word 11 reaches seq (pointers from JavaScript would be
  // BigInts in a wasm64 call, so the block goes back this way rather than through _free).
  function process(ptr, bytes, seq) {
    try {
      if (device) run(ptr, bytes);
    } catch (e) {
      log('server: ' + (e && e.stack || e));
    }
    status(11, seq);
  }

  function run(ptr, bytes) {
    const H = heap8();
    const w = new Uint32Array(H.buffer, ptr, bytes >> 2);
    let p = 0;
    const end = w.length;
    while (p < end) {
      const op = w[p], n = w[p + 1];
      const a = p + 2;
      p = a + n;
      switch (op) {
        case OP.CREATE_BUFFER: {
          const id = w[a], size = u64(w[a + 1], w[a + 2]), heapType = w[a + 3];
          const shadow = u64(w[a + 4], w[a + 5]);
          let usage;
          if (heapType === 2) usage = GPUBufferUsage.COPY_DST | GPUBufferUsage.COPY_SRC;
          else usage = GPUBufferUsage.COPY_DST | GPUBufferUsage.COPY_SRC | GPUBufferUsage.VERTEX | GPUBufferUsage.INDEX |
            GPUBufferUsage.UNIFORM | GPUBufferUsage.STORAGE;
          const gsize = Math.max(16, Math.ceil(size / 4) * 4);
          objects[id] = { id, gpu: device.createBuffer({ size: gsize, usage }), size, readback: heapType === 2, shadow };
          break;
        }
        case OP.DESTROY: {
          for (let k = 0; k < n; k++) {
            const id = w[a + k], o = objects[id];
            if (!o) continue;
            if (o.gpu && o.gpu.destroy && o.kind !== 'pipeline') {
              // buffers and textures: destroyed after the GPU's current work (WebGPU keeps them for submitted work)
              o.gpu.destroy();
            }
            objects[id] = undefined;
          }
          break;
        }
        case OP.CREATE_TEXTURE: {
          const id = w[a], dim = w[a + 1], format = TEX_FORMAT[w[a + 7]];
          if (!format) { warnOnce('fmt' + w[a + 7], 'texture format ' + w[a + 7] + ' has no WebGPU format'); break; }
          const desc = {
            dimension: dim === 3 ? '3d' : dim === 1 ? '1d' : '2d',
            size: { width: w[a + 2], height: w[a + 3], depthOrArrayLayers: w[a + 4] },
            mipLevelCount: w[a + 5], sampleCount: w[a + 6], format, usage: textureUsage(w[a + 8], format),
          };
          // an empty size (plume allows 0 for "unused"): 1, as the other back ends' drivers round it
          if (!desc.size.width || !desc.size.height || !desc.size.depthOrArrayLayers || !desc.mipLevelCount) {
            warnOnce('empty', `texture ${id}: empty size ${desc.size.width}x${desc.size.height}x${desc.size.depthOrArrayLayers} mips ${desc.mipLevelCount}, made 1`);
            desc.size.width = Math.max(1, desc.size.width); desc.size.height = Math.max(1, desc.size.height);
            desc.size.depthOrArrayLayers = Math.max(1, desc.size.depthOrArrayLayers); desc.mipLevelCount = Math.max(1, desc.mipLevelCount);
          }
          if (desc.dimension === '1d') { desc.dimension = '2d'; }
          try {
            objects[id] = { id, gpu: device.createTexture(desc), format, desc };
          } catch (e) {
            warnOnce('tex', 'createTexture: ' + e.message);
          }
          break;
        }
        case OP.CREATE_VIEW: {
          const id = w[a], tex = objects[w[a + 1]];
          if (!tex) break;
          const fmt = TEX_FORMAT[w[a + 2]];
          const desc = { dimension: VIEW_DIM[w[a + 3]], baseMipLevel: w[a + 4], mipLevelCount: w[a + 5], baseArrayLayer: w[a + 6],
            arrayLayerCount: w[a + 7], aspect: ASPECT[w[a + 8]] };
          // a view format other than the texture's: only its sRGB twin is allowed (the aspect picks depth/stencil)
          if (fmt && fmt === tex.format) desc.format = fmt;
          objects[id] = tex.gpu.createView(desc);
          break;
        }
        case OP.CREATE_SAMPLER: {
          const id = w[a];
          const desc = {
            minFilter: FILTER[w[a + 1]] || 'linear', magFilter: FILTER[w[a + 2]] || 'linear', mipmapFilter: FILTER[w[a + 3]] || 'linear',
            addressModeU: ADDRESS[w[a + 4]] || 'repeat', addressModeV: ADDRESS[w[a + 5]] || 'repeat', addressModeW: ADDRESS[w[a + 6]] || 'repeat',
            lodMinClamp: f32(w[a + 7]), lodMaxClamp: Math.min(f32(w[a + 8]), 32),
          };
          if (w[a + 9]) desc.compare = COMPARE[w[a + 9]];
          const an = w[a + 10];
          if (an > 1 && desc.minFilter === 'linear' && desc.magFilter === 'linear' && desc.mipmapFilter === 'linear') desc.maxAnisotropy = Math.min(16, an);
          objects[id] = device.createSampler(desc);
          break;
        }
        case OP.CREATE_SHADER: {
          const id = w[a], stage = w[a + 1];
          const text = textOf(w, a + 3, w[a + 2]);
          objects[id] = { id, stage, text, ids: overrideIds(text), label: 'shader ' + id, outputs: stage === 4 ? fragmentOutputs(text) : null };
          break;
        }
        case OP.CREATE_BGL: {
          const id = w[a], count = w[a + 1];
          const entries = [];
          for (let k = 0; k < count; k++) entries.push(bglEntry(w, a + 2 + k * 6));
          try {
            objects[id] = device.createBindGroupLayout({ entries });
          } catch (e) { warnOnce('bgl', 'createBindGroupLayout: ' + e.message); }
          break;
        }
        case OP.CREATE_PIPELINE_LAYOUT: {
          const id = w[a], count = w[a + 1];
          const bindGroupLayouts = [];
          for (let k = 0; k < count; k++) bindGroupLayouts.push(objects[w[a + 2 + k]]);
          objects[id] = device.createPipelineLayout({ bindGroupLayouts });
          break;
        }
        case OP.CREATE_RENDER_PIPELINE:
          createRenderPipeline(w, a);
          break;
        case OP.CREATE_COMPUTE_PIPELINE: {
          const id = w[a], layout = objects[w[a + 1]], cs = objects[w[a + 2]], nc = w[a + 3];
          const constants = [];
          for (let k = 0; k < nc; k++) constants.push([w[a + 4 + k * 2], w[a + 5 + k * 2]]);
          if (!cs || !layout) break;
          try {
            objects[id] = { kind: 'pipeline', gpu: device.createComputePipeline({
              layout, label: cs.label,
              compute: { module: shaderModule(cs.text, cs.label), entryPoint: 'main', constants: constantsFor(cs.ids, constants) },
            }) };
          } catch (e) { warnOnce('cpipe', 'createComputePipeline: ' + e.message); }
          break;
        }
        case OP.CREATE_BIND_GROUP: {
          const id = w[a], layout = objects[w[a + 1]], count = w[a + 2];
          const entries = [];
          let ok = !!layout;
          for (let k = 0; k < count; k++) {
            const b = a + 3 + k * 5;
            const kind = w[b + 1], obj = objects[w[b + 2]];
            if (!obj) { ok = false; break; }
            if (kind === 1) {
              const size = w[b + 4];
              entries.push({ binding: w[b], resource: { buffer: obj.gpu, offset: w[b + 3], size: size || undefined }, bufferId: w[b + 2] });
            } else {
              entries.push({ binding: w[b], resource: obj });
            }
          }
          if (ok) {
            objects[id] = device.createBindGroup({ layout, entries: entries.map(({ binding, resource }) => ({ binding, resource })) });
            // what it binds, for the trace: binding:buffer@offset+size
            objects[id].traceText = entries.filter((e) => e.bufferId).map((e) => e.binding + ':#' + e.bufferId + '@' + e.resource.offset + '+' + (e.resource.size || 0)).join(' ');
            const ubo0 = entries.find((e) => e.bufferId && e.binding === 0);
            if (ubo0) objects[id].traceUbo0 = [ubo0.resource.buffer, ubo0.resource.offset];
            const ubo2 = entries.find((e) => e.bufferId && e.binding === 2);
            if (ubo2) objects[id].traceUbo2 = [ubo2.resource.buffer, ubo2.resource.offset];
          }
          break;
        }
        case OP.WRITE_BUFFER: {
          const b = objects[w[a]];
          if (!b) break;
          const off = u64(w[a + 1], w[a + 2]), src = u64(w[a + 3], w[a + 4]), size = w[a + 5];
          device.queue.writeBuffer(b.gpu, off, heap8(), src, size);
          break;
        }
        case OP.WRITE_TEXTURE: {
          const tex = objects[w[a]];
          if (!tex) break;
          device.queue.writeTexture(
            { texture: tex.gpu, mipLevel: w[a + 1], origin: { x: w[a + 2], y: w[a + 3], z: w[a + 4] }, aspect: ASPECT[w[a + 13]] },
            heap8(), { offset: u64(w[a + 8], w[a + 9]), bytesPerRow: w[a + 10], rowsPerImage: w[a + 11] },
            { width: w[a + 5], height: w[a + 6], depthOrArrayLayers: w[a + 7] });
          break;
        }
        case OP.CREATE_FRAMEBUFFER: {
          const id = w[a], cc = w[a + 1];
          const colors = [], colorFormats = [];
          for (let k = 0; k < cc; k++) {
            colors.push(objects[w[a + 2 + k * 2]]);
            colorFormats.push(TEX_FORMAT[w[a + 3 + k * 2]]);
          }
          const b = a + 2 + cc * 2;
          const depth = w[b] ? objects[w[b]] : null;
          const depthFormat = w[b] ? TEX_FORMAT[w[b + 1]] : null;
          const fb = {
            colors, colorFormats, depth, depthFormat, depthReadOnly: !!w[b + 2], width: w[b + 3], height: w[b + 4], samples: w[b + 5] || 1,
            hasDepth: !!depthFormat && depthFormat !== 'stencil8',
            hasStencil: !!depthFormat && (depthFormat.includes('stencil')),
          };
          fb.key = colorFormats.join(',') + '/' + depthFormat + '/' + fb.samples + '/' + fb.depthReadOnly;
          objects[id] = fb;
          break;
        }
        case OP.SUBMIT:
          submit(w, a, a + n);
          break;
        case OP.PRESENT: {
          const tex = objects[w[a]];
          present(tex);
          break;
        }
        case OP.SIGNAL: {
          const addr = u64(w[a], w[a + 1]), value = w[a + 2];
          device.queue.onSubmittedWorkDone().then(() => {
            const h = heap32();
            Atomics.store(h, addr / 4, value);
            Atomics.notify(h, addr / 4);
          });
          break;
        }
        case OP.LOG:
          log(textOf(w, a + 1, w[a]));
          break;
        default:
          warnOnce('dop' + op, 'unknown op ' + op);
          return;
      }
    }
  }

  function createRenderPipeline(w, a) {
    // id, layout, vs, ps, topology, cull, frontFace, depthFormat, depthFlags (1 test, 2 write), depthCompare,
    // stencilFlags (1 enabled), stencilRead, stencilWrite, front {fail, depthFail, pass, compare}, back {...},
    // depthBias (i32), slope f32, clamp f32, samples, alphaToCoverage, colorCount,
    //   per target: format, blendEnabled, srcC, dstC, opC, srcA, dstA, opA, mask
    // bufferCount, per buffer: stride, stepMode, attrCount, per attr: format, offset, location
    // constantCount, per constant: id, value(u32)
    let k = a;
    const id = w[k++], layout = objects[w[k++]], vs = objects[w[k++]], psId = w[k++];
    const ps = psId ? objects[psId] : null;
    const topology = TOPOLOGY[w[k++]], cull = CULL[w[k++]] || 'none', front = w[k++] === 2 ? 'ccw' : 'cw';
    const depthFormat = TEX_FORMAT[w[k++]], depthFlags = w[k++], depthCompare = COMPARE[w[k++]] || 'always';
    const stencilFlags = w[k++], stencilRead = w[k++], stencilWrite = w[k++];
    const sf = { failOp: STENCIL_OP[w[k++]], depthFailOp: STENCIL_OP[w[k++]], passOp: STENCIL_OP[w[k++]], compare: COMPARE[w[k++]] };
    const sb = { failOp: STENCIL_OP[w[k++]], depthFailOp: STENCIL_OP[w[k++]], passOp: STENCIL_OP[w[k++]], compare: COMPARE[w[k++]] };
    const depthBias = w[k++] | 0, slope = f32(w[k++]), clamp = f32(w[k++]);
    const samples = w[k++] || 1, a2c = !!w[k++];
    const colorCount = w[k++];
    const targets = [];
    for (let t = 0; t < colorCount; t++) {
      const fmt = TEX_FORMAT[w[k++]];
      const be = w[k++];
      const sc = BLEND[w[k++]], dc = BLEND[w[k++]], oc = BLEND_OP[w[k++]], sa = BLEND[w[k++]], da = BLEND[w[k++]], oa = BLEND_OP[w[k++]];
      let mask = w[k++];
      // a target the fragment shader does not write takes nothing (WebGPU refuses a pipeline that would write it)
      if (!ps || (ps.outputs && !ps.outputs.has(t))) mask = 0;
      const target = { format: fmt, writeMask: mask };
      if (be) {
        const fix = (op, s, d) => (op === 'min' || op === 'max') ? ['one', 'one'] : [s || 'one', d || 'zero'];
        const [s1, d1] = fix(oc, sc, dc), [s2, d2] = fix(oa, sa, da);
        target.blend = { color: { operation: oc || 'add', srcFactor: s1, dstFactor: d1 }, alpha: { operation: oa || 'add', srcFactor: s2, dstFactor: d2 } };
      }
      targets.push(target);
    }
    const bufferCount = w[k++];
    const buffers = [];
    for (let b = 0; b < bufferCount; b++) {
      const stride = w[k++], step = w[k++] ? 'instance' : 'vertex', ac = w[k++];
      const attributes = [];
      for (let t = 0; t < ac; t++) {
        const fmt = w[k++], offset = w[k++], location = w[k++];
        const f = VTX_FORMAT[fmt];
        if (!f) { warnOnce('vfmt' + fmt, 'vertex format ' + fmt + ' has no WebGPU format'); continue; }
        attributes.push({ format: f, offset, shaderLocation: location });
      }
      buffers.push({ arrayStride: stride, stepMode: step, attributes });
    }
    const nc = w[k++];
    const constants = [];
    for (let t = 0; t < nc; t++) constants.push([w[k++], w[k++]]);
    if (!vs || !layout) return;
    let vsText = vs.text, psText = ps ? ps.text : null;
    if (ps) {
      const key = vs.id + ':' + ps.id;
      createRenderPipeline.links = createRenderPipeline.links || new Map();
      let linked = createRenderPipeline.links.get(key);
      if (!linked) {
        linked = linkStages(vs.text, ps.text);
        createRenderPipeline.links.set(key, linked);
      }
      vsText = linked.vs;
      psText = linked.ps;
    } else {
      // no fragment stage: the vertex outputs must still fit (the position only)
      vsText = linkStages(vs.text, '@fragment\nfn main() {}').vs;
    }
    const mapped = injectDepthMap(vsText);
    if (mapped) vsText = mapped;
    else warnOnce('depthmap', `${vs.label}: no position return to map the depth of (a reversed depth range draws at depth 1)`);
    const desc = {
      layout, label: vs.label + (ps ? ' + ' + ps.label : ''),
      vertex: { module: shaderModule(vsText, vs.label), entryPoint: 'main', buffers, constants: constantsFor(vs.ids, constants) },
      primitive: { topology, cullMode: cull, frontFace: front },
      multisample: { count: samples, alphaToCoverageEnabled: a2c },
    };
    if (ps) {
      desc.fragment = { module: shaderModule(psText, ps.label), entryPoint: 'main', targets, constants: constantsFor(ps.ids, constants) };
    } else if (targets.length) {
      // WebGPU needs a fragment stage for colour targets; without one nothing is written (writeMask 0).
    }
    if (depthFormat) {
      const ds = { format: depthFormat, depthWriteEnabled: !!(depthFlags & 2), depthCompare: (depthFlags & 1) ? depthCompare : 'always' };
      const hasStencil = depthFormat.includes('stencil');
      if (hasStencil && (stencilFlags & 1)) {
        ds.stencilFront = sf;
        ds.stencilBack = sb;
        ds.stencilReadMask = stencilRead & 0xFF;
        ds.stencilWriteMask = stencilWrite & 0xFF;
      }
      if (depthBias || slope) { ds.depthBias = depthBias; ds.depthBiasSlopeScale = slope; ds.depthBiasClamp = clamp; }
      desc.depthStencil = ds;
    }
    try {
      device.pushErrorScope('validation');
      const gpu = device.createRenderPipeline(desc);
      const o = { kind: 'pipeline', gpu, desc, label: desc.label, targets: targets.map((t) => t.format + (t.blend ? '+b' : '') + ':' + t.writeMask),
        ds: desc.depthStencil ? desc.depthStencil.depthCompare + (desc.depthStencil.depthWriteEnabled ? '+w' : '') +
          (desc.depthStencil.stencilFront ? ' st:' + desc.depthStencil.stencilFront.compare : '') : '', topo: desc.primitive.topology,
        shaders: [vs, ps], depthMapped: !!mapped };  // their WGSL, for WebGpuServer.inspect
      objects[id] = o;
      device.popErrorScope().then((err) => {
        if (err) {
          warnOnce('pipe:' + err.message.slice(0, 60), `pipeline ${desc.label}: ${err.message}`);
          o.gpu = null;
        }
      });
    } catch (e) {
      warnOnce('pipe', 'createRenderPipeline: ' + e.message);
    }
  }

  // Readback buffers are GPU buffers; after a submit that copies into one, its range goes to a mappable staging
  // buffer (a readback buffer itself can be copied into again while a map of it is pending) and from there to the
  // CPU copy plume_webgpu's map() returns, before the fence is set.
  const stagingPool = [];
  function staging(size) {
    for (let k = 0; k < stagingPool.length; k++) {
      if (stagingPool[k].size >= size) return stagingPool.splice(k, 1)[0];
    }
    let s = 1 << 16;
    while (s < size) s *= 2;
    return device.createBuffer({ size: s, usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST });
  }

  function submit(w, a, end) {
    // SUBMIT: fenceLo, fenceHi, fenceValue, listCount, per list: words, then the list's words
    const fenceAddr = u64(w[a], w[a + 1]), fenceValue = w[a + 2], count = w[a + 3];
    let p = a + 4;
    const rec = new Recorder();
    for (let i = 0; i < count; i++) {
      const n = w[p];
      recordList(rec, w, p + 1, p + 1 + n);
      p += 1 + n;
      // state does not carry from one plume list to the next
      rec.endPass();
      rec.fb = null;
      rec.pipelineObj = null;
      rec.groups = [null, null, null, null];
      rec.cgroups = [null, null, null, null];
      rec.vbufs = [];
      rec.ibuf = null;
      rec.viewport = rec.scissor = rec.rawScissor = null;
      rec.blendConstant = null;
    }
    rec.writePush();
    if (traceLeft && rec.traceLines) {
      traceLeft--;
      log('trace: submit of ' + count + ' lists, ' + (rec.traceSeen || 0) + ' draws, ' + rec.traceLines.length +
        ' shown\n  ' + rec.traceLines.join('\n  '));
      rec.traceSeen = 0;
    }
    const copies = [];
    for (const [id, range] of rec.readbacks) {
      const b = objects[id];
      if (!b || !b.shadow) continue;
      const lo = Math.floor(range[0] / 4) * 4, hi = Math.min(b.gpu.size, Math.ceil(range[1] / 4) * 4);
      if (hi <= lo) continue;
      const st = staging(hi - lo);
      rec.enc.copyBufferToBuffer(b.gpu, lo, st, 0, hi - lo);
      copies.push([b, st, lo, hi]);
    }
    let caps = null;
    if (rec.traceCaps && rec.traceCaps.length) {
      let at = 0;
      for (const c of rec.traceCaps) { c.push(at); at += c[3]; }
      caps = [rec.traceCaps, device.createBuffer({ size: at, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ })];
      rec.traceCaps.forEach(([, buf, off, size, to]) => rec.enc.copyBufferToBuffer(buf, off, caps[1], to, size));
      rec.traceCaps = null;
    }
    let vcaps = null;
    if (rec.traceVbs && rec.traceVbs.length) {
      let at = 0;
      for (const c of rec.traceVbs) { c.push(at); at += c[3]; }
      vcaps = [rec.traceVbs, device.createBuffer({ size: at, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ })];
      rec.traceVbs.forEach(([, buf, off, size, to]) => rec.enc.copyBufferToBuffer(buf, off, vcaps[1], to, size));
      rec.traceVbs = null;
    }
    device.queue.submit([rec.enc.finish()]);
    if (vcaps) {
      vcaps[1].mapAsync(GPUMapMode.READ).then(() => {
        const all = vcaps[1].getMappedRange();
        traceVertexData = vcaps[0].map(([line, , off, size, to]) => [line, off, all.slice(to, to + size)]);
        vcaps[1].destroy();
      });
    }
    if (caps) {
      caps[1].mapAsync(GPUMapMode.READ).then(() => {
        const f = new Float32Array(caps[1].getMappedRange());
        // a filtered trace (trace(n, cap, filter)) has each draw's 256 vertex constants: shown are c0-c4, c15-c16,
        // c31-c48 (Oblivion's first six bones) and c255; WebGpuServer.traceConstants() keeps them all
        const regs = (k, list) => list.map((r) => 'c' + r + '=' +
          Array.from(f.subarray(k + r * 4, k + r * 4 + 4), (v) => +v.toFixed(3)).join(',')).join(' ');
        const full = [0, 1, 2, 3, 4, 15, 16, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 255];
        traceConstants = caps[0].map(([line, , , size, to]) => [line, f.slice(to / 4, (to + size) / 4)]);
        log('trace constants (draw: ' + (caps[0][0][3] > 64 ? 'registers' : 'c0..c3') + '):\n  ' +
          caps[0].map(([line, , , size, to]) => line + ': ' + (size > 64 ? regs(to / 4, full) :
            Array.from(f.subarray(to / 4, to / 4 + 16), (v) => +v.toFixed(3)).join(','))).join('\n  '));
        caps[1].destroy();
      });
    }
    const waits = [device.queue.onSubmittedWorkDone()];
    for (const [b, st, lo, hi] of copies) {
      waits.push(st.mapAsync(GPUMapMode.READ, 0, hi - lo).then(() => {
        heap8().set(new Uint8Array(st.getMappedRange(0, hi - lo)), b.shadow + lo);
        st.unmap();
        stagingPool.push(st);
      }, (e) => warnOnce('map', 'readback map: ' + e.message)));
    }
    if (fenceAddr) {
      Promise.all(waits).then(() => {
        const h = heap32();
        Atomics.store(h, fenceAddr / 4, fenceValue);
        Atomics.notify(h, fenceAddr / 4);
      });
    }
  }

  function present(tex) {
    presentCount++;
    // SDL sizes the canvas to its window (2560x1440) when it makes it; the page's layout decides.
    if (sizeCanvas && (canvas.width !== sizeCanvas.w || canvas.height !== sizeCanvas.h)) sizeCanvas();
    if (tex && tex.gpu) {
      let group = blit.groups.get(tex.id);
      if (!group) {
        group = device.createBindGroup({ layout: blit.pipeline.getBindGroupLayout(0), entries: [
          { binding: 0, resource: tex.gpu.createView() }, { binding: 1, resource: blit.sampler }] });
        blit.groups.set(tex.id, group);
        if (blit.groups.size > 16) blit.groups.delete(blit.groups.keys().next().value);
      }
      const enc = device.createCommandEncoder();
      const pass = enc.beginRenderPass({ colorAttachments: [{ view: context.getCurrentTexture().createView(), loadOp: 'clear',
        clearValue: { r: 0, g: 0, b: 0, a: 1 }, storeOp: 'store' }] });
      pass.setPipeline(blit.pipeline);
      pass.setBindGroup(0, group);
      pass.draw(3);
      pass.end();
      device.queue.submit([enc.finish()]);
    }
    status(4, presentCount);
  }

  return {
    init: (addr, selectorPtr) => init(addr, UTF8ToString(selectorPtr)),
    process,
    // for tests (web/plume/test_link.js)
    linkStages: (vs, ps) => linkStages(vs, ps),
    trace: (n, cap, filter) => { traceLeft = n || 1; traceCap = cap || 200; traceFilter = filter || ''; },
    traceConstants: () => traceConstants,
    traceVertices: () => traceVertexData,
    debug: (mode) => { debugMode = mode || ''; },
    inspect: (id) => objects[id],  // a server object by id, from the page's console
    // A buffer's bytes (a promise of an ArrayBuffer), from the page's console: what a draw read.
    readBuffer: async (id, offset, size) => {
      const b = objects[id];
      const n = (size + 3) & ~3;
      const st = device.createBuffer({ size: n, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
      const e = device.createCommandEncoder();
      e.copyBufferToBuffer(b.gpu, offset, st, 0, n);
      device.queue.submit([e.finish()]);
      await st.mapAsync(GPUMapMode.READ);
      const r = st.getMappedRange().slice(0);
      st.destroy();
      return r;
    },
    fragmentOutputs: (ps) => fragmentOutputs(ps),
  };
})();
