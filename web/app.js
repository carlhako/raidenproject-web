// Page logic: disc download, lobby, input, rendering, audio, and the WebSocket side of netplay.
import { createDisplay, FILTERS } from './display.js';
const $ = (id) => document.getElementById(id);
const el = { menu: $('menu'), status: $('status'), progress: $('progress'), bar: $('bar'), lobby: $('lobby'), toast: $('toast'), chat: $('chat'), log: $('log'),
  chatInput: $('chat-input'), screen: $('screen'), own: $('own-disc'), stage: $('stage'), filters: $('filters'), banner: $('banner'), leave: $('btn-leave'),
  games: $('games'), online: $('online'), name: $('name'), lobbyLog: $('lobby-log'), lobbyInput: $('lobby-input') };
const display = createDisplay(el.screen);
const CTRL = { SEQ: 0, W: 1, H: 2, FRAME: 3, AUD_W: 4, AUD_R: 5, PAD0: 6, LOGN: 8, WAKE: 9, ROLE: 11, LOCK: 12, P2FROM: 13, DELAY: 14, CATCH: 15 };
const ROLES = { host: 0, watch: 1, p2: 2 };

function setStatus(msg, err = false) { el.status.textContent = msg; el.status.classList.toggle('err', err); }
function show(...which) { for (const k of ['lobby', 'progress']) el[k].hidden = !which.includes(k); }
// ---------------------------------------------------------------- error reporting (POST /api/clientlog)
const reported = new Set(); let reportCount = 0;
function errText(e) {
  if (!e || typeof e !== 'object') return String(e);
  const head = (e.name || 'Error') + ': ' + (e.message || String(e)), st = e.stack || '';
  return st.startsWith(head) ? st : head + (st ? '\n' + st : '');
}
function reportError(kind, msg, extra = {}) {
  try {
    msg = String(msg).slice(0, 8000);
    const key = kind + '|' + msg.slice(0, 300);
    if (reported.has(key) || ++reportCount > 20) return;
    reported.add(key);
    const g = game;
    const body = JSON.stringify({
      kind, msg, ...extra, ua: navigator.userAgent, url: location.pathname, t: new Date().toISOString(),
      role: g?.role ?? null, session: g?.id ?? null, frame: g ? Atomics.load(g.ctrl, CTRL.FRAME) : null,
      audio: audioCtx ? audioCtx.state : null,
    });
    const blob = new Blob([body], { type: 'application/json' });
    if (!(navigator.sendBeacon && navigator.sendBeacon('api/clientlog', blob)))
      fetch('api/clientlog', { method: 'POST', body, headers: { 'Content-Type': 'application/json' }, keepalive: true }).catch(() => {});
  } catch { /* never let reporting throw */ }
}
window.addEventListener('error', (e) => reportError('window.error', e.error ? errText(e.error) : e.message, { src: `${e.filename}:${e.lineno}:${e.colno}` }));
window.addEventListener('unhandledrejection', (e) => reportError('unhandledrejection', errText(e.reason)));

function toast(msg, ms = 6000) { el.toast.textContent = msg; el.toast.hidden = false; clearTimeout(toast.t); toast.t = setTimeout(() => el.toast.hidden = true, ms); }
const storeGet = (k) => { try { return JSON.parse(localStorage.getItem(k)); } catch { return null; } };
const storeSet = (k, v) => { try { localStorage.setItem(k, JSON.stringify(v)); } catch { /* storage unavailable */ } };

// ---------------------------------------------------------------- video filter (top-right selector, V key)
function renderFilters() {
  el.filters.textContent = '';
  const ok = new Set(display.filters.map((f) => f.id));
  for (const f of FILTERS) {
    const b = document.createElement('button'); b.textContent = f.name; b.setAttribute('aria-pressed', String(f.id === display.filter));
    if (!ok.has(f.id)) { b.disabled = true; b.title = 'Needs WebGL2'; }
    b.onclick = (e) => { setFilter(f.id); e.currentTarget.blur(); }; el.filters.append(b);
  }
}
function setFilter(id) { display.setFilter(id); storeSet('filter', id); renderFilters(); }
function cycleFilter() {
  const list = display.filters; if (!list.length) return;
  setFilter(list[(list.findIndex((x) => x.id === display.filter) + 1) % list.length].id);
}
if (storeGet('filter')) display.setFilter(storeGet('filter'));
renderFilters();

