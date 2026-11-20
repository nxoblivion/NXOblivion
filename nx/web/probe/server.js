// Serves the probe page on the LAN and saves each POST /report to reports/<time>.json.
// HTTP on <port>, HTTPS (self-signed cert/, cross-origin isolated) on <port+1>.
// Usage: node server.js [port]
const http = require('http');
const https = require('https');
const fs = require('fs');
const path = require('path');
const port = +process.argv[2] || 8642;
const dir = __dirname;
fs.mkdirSync(path.join(dir, 'reports'), { recursive: true });

function handler(req, res) {
  // Cross-origin isolation: what SharedArrayBuffer (wasm threads) needs.
  res.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
  res.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');
  if (req.method === 'POST' && req.url === '/report') {
    let body = '';
    req.on('data', (d) => { body += d; if (body.length > 1e6) req.destroy(); });
    req.on('end', () => {
      const name = new Date().toISOString().replace(/[:.]/g, '-') + '.json';
      fs.writeFileSync(path.join(dir, 'reports', name), body);
      console.log('report from', req.socket.remoteAddress, '->', name);
      res.end('ok');
    });
    return;
  }
  res.setHeader('Content-Type', 'text/html; charset=utf-8');
  res.end(fs.readFileSync(path.join(dir, 'index.html')));
}

http.createServer(handler).listen(port, '0.0.0.0', () => console.log('http probe on port', port));
const certDir = path.join(dir, 'cert');
if (fs.existsSync(path.join(certDir, 'cert.pem'))) {
  https.createServer({
    key: fs.readFileSync(path.join(certDir, 'key.pem')),
    cert: fs.readFileSync(path.join(certDir, 'cert.pem')),
  }, handler).listen(port + 1, '0.0.0.0', () => console.log('https probe on port', port + 1));
}
