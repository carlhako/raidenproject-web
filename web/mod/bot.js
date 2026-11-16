// AI wingman: plays player 2 from what it reads in game RAM (web/mod/ram-map.js).
// Classic script; adds `RaidenMod.createBot()` to the global object.
//
// Deterministic: no Math.random, no clocks. Its output depends only on game RAM and its own
// state, though that does not matter for netplay because its pad words are sent through the
// server's input log like a human player's.
//
// Each frame it reads the enemy bullets and air enemies, predicts where they will be over the
// next ~half second (velocity measured from the previous frame), scores a set of short movement
// plans by how close they come to danger plus where it would like to be (under a target, near a
// pickup, low on the screen, away from player 1 and the walls), and takes the first step of the
// best plan. It holds rapid fire all the time and bombs when every plan gets hit.
(function (g) {
  'use strict';
  const M = g.RaidenMod || (g.RaidenMod = {});

  const BTN = { start: 8, up: 16, right: 32, down: 64, left: 128, rapid: 4096, bomb: 16384 };
  // 8 directions + stay; y is up in game space, so "up" adds to y
  const DIRS = [[0, 0], [0, 1], [1, 1], [1, 0], [1, -1], [0, -1], [-1, -1], [-1, 0], [-1, 1]];
  const DIR_BITS = DIRS.map(([dx, dy]) => (dx > 0 ? BTN.right : dx < 0 ? BTN.left : 0) | (dy > 0 ? BTN.up : dy < 0 ? BTN.down : 0));

  const DEFAULTS = {
    player: 1,          // which player the bot controls (0 = P1, 1 = P2)
    horizon: 22,        // frames of look-ahead
    switchAt: [3, 7, 12], // a plan = direction A for k frames, then direction B
    hitR: 9,            // centre distance that counts as a hit (bullet)
    margin: 14,         // extra distance that is still uncomfortable
    airR: 18,           // body radius of an air enemy
    homeY: 70,          // preferred height above the bottom of the field
    keepFromP1: 36,     // try to keep this far from player 1
    bombCooldown: 150,
    airLayer: true,     // also avoid class-0 objects on the air layer (boss parts)
    lead: 0,            // extra frames before the pad takes effect (the worker adds the netplay delay)
  };

  function createBot(opts = {}) {
    const C = Object.assign({}, DEFAULTS, opts);
    const ram = M.ram;
    let prevPos = new Map();   // object pointer -> [x, y] last frame (camera-relative)
    let prevSy = 0, prevSx = 0;
    let lastDir = 0, bombWait = 0, startTimer = 0, frameNo = 0;
    let debug = null;

    function velocity(o, sx, sy) {
      const q = prevPos.get(o.p);
      if (q) {   // world-space motion; positions are camera-relative so add the camera's move back
        const vx = o.x - q[0] + (sx - prevSx), vy = o.y - q[1] + (sy - prevSy);
        if (Math.abs(vx) < 16 && Math.abs(vy) < 16) return [vx, vy];
      }
      return [o.vx, o.vy];
    }

    // threats: {x, y, vx, vy, r} in camera space; camera motion per frame is (cvx, cvy)
    function planCost(me, threats, dA, k, dB, lead, goal) {
      const F = ram.FIELD, H = C.horizon, sp = me.speed;
      let x = me.x, y = me.y, cost = 0, firstHit = 99;
      for (let t = 1; t <= H; t++) {
        const d = DIRS[t <= k ? dA : dB];
        x = Math.min(F.xMax, Math.max(F.xMin, x + d[0] * sp));
        y = Math.min(F.yMax, Math.max(F.yMin, y + d[1] * sp));
        const tt = t + lead, w = 1 + (H - t) / H * 2;    // near-term danger counts up to 3x
        for (let i = 0; i < threats.length; i++) {
          const b = threats[i];
          const bx = b.x + b.vx * tt, by = b.y + b.vy * tt;
          const ddx = bx - x, ddy = by - y, R = b.r;
          const d2 = ddx * ddx + ddy * ddy, lim = R + C.margin;
          if (d2 >= lim * lim) continue;
          const dist = Math.sqrt(d2);
          if (dist < R) { cost += 1000 * w; if (t < firstHit) firstHit = t; }
          else cost += (lim - dist) * (lim - dist) * 0.6 * w;
        }
      }
      // where it ends up
      cost += goal(x, y);
      // walls and corners
      if (x < F.xMin + 16) cost += (F.xMin + 16 - x) * 3;
      if (x > F.xMax - 16) cost += (x - (F.xMax - 16)) * 3;
      if (y < F.yMin + 10) cost += (F.yMin + 10 - y) * 3;
      return { cost, firstHit };
    }

    function step(r, frame) {
      frameNo = frame;
      const s = ram.read(r);
      const me = s.players[C.player], other = s.players[1 - C.player];
      let pad = 0;

      // not in the game, or out of lives: press Start now and then (title screen, join, continue).
      // Never while playing: Start pauses the game.
      if (!me.active || (!me.flying && me.lives === 0)) {
        startTimer++;
        if (startTimer % 90 < 6) pad |= BTN.start;
        prevPos = new Map(); debug = null;
        return pad;
      }
      startTimer = 0;
      if (!me.flying) { prevPos = new Map(); debug = null; return BTN.rapid; }   // exploding / respawning

      const lead = C.lead + (C.extraLead || 0);
      const threats = [];
      const next = new Map();
      for (const b of s.bullets) {
        const [vx, vy] = velocity(b, s.sx, s.sy);
        next.set(b.p, [b.x, b.y]);
        // camera-relative motion: bullets keep their world velocity, the camera scrolls with cvx/cvy
        threats.push({ x: b.x, y: b.y, vx: vx - (s.sx - prevSx), vy: vy - (s.sy - prevSy), r: C.hitR });
      }
      const air = [];
      for (const e of s.enemies) {
        if (e.cls !== ram.CLS.AIR && !(C.airLayer && e.layer === ram.LAYER.AIR)) continue;   // ground units cannot ram you
        const [vx, vy] = velocity(e, s.sx, s.sy);
        next.set(e.p, [e.x, e.y]);
        const t = { x: e.x, y: e.y, vx: vx - (s.sx - prevSx), vy: vy - (s.sy - prevSy), r: C.airR };
        threats.push(t); air.push(t);
      }

      // --- what it wants: a firing lane under a target, pickups, a home height, room from P1
      let targetX = null, best = 1e9;
      for (const e of s.enemies) {
        if (e.x < 0 || e.x > 240 || e.y < me.y + 20 || e.y > 320) continue;   // on screen and ahead
        const p1Lane = other.flying ? Math.abs(other.x - e.x) < 14 : false;
        const isAir = e.layer === ram.LAYER.AIR || e.cls === ram.CLS.AIR;
        const sc = Math.abs(e.x - me.x) * 0.7 + (e.y - me.y) * 0.3 + (p1Lane ? 60 : 0) + (isAir ? 0 : 25);
        if (sc < best) { best = sc; targetX = e.x; }
      }
      let item = null; best = 1e9;
      for (const it of s.items) {
        if (it.x < 4 || it.x > 236 || it.y < 8 || it.y > 300) continue;
        const dMe = Math.hypot(it.x - me.x, it.y - me.y);
        const dP1 = other.flying ? Math.hypot(it.x - other.x, it.y - other.y) : 1e9;
        if (dP1 < dMe * 0.6) continue;                      // let player 1 have the ones they are on top of
        if (dMe < best) { best = dMe; item = it; }
      }
      const homeY = C.homeY;
      const goal = (x, y) => {
        let c = 0;
        if (item) c += Math.hypot(item.x - x, item.y - y) * 0.9;
        else c += Math.abs(y - homeY) * 0.25;
        if (targetX !== null) c += Math.abs(x - targetX) * (item ? 0.15 : 0.6);
        else c += Math.abs(x - (other.flying ? (other.x < 120 ? 170 : 70) : 120)) * 0.2;
        for (const a of air) {                               // do not sit right under/over big planes
          const dx = Math.abs(a.x - x), dy = a.y - y;
          if (dx < 28 && dy > -10 && dy < 60) c += (28 - dx) * 1.5;
        }
        if (other.flying) {
          const d = Math.hypot(other.x - x, other.y - y);
          if (d < C.keepFromP1) c += (C.keepFromP1 - d) * 1.2;
        }
        return c;
      };

      // --- search
      let bestCost = 1e18, bestDir = 0, anySafe = false, bestPlan = null;
      for (const k of C.switchAt) for (let a = 0; a < 9; a++) for (let b = 0; b < 9; b++) {
        const res = planCost(me, threats, a, k, b, lead, goal);
        let c = res.cost;
        if (a === lastDir) c -= 4;                           // hysteresis against jitter
        if (res.firstHit > 8) anySafe = true;
        if (c < bestCost) { bestCost = c; bestDir = a; bestPlan = [a, k, b]; }
      }
      lastDir = bestDir;
      pad |= DIR_BITS[bestDir] | BTN.rapid;

      if (bombWait > 0) bombWait--;
      if (!anySafe && me.bombs > 0 && bombWait === 0 && !me.bombing) { pad |= BTN.bomb; bombWait = C.bombCooldown; }

      prevPos = next; prevSx = s.sx; prevSy = s.sy;
      debug = { me: [me.x, me.y], plan: bestPlan, cost: bestCost, threats: threats.length, targetX, item: item ? [item.x, item.y] : null };
      return pad;
    }

    return { step, get debug() { return debug; }, config: C };
  }

  M.createBot = createBot;
  M.BTN = BTN;
})(typeof self !== 'undefined' ? self : globalThis);