// ---------------------------------------------------------------- disc (downloaded once into OPFS)
async function opfsDisc() { return (await navigator.storage.getDirectory()).getFileHandle('disc.img', { create: true }); }

async function writeStream(readable, total, label) {
  const fh = await opfsDisc(); const w = await fh.createWritable();
  const reader = readable.getReader(); let got = 0; const t0 = performance.now();
  for (;;) {
    const { done, value } = await reader.read(); if (done) break;
    await w.write(value); got += value.length;
    el.bar.style.width = (total ? 100 * got / total : 0) + '%';
    const mb = (got / 1048576).toFixed(0), rate = (got / 1048576 / ((performance.now() - t0) / 1000)).toFixed(1);
    setStatus(`${label}: ${mb}${total ? ' / ' + (total / 1048576).toFixed(0) : ''} MB (${rate} MB/s)`);
  }
  await w.close(); return got;
}

async function ensureDisc() {
  let meta = null;
  try { const r = await fetch('data/disc.json', { cache: 'no-cache' }); if (r.ok) meta = await r.json(); } catch { /* none hosted */ }
  const have = storeGet('disc-meta');
  let size = 0; try { size = (await (await opfsDisc()).getFile()).size; } catch { /* none */ }
  if (size && have && ((meta && have.sha256 === meta.sha256 && size === meta.size) || (have.own && size === have.size))) return true;
  if (!meta) { setStatus('This server has no game data. Choose your own disc image below.', true); show(); return false; }
  show('progress'); setStatus('Downloading game data (first visit only)…');
  const r = await fetch('data/disc.img'); if (!r.ok) throw new Error('disc.img: HTTP ' + r.status);
  const got = await writeStream(r.body, meta.size, 'Downloading game data');
  if (got !== meta.size) throw new Error('Download incomplete');
  storeSet('disc-meta', { sha256: meta.sha256, size: meta.size });
  return true;
}

el.own.addEventListener('change', async () => {
  const f = el.own.files[0]; if (!f) return;
  if (f.size % 2352) { setStatus('That is not a raw 2352-byte-sector .bin image.', true); return; }
  show('progress');
  try { await writeStream(f.stream(), f.size, 'Importing disc'); storeSet('disc-meta', { own: true, size: f.size }); await boot(); }
  catch (e) { setStatus('Import failed: ' + e.message, true); show('lobby'); }
});

// ---------------------------------------------------------------- input
const KEYS = { ArrowUp: 1 << 4, ArrowRight: 1 << 5, ArrowDown: 1 << 6, ArrowLeft: 1 << 7, Enter: 1 << 3, Backspace: 1, ShiftRight: 1,
  KeyZ: 1 << 14, KeyX: 1 << 13, KeyA: 1 << 15, KeyS: 1 << 12, KeyQ: 1 << 10, KeyW: 1 << 11, KeyE: 1 << 8, KeyR: 1 << 9 };
let kb = 0, game = null, prevStart = false;
const latched = new Map();      // bit -> expiry: a tap shorter than ~2 frames still registers       // game = { worker, ctrl, log, fb, role, id, ... } while in a game

