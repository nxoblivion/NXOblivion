// node web/plume/test_link.js <wgsl dir>: links every vertex shader with a few pixel shaders (webgpu_server.js's
// linkStages) and checks the result's shape: at most 16 interpolators, the locations 0..n-1 on both sides, the vertex
// constructor's argument count matching its struct.
const fs = require('fs'), path = require('path');
global.wasmMemory = { buffer: new ArrayBuffer(8) };
const src = fs.readFileSync(path.join(__dirname, 'webgpu_server.js'), 'utf8');
const S = new Function(src + '\nreturn WebGpuServer;')();
const dir = process.argv[2];
const files = fs.readdirSync(dir);
const vs = files.filter((f) => f.startsWith('vs_')).map((f) => fs.readFileSync(path.join(dir, f), 'utf8'));
const ps = files.filter((f) => f.startsWith('ps_')).map((f) => fs.readFileSync(path.join(dir, f), 'utf8'));
let bad = 0, n = 0;
for (let i = 0; i < vs.length; i++) {
  for (let j = 0; j < 4; j++) {
    const p = ps[(i * 7 + j * 31) % ps.length];
    const out = S.linkStages(vs[i], p);
    n++;
    const st = /struct tint_symbol \{\n([\s\S]*?)\n\}/.exec(out.vs);
    const locs = st ? [...st[1].matchAll(/@location\((\d+)\)/g)].map((m) => +m[1]) : [];
    const ret = /return tint_symbol\(([^;]*)\);/.exec(out.vs);
    const members = st ? st[1].split('\n').filter((l) => /^\s*\w+ : /.test(l)).length : -1;
    const args = ret ? ret[1].split(',').length : -1;
    const fsl = [...(/@fragment\s*\nfn main\(([\s\S]*?)\) ->/.exec(out.ps) || ['', ''])[1].matchAll(/@location\((\d+)u?\)/g)].map((m) => +m[1]);
    const ok = locs.length <= 16 && locs.every((l, k) => l === k) && members === args && [...fsl].sort((a, b) => a - b).every((l, k) => l === k) &&
      fsl.length === locs.length;
    if (!ok) {
      if (bad++ < 3) console.log('bad', i, j, { locs, members, args, fsl });
    }
  }
}
console.log(`${n} links, ${bad} bad`);
const o = S.fragmentOutputs(ps[0]);
console.log('outputs of ps 0', [...o]);
