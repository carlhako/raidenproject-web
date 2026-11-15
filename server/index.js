// Static file server (with cross-origin isolation headers) + lobby / spectate / drop-in co-op relay.
// Each game is a session; the server keeps its authoritative input log so late watchers can replay it.
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { fileURLToPath } from 'node:url';
import { WebSocketServer } from 'ws';

const here = path.dirname(fileURLToPath(import.meta.url));
const PORT = +(process.env.PORT || 8080);
const HOST = process.env.HOST || '0.0.0.0';
const ROOT = path.resolve(process.env.DIST_DIR || path.join(here, '..', 'dist'));
const ACCESS_KEY = process.env.ACCESS_KEY || '';
const MAX_SESSIONS = 60, MAX_FRAMES = 1 << 19, JOIN_MARGIN = 90;

const TYPES = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.mjs': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8', '.wasm': 'application/wasm', '.json': 'application/json', '.img': 'application/octet-stream',
  '.png': 'image/png', '.svg': 'image/svg+xml', '.ico': 'image/x-icon' };

function authorized(req) {
  if (!ACCESS_KEY) return true;
  const h = req.headers.authorization || '';
  if (!h.startsWith('Basic ')) return false;
  const pass = Buffer.from(h.slice(6), 'base64').toString().split(':').slice(1).join(':');
  const a = crypto.createHash('sha256').update(pass).digest(), b = crypto.createHash('sha256').update(ACCESS_KEY).digest();
  return crypto.timingSafeEqual(a, b);
}

function isolation(res) {
  res.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
  res.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');
  res.setHeader('Cross-Origin-Resource-Policy', 'same-origin');
  res.setHeader('X-Content-Type-Options', 'nosniff');
}

const server = http.createServer((req, res) => {
  isolation(res);
  if (req.url === '/healthz') { res.writeHead(200); return res.end('ok'); }
  if (!authorized(req)) { res.writeHead(401, { 'WWW-Authenticate': 'Basic realm="Raiden II"' }); return res.end('Authentication required'); }
  if (req.method !== 'GET' && req.method !== 'HEAD') { res.writeHead(405); return res.end(); }
  let rel;
  try { rel = decodeURIComponent(new URL(req.url, 'http://x').pathname); } catch { res.writeHead(400); return res.end(); }
  if (rel.endsWith('/')) rel += 'index.html';
  const file = path.join(ROOT, path.normalize(rel));
  if (file !== ROOT && !file.startsWith(ROOT + path.sep)) { res.writeHead(403); return res.end(); }
  fs.stat(file, (err, st) => {
    if (err || !st.isFile()) { res.writeHead(404); return res.end('Not found'); }
    const ext = path.extname(file);
    const headers = { 'Content-Type': TYPES[ext] || 'application/octet-stream', 'Accept-Ranges': 'bytes',
      'Cache-Control': ext === '.img' ? 'public, max-age=86400' : 'no-cache' };
    let start = 0, end = st.size - 1, status = 200;
    const m = /^bytes=(\d*)-(\d*)$/.exec(req.headers.range || '');
    if (m && (m[1] || m[2])) {
      if (m[1]) { start = +m[1]; if (m[2]) end = Math.min(+m[2], end); } else { start = Math.max(0, st.size - +m[2]); }
      if (start > end || start >= st.size) { res.writeHead(416, { 'Content-Range': `bytes */${st.size}` }); return res.end(); }
      status = 206; headers['Content-Range'] = `bytes ${start}-${end}/${st.size}`;
    }
    headers['Content-Length'] = end - start + 1;
    res.writeHead(status, headers);
    if (req.method === 'HEAD') return res.end();
    fs.createReadStream(file, { start, end }).pipe(res);
  });
});

// ---------------------------------------------------------------- sessions
// A session is one running Raiden II instance owned by its host. The server keeps the per-frame input log
// (p1 | p2 << 16, active-low pad words). Frames before `lockFrom` are the host playing alone (its inputs are
// appended as they arrive). From `lockFrom` the host and player 2 run in lockstep: the server merges both
// inputs for each frame and broadcasts the confirmed frame to everybody (host, P2, watchers).
const sessions = new Map();
const clients = new Set();
const send = (ws, o) => { if (ws && ws.readyState === 1) ws.send(JSON.stringify(o)); };
const sid = () => crypto.randomBytes(4).toString('hex');
const cleanName = (n) => String(n || '').replace(/[^\w .\-]/g, '').trim().slice(0, 16) || 'Player';
const lobbyChat = [];

function frameMsg(start, vals) {                       // [u32 start][u32 count][u32 x count]
  const b = Buffer.alloc(8 + vals.length * 4); b.writeUInt32LE(start, 0); b.writeUInt32LE(vals.length, 4);
  for (let i = 0; i < vals.length; i++) b.writeUInt32LE(vals[i] >>> 0, 8 + i * 4);
  return b;
}
const members = (s) => [s.host, s.p2, ...s.watchers].filter(Boolean);
const bcast = (s, o, except) => members(s).forEach(w => { if (w !== except) send(w, o); });
const bcastBin = (s, buf, except) => members(s).forEach(w => { if (w !== except && w.readyState === 1) w.send(buf, { binary: true }); });