function padFromGamepad(gp) {
  if (!gp) return 0;
  const b = (i) => gp.buttons[i] && gp.buttons[i].pressed; let m = 0;
  if (b(12)) m |= 1 << 4; if (b(15)) m |= 1 << 5; if (b(13)) m |= 1 << 6; if (b(14)) m |= 1 << 7;
  if (b(9)) m |= 1 << 3; if (b(8)) m |= 1; if (b(0)) m |= 1 << 14; if (b(1)) m |= 1 << 13; if (b(2)) m |= 1 << 15; if (b(3)) m |= 1 << 12;
  if (b(4)) m |= 1 << 10; if (b(5)) m |= 1 << 11; if (b(6)) m |= 1 << 8; if (b(7)) m |= 1 << 9;
  const ax = gp.axes[0] || 0, ay = gp.axes[1] || 0;
  if (ax < -0.5) m |= 1 << 7; if (ax > 0.5) m |= 1 << 5; if (ay < -0.5) m |= 1 << 4; if (ay > 0.5) m |= 1 << 6;
  return m;
}
function updatePad() {
  if (!game) return;
  const pads = (navigator.getGamepads ? [...navigator.getGamepads()] : []).filter(Boolean);
  const nowMs = performance.now(); let lat = 0;
  for (const [bit, until] of latched) { if (until > nowMs) lat |= bit; else latched.delete(bit); }
  const m0 = kb | lat | padFromGamepad(pads[0]);
  Atomics.store(game.ctrl, CTRL.PAD0, (~m0) & 0xffff);
  if (!keysEl.hidden) for (const k of keyCaps) k.el.classList.toggle('on', !!(m0 & k.bit));
  const st = !!(m0 & (1 << 3));
  if (st && !prevStart) onStartPressed();
  prevStart = st;
}
const keysEl = document.getElementById('keys');
const keyCaps = [...keysEl.querySelectorAll('.k')].map((e) => ({ el: e, bit: +e.dataset.bit }));
let keysWanted = true; try { keysWanted = localStorage.getItem('keys') !== '0'; } catch {}
const syncKeys = () => { keysEl.hidden = !(game && keysWanted); };
const typing = (e) => e.target instanceof HTMLInputElement;
addEventListener('keydown', (e) => {
  if (typing(e) || !game) return;
  if (e.code === 'KeyT') { e.preventDefault(); if (!e.repeat) el.chatInput.focus(); return; }
  if (e.code === 'KeyK' && !e.repeat) { keysWanted = !keysWanted; try { localStorage.setItem('keys', keysWanted ? '1' : '0'); } catch {} syncKeys(); return; }
  if (e.code === 'KeyV' && !e.repeat) { e.preventDefault(); cycleFilter(); return; }
  if (e.code in KEYS) { e.preventDefault(); kb |= KEYS[e.code]; latched.set(KEYS[e.code], performance.now() + 40); updatePad(); }
});
addEventListener('keyup', (e) => { if (game && e.code in KEYS) { kb &= ~KEYS[e.code]; updatePad(); } });
addEventListener('blur', () => { kb = 0; latched.clear(); updatePad(); });
setInterval(updatePad, 8);

// Start pressed while watching = ask to join as player 2 (once caught up with the live game)
function onStartPressed() {
  if (!game || game.role !== 'watch') return;
  if (Atomics.load(game.ctrl, CTRL.CATCH)) return banner('Still catching up to the live game…');
  if (game.p2) return banner('Player 2 is already taken. Keep watching!');
  if (game.joining) return; game.joining = true; sendWs({ t: 'join' });
}

// ---------------------------------------------------------------- game session
let audioCtx = null, raf = 0, lastSeq = -1, holdUntil = 0;
function banner(msg, cls, ms) {
  el.banner.textContent = msg; el.banner.className = cls || ''; el.banner.hidden = !msg;
  holdUntil = ms ? Date.now() + ms : 0;
}
function refreshBanner() {
  if (!game) return banner('');
  const c = game.ctrl;
  if (game.role === 'watch') {
    if (Atomics.load(c, CTRL.CATCH)) {
      const n = Atomics.load(c, CTRL.LOGN), f = Atomics.load(c, CTRL.FRAME);
      return banner(`Catching up to ${game.host}'s game… ${n ? Math.floor(100 * f / n) : 0}%`);
    }
    return banner(game.p2 ? `Watching ${game.host} and ${game.p2}` : `Watching ${game.host}: press Start to join as player 2`, game.p2 ? '' : 'go');
  }
  if (game.role === 'p2') return banner(`You are player 2 in ${game.host}'s game`, 'go');
  const o = game.p2 ? ` with ${game.p2}` : ''; banner(o ? `Playing${o}` : '');
}
setInterval(() => { if (game && Date.now() >= holdUntil) refreshBanner(); }, 250);

