// node-harness hook: the AI wingman (web/mod/bot.js) plays player 2; reports how it fares.
//   HOOK=tools/hooks/bot.mjs node --stack-size=984 tools/node-harness.mjs <disc> 20000 "300:start,600:start,900:start,1200:start"
// env: BOT_P1=1 lets a second bot instance fly player 1 too; BOT_RANDOM=1 replaces the bot with random input
//      (baseline); BOT_SHOTS=n saves a screenshot with the bot's view every n frames.
import fs from 'node:fs'; import vm from 'node:vm'; import path from 'node:path';
const here = path.dirname(new URL(import.meta.url).pathname);
for (const f of ['ram-map.js', 'bot.js']) vm.runInThisContext(fs.readFileSync(path.join(here, '../../web/mod', f), 'utf8'), { filename: f });
const M = globalThis.RaidenMod;
let ctx, bot2, bot1, out = 0;
const st = { frames: 0, flying: 0, deaths: 0, bombs: 0, joined: -1, lastLives: -1, wasFlying: false, p1deaths: 0, p1fly: false, maxScore: 0 };
const shots = +(process.env.BOT_SHOTS || 0);
export function init(c) { ctx = c; bot2 = M.createBot(Object.assign({ player: 1 }, JSON.parse(process.env.BOT_CFG || '{}'))); if (process.env.BOT_P1) bot1 = M.createBot({ player: 0 }); }
export function pad(port, f, p) {
  if (port === 1) {
    if (process.env.BOT_RANDOM) {
      const s = M.ram.read(ctx.ram); if (!s.players[1].active) return (f % 90 < 6) ? 8 : 0;
      let x = (Math.floor(f / 8) * 2654435761) >>> 0; x ^= x >>> 15; return (x & 0xf0) | 4096;
    }
    const b = bot2.step(ctx.ram, f); if (b & 16384) st.bombs++; return p | b;
  }
  if (port === 0 && bot1 && f > 1300) return p | bot1.step(ctx.ram, f);
  return p;
}
export function frame(f) {
  const s = M.ram.read(ctx.ram), me = s.players[1], p1 = s.players[0];
  if (me.active && st.joined < 0) st.joined = f;
  if (st.joined >= 0) { st.frames++; if (me.flying) st.flying++; }
  if (st.wasFlying && !me.flying) {
    st.deaths++; const tag = 'death_' + String(f).padStart(6, '0');
    ctx.shot(tag); fs.writeFileSync(path.join(ctx.outdir, tag + '.json'), JSON.stringify({ prev: st.prev, s, info: [...ctx.ram.bytes(0x800ecf60, 0x40)], ship: [...ctx.ram.bytes(0x800f7ac4, 0x128)] }));
  }
  st.prev = { s, dbg: bot2.debug };
  st.wasFlying = me.flying;
  if (st.p1fly && !p1.flying) st.p1deaths++; st.p1fly = p1.flying;
  st.maxScore = Math.max(st.maxScore, me.score);
  if (shots && f % shots === 0 && st.joined >= 0) {
    ctx.shot('bot_' + String(f).padStart(6, '0'));
    fs.writeFileSync(path.join(ctx.outdir, 'bot_' + String(f).padStart(6, '0') + '.json'), JSON.stringify({ s, dbg: bot2.debug }));
  }
  if (f % 2000 === 0) report(f);
}
function report(f) { console.log(`[bot] f=${f} joined=${st.joined} frames=${st.frames} flying=${st.flying} deaths=${st.deaths} bombs=${st.bombs} p2score=${st.maxScore} p1deaths=${st.p1deaths} lives=${M.ram.read(ctx.ram).players[1].lives}`); }