function lobbyList() {
  const now = Date.now();
  return [...sessions.values()].map(s => ({ id: s.id, host: s.host.name, p2: s.p2 ? s.p2.name : null, watchers: s.watchers.size,
    secs: Math.round((now - s.started) / 1000), frames: s.n, locked: s.lockFrom >= 0 }));
}
let lobbyTimer = null;
function lobbyDirty() {
  if (lobbyTimer) return;
  lobbyTimer = setTimeout(() => {
    lobbyTimer = null; const m = { t: 'lobby', sessions: lobbyList(), online: clients.size };
    for (const c of clients) if (!c.session) send(c, m);
  }, 150);
}

function append(s, v) {
  if (s.n >= MAX_FRAMES) return endSession(s, 'This game reached the maximum length.');
  if (s.n >= s.log.length) { const g = new Uint32Array(s.log.length * 2); g.set(s.log); s.log = g; }
  const f = s.n; s.log[s.n++] = v;
  return f;
}
// merge confirmed lockstep frames as far as both inputs are in
function pump(s) {
  const out = []; const start = s.n;
  for (;;) {
    const f = s.n; const a = s.in1.get(f); if (a === undefined) break;
    let b = 0xffff;
    if (s.p2 && f >= s.p2From) { b = s.in2.get(f); if (b === undefined) break; }
    s.in1.delete(f); s.in2.delete(f);
    if (append(s, (a & 0xffff) | ((b & 0xffff) << 16)) === undefined) return;
    out.push(s.log[f]);
  }
  if (out.length) bcastBin(s, frameMsg(start, out));
}
function endSession(s, reason) {
  if (!sessions.has(s.id)) return;
  sessions.delete(s.id);
  for (const w of members(s)) { if (w !== s.host) send(w, { t: 'ended', reason }); w.session = null; w.role = null; }
  s.host.session = null; s.host.role = null;
  if (s.host.readyState === 1 && reason) send(s.host, { t: 'ended', reason });
  lobbyDirty();
}
function leaveSession(ws) {
  const s = ws.session; if (!s) return;
  if (s.host === ws) return endSession(s, ws.kicked ? 'Disconnected.' : 'The host left the game.');
  if (s.p2 === ws) { s.p2 = null; s.in2.clear(); bcast(s, { t: 'p2left' }, ws); pump(s); }
  s.watchers.delete(ws); ws.session = null; ws.role = null;
  bcast(s, { t: 'sys', text: `${ws.name} left.` });
  lobbyDirty();
}
function rttMs(w) { return w.rtt == null ? 80 : w.rtt; }

const wss = new WebSocketServer({ noServer: true, maxPayload: 1 << 19 });
server.on('upgrade', (req, socket, head) => {
  if (!authorized(req) || new URL(req.url, 'http://x').pathname !== '/ws') { socket.write('HTTP/1.1 401 Unauthorized\r\n\r\n'); return socket.destroy(); }
  wss.handleUpgrade(req, socket, head, ws => wss.emit('connection', ws, req));
});