function b64ToCard(saved) {
  const out = {}; for (const [k, v] of Object.entries(saved || {})) out[k] = Uint8Array.from(atob(v), (c) => c.charCodeAt(0)); return out;
}
async function startGame(role, opts = {}) {
  stopGame();
  const ctrlSab = new SharedArrayBuffer(256), fbSab = new SharedArrayBuffer(640 * 512 * 4),
    audSab = new SharedArrayBuffer(32768 * 2 * 2), logSab = new SharedArrayBuffer(4 << 19);
  const ctrl = new Int32Array(ctrlSab), log = new Uint32Array(logSab), fb = new Uint8Array(fbSab);
  Atomics.store(ctrl, CTRL.PAD0, 0xffff); Atomics.store(ctrl, CTRL.ROLE, ROLES[role]); Atomics.store(ctrl, CTRL.LOCK, opts.lockFrom ?? -1);
  Atomics.store(ctrl, CTRL.P2FROM, opts.p2From ?? -1); Atomics.store(ctrl, CTRL.DELAY, opts.delay || 0);
  const worker = new Worker('game-worker.js');
  game = { worker, ctrl, log, fb, role, id: opts.id, host: opts.host || el.name.value || 'You', p2: opts.p2 || null, joining: false, localHash: new Map() };
  worker.onmessage = (e) => onWorker(e.data);
  worker.onerror = (e) => { const m = `${e.message} (${e.filename}:${e.lineno}:${e.colno})`; toast('Worker error: ' + m); reportError('worker.error', m); };
  worker.onmessageerror = () => reportError('worker.messageerror', 'message could not be deserialized');
  worker.postMessage({ type: 'start', ctrl: ctrlSab, fb: fbSab, audio: audSab, log: logSab, memcard: b64ToCard(opts.memcard) });
  el.menu.hidden = true; el.chat.hidden = false; el.leave.hidden = false; syncKeys(); el.screen.classList.remove('idle');
  lastSeq = -1; cancelAnimationFrame(raf); raf = requestAnimationFrame(render);
  refreshBanner();
  const g = game;
  if (!audioCtx || audioCtx.state === 'closed') {
    try {
      audioCtx = new AudioContext({ sampleRate: 44100, latencyHint: 'interactive' });
      await audioCtx.audioWorklet.addModule('audio-worklet.js');
      const node = new AudioWorkletNode(audioCtx, 'psx-audio', { outputChannelCount: [2], processorOptions: { ctrl: ctrlSab, audio: audSab } });
      node.onprocessorerror = (e) => reportError('audio.processorerror', errText(e.error || e.message || 'audio worklet failed'));
      node.connect(audioCtx.destination);
    } catch (e) { console.warn('audio unavailable', e); reportError('audio.init', errText(e)); audioCtx = null; }
  }
  if (g === game && audioCtx && audioCtx.state === 'suspended') audioCtx.resume();
}

function render() {
  raf = requestAnimationFrame(render);
  if (!game) return;
  const seq = Atomics.load(game.ctrl, CTRL.SEQ);
  if ((seq & 1) || seq === lastSeq) return;
  const w = Atomics.load(game.ctrl, CTRL.W), h = Atomics.load(game.ctrl, CTRL.H);
  if (!w || !h) return;
  const px = display.pixels(w, h);
  for (let y = 0; y < h; y++) px.set(game.fb.subarray(y * 2560, y * 2560 + w * 4), y * w * 4);
  if (Atomics.load(game.ctrl, CTRL.SEQ) !== seq) return;      // torn: try again next frame
  lastSeq = seq; display.present();
}

function onWorker(m) {
  switch (m.type) {
    case 'input':                                    // local input for frame m.frame -> server
      if (ws && ws.readyState === 1 && game && game.role !== 'watch') { const b = new DataView(new ArrayBuffer(6)); b.setUint32(0, m.frame, true); b.setUint16(4, m.b, true); ws.send(b.buffer); }
      break;
    case 'hash': sendWs({ t: 'hash', frame: m.frame, h: m.h }); break;
    case 'memcard': if (game && game.role === 'host') storeSet('memcard', m.files); break;
    case 'fatal': toast('Game crashed: ' + m.msg, 20000); console.error(m.msg); reportError('fatal', m.msg); break;
    case 'log': if (/FATAL|cannot/i.test(m.msg)) { console.warn(m.msg); reportError('worker.log', m.msg); } break;
  }
}

