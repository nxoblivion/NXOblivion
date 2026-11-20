// reOblivion web dev server: static files with the cross-origin isolation headers that wasm threads
// (SharedArrayBuffer) need, Range requests (the game's files are read in pieces), HTTP and HTTPS.
//
// Usage: node serve.js [--root DIR] [--port 8650] [--mount /game=D:/path/to/extracted/disc]
//   HTTP on --port (localhost is a secure context), HTTPS on --port+1 with probe/cert (LAN, the fridge).
// Mounts map a URL prefix onto a folder outside the root (the game's own files: never copied into the site).
const http = require('http');
const https = require('https');
const fs = require('fs');
const path = require('path');

const args = process.argv.slice(2);
let root = __dirname, port = 8650;
const mounts = [];
for (let i = 0; i < args.length; i++) {
  if (args[i] === '--root') root = path.resolve(args[++i]);
  else if (args[i] === '--port') port = +args[++i];
  else if (args[i] === '--mount') {
    const [prefix, dir] = args[++i].split(/=(.*)/s);
    mounts.push({ prefix: prefix.replace(/\/$/, ''), dir: path.resolve(dir) });
  }
}

const types = {
  '.html': 'text/html; charset=utf-8', '.js': 'text/javascript', '.mjs': 'text/javascript',
  '.wasm': 'application/wasm', '.json': 'application/json', '.css': 'text/css', '.png': 'image/png',
  '.svg': 'image/svg+xml', '.txt': 'text/plain; charset=utf-8', '.data': 'application/octet-stream',
};

function resolve(urlPath) {
  for (const m of mounts) {
    if (urlPath === m.prefix || urlPath.startsWith(m.prefix + '/')) return path.join(m.dir, urlPath.slice(m.prefix.length));
  }
  return path.join(root, urlPath);
}

function handler(req, res) {
  res.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
  res.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');
  res.setHeader('Cross-Origin-Resource-Policy', 'same-origin');
  res.setHeader('Cache-Control', 'no-cache');
  let urlPath;
  try { urlPath = decodeURIComponent(new URL(req.url, 'http://x').pathname); } catch { res.statusCode = 400; return res.end(); }
  if (urlPath.includes('\0')) { res.statusCode = 400; return res.end(); }
  let file = resolve(path.posix.normalize(urlPath));
  for (const m of mounts) {
    if (urlPath === m.prefix + '/.manifest') {
      // Every file under the mount, one "<size> <path>" line each (the game's /game tree, web_game_files.cpp).
      const out = [];
      const walk = (dir, rel) => {
        for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
          const full = path.join(dir, e.name), r = rel ? rel + '/' + e.name : e.name;
          const s = fs.statSync(full);
          if (s.isDirectory()) walk(full, r); else out.push(`${s.size} ${r}`);
        }
      };
      walk(m.dir, '');
      res.setHeader('Content-Type', 'text/plain; charset=utf-8');
      return res.end(out.join('\n') + '\n');
    }
  }
  let st;
  try { st = fs.statSync(file); } catch { res.statusCode = 404; return res.end('not found'); }
  if (st.isDirectory()) {
    if (req.headers['x-list'] === '1' || urlPath.endsWith('/.list')) {
      // Directory listing as JSON (the game's file index is built from these).
      const entries = fs.readdirSync(file, { withFileTypes: true }).map((e) => {
        const s = fs.statSync(path.join(file, e.name));
        return { name: e.name, dir: e.isDirectory(), size: s.size };
      });
      res.setHeader('Content-Type', 'application/json');
      return res.end(JSON.stringify(entries));
    }
    file = path.join(file, 'index.html');
    try { st = fs.statSync(file); } catch { res.statusCode = 404; return res.end('not found'); }
  }
  res.setHeader('Content-Type', types[path.extname(file).toLowerCase()] || 'application/octet-stream');
  res.setHeader('Accept-Ranges', 'bytes');
  const range = req.headers.range && /^bytes=(\d*)-(\d*)$/.exec(req.headers.range);
  if (range) {
    let start = range[1] === '' ? st.size - +range[2] : +range[1];
    let end = range[1] === '' || range[2] === '' ? st.size - 1 : Math.min(+range[2], st.size - 1);
    if (start > end || start >= st.size) { res.statusCode = 416; res.setHeader('Content-Range', `bytes */${st.size}`); return res.end(); }
    res.statusCode = 206;
    res.setHeader('Content-Range', `bytes ${start}-${end}/${st.size}`);
    res.setHeader('Content-Length', end - start + 1);
    if (req.method === 'HEAD') return res.end();
    return fs.createReadStream(file, { start, end }).pipe(res);
  }
  res.setHeader('Content-Length', st.size);
  if (req.method === 'HEAD') return res.end();
  fs.createReadStream(file).pipe(res);
}

// Long-lived connections and no time limit on a response: over a VPN the 69 MB wasm and the game's many range reads are
// slow, and Node's defaults (an idle connection closed after 5 s, a request cut after 5 minutes) made the browser retry
// requests on closed connections until it gave up ("Too many retries", 2026-11-19).
const relax = (server) => {
  server.keepAliveTimeout = 120000;
  server.headersTimeout = 125000;
  server.requestTimeout = 0;
  server.timeout = 0;
  return server;
};
relax(http.createServer(handler)).listen(port, '0.0.0.0', () => console.log(`http://localhost:${port}/ (root ${root})`));
const cert = path.join(__dirname, 'probe', 'cert');
if (fs.existsSync(path.join(cert, 'cert.pem'))) {
  relax(https.createServer({ key: fs.readFileSync(path.join(cert, 'key.pem')), cert: fs.readFileSync(path.join(cert, 'cert.pem')) }, handler))
    .listen(port + 1, '0.0.0.0', () => console.log(`https://<lan-ip>:${port + 1}/`));
}
for (const m of mounts) console.log(`mount ${m.prefix} -> ${m.dir}`);