wss.on('connection', ws => {
  ws.isAlive = true; ws.session = null; ws.role = null; ws.name = 'Player'; ws.msgs = 0; ws.windowStart = Date.now(); ws.rtt = null; ws.hashes = new Map();
  clients.add(ws);
  send(ws, { t: 'lobby', sessions: lobbyList(), online: clients.size }); send(ws, { t: 'lobbychat', lines: lobbyChat });
  lobbyDirty();
  ws.on('pong', () => { ws.isAlive = true; });
  ws.on('message', (data, isBinary) => {
    const now = Date.now();
    if (now - ws.windowStart > 1000) { ws.windowStart = now; ws.msgs = 0; }
    if (++ws.msgs > 300) return;
    const s = ws.session;
    if (isBinary) {                                    // input: [u32 frame][u16 buttons]
      if (!s || data.length !== 6) return;
      const f = data.readUInt32LE(0), b = data.readUInt16LE(4);
      if (ws === s.host) {
        if (s.lockFrom < 0 || f < s.lockFrom) {        // host playing alone: this is the frame itself
          if (f === s.n) { append(s, b | (0xffff << 16)); bcastBin(s, frameMsg(f, [s.log[f]]), ws); s.last = now; }
        } else if (f < s.n + 256 && f >= s.n) { s.in1.set(f, b); pump(s); s.last = now; }
      } else if (ws === s.p2 && f >= s.p2From && f >= s.n && f < s.n + 256) { s.in2.set(f, b); pump(s); }
      return;
    }
    let m; try { m = JSON.parse(data.toString()); } catch { return; }
    switch (m.t) {
      case 'hello': ws.name = cleanName(m.name); break;
      case 'pong': if (ws.pingAt) { const r = Date.now() - ws.pingAt; ws.rtt = ws.rtt == null ? r : Math.round(ws.rtt * 0.7 + r * 0.3); ws.pingAt = 0; } break;
      case 'host': {
        if (s) return;
        if (sessions.size >= MAX_SESSIONS) return send(ws, { t: 'error', msg: 'Server is full, try again later.' });
        ws.name = cleanName(m.name);
        const mc = (m.memcard && typeof m.memcard === 'object') ? m.memcard : {};
        const ns = { id: sid(), host: ws, p2: null, watchers: new Set(), log: new Uint32Array(1 << 14), n: 0, lockFrom: -1, p2From: -1, delay: 0,
          in1: new Map(), in2: new Map(), memcard: mc, chat: [], hostHash: new Map(), started: Date.now(), last: Date.now() };
        sessions.set(ns.id, ns); ws.session = ns; ws.role = 'host';
        send(ws, { t: 'hosting', id: ns.id }); lobbyDirty();
        break;
      }
      case 'watch': {
        if (s) return;
        const t = sessions.get(String(m.id)); if (!t) return send(ws, { t: 'error', msg: 'That game has ended.' });
        ws.name = cleanName(m.name); ws.session = t; ws.role = 'watch'; t.watchers.add(ws);
        send(ws, { t: 'watching', id: t.id, host: t.host.name, memcard: t.memcard, n: t.n, lockFrom: t.lockFrom, p2From: t.p2From, delay: t.delay,
          p2: t.p2 ? t.p2.name : null, chat: t.chat });
        for (let i = 0; i < t.n; i += 16384) { const end = Math.min(t.n, i + 16384); ws.send(frameMsg(i, t.log.subarray(i, end)), { binary: true }); }
        bcast(t, { t: 'sys', text: `${ws.name} is watching.` }, ws); lobbyDirty();
        break;
      }
      case 'join': {                                   // a watcher asks to become player 2
        if (!s || ws.role !== 'watch') return;
        if (s.p2) return send(ws, { t: 'joinfail', msg: 'Player 2 is already taken.' });
        const mx = Math.max(rttMs(s.host), rttMs(ws));
        const delay = s.delay || Math.max(4, Math.min(14, Math.ceil(mx / 16.7) + 3));
        const F = s.n + JOIN_MARGIN;
        s.delay = delay; s.p2 = ws; s.p2From = F; s.in2.clear(); ws.role = 'p2'; s.watchers.delete(ws);
        if (s.lockFrom < 0) s.lockFrom = F;
        const msg = { t: 'seg', lockFrom: s.lockFrom, p2From: F, delay, p2: ws.name };
        bcast(s, msg); send(ws, { t: 'you_p2' });
        bcast(s, { t: 'sys', text: `${ws.name} joined as player 2!` }); lobbyDirty();
        break;
      }
      case 'hash': {
        if (!s) return; const f = m.frame | 0, h = m.h >>> 0;
        if (ws === s.host) {
          s.hostHash.set(f, h); if (s.hostHash.size > 4000) s.hostHash.delete(s.hostHash.keys().next().value);
          for (const w of members(s)) { const x = w.hashes.get(f); if (x !== undefined) { w.hashes.delete(f); if (x !== h) send(w, { t: 'desync', frame: f }); } }
        } else {
          const hh = s.hostHash.get(f);
          if (hh === undefined) { ws.hashes.set(f, h); if (ws.hashes.size > 200) ws.hashes.delete(ws.hashes.keys().next().value); }
          else if (hh !== h) send(ws, { t: 'desync', frame: f });
        }
        break;
      }
      case 'chat': {
        const text = String(m.text || '').slice(0, 300).trim(); if (!text) return;
        if (s) { const line = { name: ws.name, text }; s.chat.push(line); if (s.chat.length > 30) s.chat.shift(); bcast(s, { t: 'chat', ...line }); }
        else { const line = { name: ws.name, text }; lobbyChat.push(line); if (lobbyChat.length > 40) lobbyChat.shift(); for (const c of clients) if (!c.session) send(c, { t: 'chat', ...line }); }
        break;
      }
      case 'leave': leaveSession(ws); lobbyDirty(); send(ws, { t: 'lobby', sessions: lobbyList(), online: clients.size }); break;
    }
  });
  ws.on('close', () => { clients.delete(ws); ws.kicked = true; leaveSession(ws); lobbyDirty(); });
  ws.on('error', () => {});
});

setInterval(() => {
  const now = Date.now();
  for (const ws of wss.clients) {
    if (!ws.isAlive) { ws.terminate(); continue; }
    ws.isAlive = false; ws.ping(); ws.pingAt = now; send(ws, { t: 'ping' });
  }
  for (const s of sessions.values()) if (now - s.last > 5 * 60 * 1000) endSession(s, 'The game timed out.');
}, 5000).unref();

server.listen(PORT, HOST, () => console.log(`raidenproject-web listening on http://${HOST}:${PORT}  (dist: ${ROOT})${ACCESS_KEY ? '  [access key required]' : ''}`));