function stopGame() {
  if (game) { game.worker.terminate(); game = null; }
  cancelAnimationFrame(raf); kb = 0; prevStart = false;
  if (audioCtx) { audioCtx.close().catch(() => {}); audioCtx = null; }
  syncKeys(); el.chat.hidden = true; el.log.textContent = ''; el.leave.hidden = true; banner('');
}
function toMenu(msg) {
  stopGame(); el.menu.hidden = false; show('lobby'); setStatus(msg || ''); display.reset(); el.screen.classList.add('idle');
}

// ---------------------------------------------------------------- server connection
let ws = null, wsOpen = null;
const sendWs = (o) => { if (ws && ws.readyState === 1) ws.send(JSON.stringify(o)); };
function connect() {
  if (wsOpen) return wsOpen;
  wsOpen = new Promise((resolve, reject) => {
    ws = new WebSocket((location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/ws');
    ws.binaryType = 'arraybuffer';
    ws.onopen = () => { sendWs({ t: 'hello', name: myName() }); resolve(ws); };
    ws.onerror = () => { wsOpen = null; reject(new Error('Cannot reach the game server')); };
    ws.onclose = () => { wsOpen = null; ws = null; const was = !!game; toMenu(was ? 'Connection to the server was lost.' : ''); setTimeout(() => { if (!game) connect().catch(() => {}); }, 3000); };
    ws.onmessage = (e) => onWs(e.data);
  });
  return wsOpen;
}
const myName = () => (el.name.value || '').trim().slice(0, 16) || 'Player';

function onWs(data) {
  if (typeof data !== 'string') {                    // confirmed frames: [u32 start][u32 count][u32 x count]
    if (!game) return;
    const v = new DataView(data), start = v.getUint32(0, true), n = v.getUint32(4, true);
    for (let i = 0; i < n; i++) game.log[start + i] = v.getUint32(8 + 4 * i, true);
    const end = start + n;
    if (end > Atomics.load(game.ctrl, CTRL.LOGN)) Atomics.store(game.ctrl, CTRL.LOGN, end);
    Atomics.add(game.ctrl, CTRL.WAKE, 1); Atomics.notify(game.ctrl, CTRL.WAKE);
    return;
  }
  const m = JSON.parse(data);
  switch (m.t) {
    case 'ping': sendWs({ t: 'pong' }); break;
    case 'lobby': renderGames(m); break;
    case 'lobbychat': el.lobbyLog.textContent = ''; m.lines.forEach((l) => chatLine(l, null, true)); break;
    case 'hosting': startGame('host', { id: m.id, host: myName(), memcard: storeGet('memcard') || {} }); chatLine({ text: 'Your game is live: anyone can watch and join as player 2.' }, 'sys'); break;
    case 'watching':
      startGame('watch', { id: m.id, host: m.host, memcard: m.memcard, lockFrom: m.lockFrom, p2From: m.p2From, delay: m.delay, p2: m.p2 });
      (m.chat || []).forEach((l) => chatLine(l)); chatLine({ text: `Watching ${m.host}. Press Start to join as player 2.` }, 'sys'); break;
    case 'seg':
      if (!game) break;
      Atomics.store(game.ctrl, CTRL.DELAY, m.delay); Atomics.store(game.ctrl, CTRL.P2FROM, m.p2From); Atomics.store(game.ctrl, CTRL.LOCK, m.lockFrom);
      game.p2 = m.p2; break;
    case 'you_p2': if (game) { game.role = 'p2'; Atomics.store(game.ctrl, CTRL.ROLE, ROLES.p2); game.joining = false; refreshBanner(); } break;
    case 'joinfail': if (game) { game.joining = false; banner(m.msg, '', 3000); } break;
    case 'p2left': if (game) { game.p2 = null; chatLine({ text: 'Player 2 left.' }, 'sys'); } break;
    case 'sys': chatLine({ text: m.text }, 'sys'); break;
    case 'chat': chatLine(m); break;
    case 'desync': reportError('desync', `frame ${m.frame}`); toast(`Desync detected at frame ${m.frame}: this game is no longer in sync with the host. Leave and watch again.`, 30000); break;
    case 'ended': if (game) toMenu(m.reason || 'The game ended.'); break;
    case 'error': setStatus(m.msg, true); if (game) toMenu(m.msg); break;
  }
}
function chatLine(m, cls, lobby) {
  const d = document.createElement('div');
  if (m.name) { const b = document.createElement('b'); b.textContent = m.name + ': '; d.append(b); }
  d.append(document.createTextNode(m.text)); if (cls) d.className = cls;
  const target = lobby || !game ? el.lobbyLog : el.log; target.append(d);
  if (target === el.lobbyLog) { while (target.children.length > 60) target.firstChild.remove(); target.scrollTop = target.scrollHeight; }
  else while (target.children.length > 8) target.firstChild.remove();
}
function fmtTime(s) { return Math.floor(s / 60) + ':' + String(s % 60).padStart(2, '0'); }
function renderGames(m) {
  if (game) return;
  el.online.textContent = m.online ? `· ${m.online} online` : '';
  el.games.textContent = '';
  if (!m.sessions.length) { const li = document.createElement('li'); li.className = 'empty'; li.textContent = 'Nobody is playing right now.'; el.games.append(li); return; }
  for (const s of m.sessions) {
    const li = document.createElement('li'), info = document.createElement('div'), btn = document.createElement('button');
    info.className = 'info'; info.textContent = s.p2 ? `${s.host} + ${s.p2}` : s.host;
    const sm = document.createElement('small'); sm.textContent = `${s.p2 ? '2 players' : 'Player 2 free'} · ${s.watchers} watching · ${fmtTime(s.secs)}`; info.append(sm);
    btn.textContent = 'Watch'; btn.onclick = () => watch(s.id);
    li.append(info, btn); el.games.append(li);
  }
}

// ---------------------------------------------------------------- menu wiring
async function play() {
  try { await connect(); storeSet('name', myName()); sendWs({ t: 'hello', name: myName() }); sendWs({ t: 'host', name: myName(), memcard: storeGet('memcard') || {} }); } catch (e) { setStatus(e.message, true); }
}
async function watch(id) {
  try { await connect(); storeSet('name', myName()); setStatus('Joining the game…'); sendWs({ t: 'watch', id, name: myName() }); } catch (e) { setStatus(e.message, true); }
}
el.name.value = storeGet('name') || '';
$('btn-play').onclick = play;
$('btn-leave').onclick = () => { sendWs({ t: 'leave' }); toMenu(); };
$('btn-full').onclick = () => { if (document.fullscreenElement) document.exitFullscreen(); else el.stage.requestFullscreen(); };
$('chat-form').addEventListener('submit', (e) => { e.preventDefault(); const t = el.chatInput.value.trim(); if (t) sendWs({ t: 'chat', text: t }); el.chatInput.value = ''; el.chatInput.blur(); });
$('lobby-form').addEventListener('submit', (e) => { e.preventDefault(); const t = el.lobbyInput.value.trim(); if (t) sendWs({ t: 'chat', text: t }); el.lobbyInput.value = ''; });
for (const i of [el.chatInput, el.lobbyInput, el.name]) i.addEventListener('keydown', (e) => { e.stopPropagation(); if (e.key === 'Escape') i.blur(); });

async function boot() {
  el.screen.classList.add('idle');
  if (!crossOriginIsolated) { setStatus('This page must be served over HTTPS (or localhost) with cross-origin isolation.', true); return; }
  try {
    if (!(await ensureDisc())) return;
    show('lobby'); setStatus('');
    await connect().catch((e) => setStatus(e.message, true));
  } catch (e) { setStatus('Setup failed: ' + e.message, true); reportError('setup', errText(e)); show('lobby'); }
}
boot();
