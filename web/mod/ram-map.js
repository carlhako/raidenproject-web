// Raiden II (jj3) RAM map: where the game keeps players, enemies, bullets and pickups.
// Classic script (loaded by the game worker with importScripts and by tools/node-harness.mjs);
// it adds `RaidenMod.ram` to the global object. Details and how they were found: docs/RAM_MAP.md.
//
// Every reader takes `r`, a memory accessor with u8/u16/u32/s32(addr) over PSX addresses
// (main RAM 0x80000000.. and the scratchpad 0x1f800000..). See RaidenMod.ram.accessor().
(function (g) {
  'use strict';
  const M = g.RaidenMod || (g.RaidenMod = {});

  const FX = 65536;                       // positions and velocities are 16.16 fixed point
  const A = {
    scrollX: 0x1f80002c,                  // camera, 16.16, world units (scratchpad)
    scrollY: 0x1f800030,                  // world y grows upward; it rises as the stage scrolls
    playerInfo: [0x800ecf20, 0x800ecf60], // per player, 0x40 bytes each (see INFO)
    // ship objects live in the "players" pool; which slot a player gets depends on join order,
    // so always go through playerInfo[i] + INFO.ship
    poolTable: 0x8007bf90,                // static {start, count} pairs for every object pool
  };
  // Player info block
  const INFO = {
    active: 0x00,                          // u8: 1 while this player is in the game (incl. continue screen)
    down: 0x02,                            // u8: 1 from being shot down until the next ship flies in
    ship: 0x04,                            // u32: pointer to this player's ship object
    bombs: 0x18,                           // u8[8]: one byte per bomb in stock (bomb kind), 0 = empty
    lives: 0x20,                           // u8 (the bytes after it hold other counters)
    score: 0x28,                           // u32
    bombTimer: 0x38,                       // u32: counts down while a bomb is going off
  };
  // Object (all pools share the header)
  const OBJ = {
    x: 0x04, y: 0x08,                      // s32 16.16 world position
    vx: 0x0c, vy: 0x10,                    // s32 16.16 velocity (for the ships: move speed)
    layer: 0x31,                           // u8: draw/update list
    cls: 0x33,                             // u8: object class (see CLS)
    kind: 0x34,                            // u32: subtype (pickups: which item)
    prev: 0x38, next: 0x3c,                // pool links: objects before a pool's free head are live
    update: 0x4c, anim: 0x50,              // u32 handlers called every frame (0 = none)
  };
  const CLS = { AIR: 0x09, GROUND: 0x0a, BULLET: 0x0b, ITEM: 0x0c, PLAYER: 0x0d };
  const LAYER = { AIR: 0x15, ITEM: 0x17, BULLET: 0x1a, PLAYER: 0x1b };   // boss parts are class 0 on the air layer
  // Pools: address of the pool's free-list head pointer. Live objects are reached by walking
  // `prev` from the free head.
  const POOL = {
    players: 0x80101400,                   // ships (2 + spare), 0x128 bytes each
    enemies: 0x80101404,                   // air and ground enemies, 44 x 0x128
    items: 0x80101414,                     // pickups and score pop-ups, 10 x 0x128
    shotsA: 0x801013a8,                    // player shots, 36 x 0x90
    shotsB: 0x801013ac,                    // player shots, 36 x 0x90
    enemyShots: 0x801013b0,                // enemy bullets, 30 x 0x90
    effects: 0x801013b4,                   // explosions, debris, 100 x 0x90
    misc: 0x801013b8,                      // 64 x 0x90, rarely used
  };
  // Playfield: the ship is kept within scroll + [8, 232] x [16, 304]; it is drawn squeezed
  // vertically by 3/4 into the 240-line picture.
  const FIELD = { xMin: 8, xMax: 232, yMin: 16, yMax: 304 };
  const SCREEN = { x0: 58, yTop: 316, yScale: 0.75 };   // screen = (x + x0, (yTop - y) * yScale)

  function inRam(p) { return p >= 0x80000000 && p < 0x80200000; }

  function walk(r, pool, out, max = 128) {
    const head = r.u32(pool);
    if (!inRam(head)) return out;
    let p = r.u32(head + OBJ.prev), n = 0;
    while (inRam(p) && n++ < max) { out.push(p); p = r.u32(p + OBJ.prev); }
    return out;
  }

  function obj(r, p, sx, sy) {
    return {
      p, x: r.s32(p + OBJ.x) / FX - sx, y: r.s32(p + OBJ.y) / FX - sy,
      vx: r.s32(p + OBJ.vx) / FX, vy: r.s32(p + OBJ.vy) / FX,
      cls: r.u8(p + OBJ.cls), layer: r.u8(p + OBJ.layer), kind: r.u32(p + OBJ.kind) & 0xffff,
      update: r.u32(p + OBJ.update),
    };
  }

  function player(r, i, sx, sy) {
    const info = A.playerInfo[i];
    let bombs = 0; for (let k = 0; k < 8; k++) if (r.u8(info + INFO.bombs + k)) bombs++;
    const active = r.u8(info + INFO.active) === 1, sp = r.u32(info + INFO.ship);
    const ship = inRam(sp) ? sp : 0;
    const flying = active && ship !== 0 && r.u8(info + INFO.down) === 0;
    return {
      i, active, flying, lives: r.u8(info + INFO.lives), bombs, score: r.u32(info + INFO.score),
      bombing: r.u32(info + INFO.bombTimer) > 0,
      ship, x: ship ? r.s32(ship + OBJ.x) / FX - sx : -999, y: ship ? r.s32(ship + OBJ.y) / FX - sy : -999,
      speed: (ship && r.s32(ship + OBJ.vx) / FX) || 2,
    };
  }

  // One snapshot of everything the bot (or a debug overlay) cares about, in camera-relative
  // coordinates (x right, y up, origin = camera).
  function read(r) {
    const sx = r.s32(A.scrollX) / FX, sy = r.s32(A.scrollY) / FX;
    const s = { sx, sy, players: [player(r, 0, sx, sy), player(r, 1, sx, sy)], enemies: [], bullets: [], items: [] };
    for (const p of walk(r, POOL.enemies, [])) { const o = obj(r, p, sx, sy); if (o.update) s.enemies.push(o); }
    for (const p of walk(r, POOL.enemyShots, [])) { const o = obj(r, p, sx, sy); if (o.update) s.bullets.push(o); }
    for (const p of walk(r, POOL.misc, [])) { const o = obj(r, p, sx, sy); if (o.update && o.cls === CLS.BULLET) s.bullets.push(o); }
    for (const p of walk(r, POOL.items, [])) { const o = obj(r, p, sx, sy); if (o.update && o.cls === CLS.ITEM) s.items.push(o); }
    return s;
  }

  // Memory accessor over an Emscripten heap: ramOff/spOff from _psx_ram_ptr/_psx_scratch_ptr.
  // getHeap() must return the current HEAPU8 (it changes if wasm memory grows).
  function accessor(getHeap, ramOff, spOff) {
    const off = (a) => ((a >>> 0) & 0x1ffffc00) === 0x1f800000 ? spOff + (a & 0x3ff) : ramOff + (a & 0x1fffff);
    const r = {
      u8: (a) => getHeap()[off(a)],
      u16: (a) => { const h = getHeap(), o = off(a); return h[o] | h[o + 1] << 8; },
      u32: (a) => { const h = getHeap(), o = off(a); return (h[o] | h[o + 1] << 8 | h[o + 2] << 16 | h[o + 3] << 24) >>> 0; },
      s32: (a) => r.u32(a) | 0,
    };
    return r;
  }

  M.ram = { A, INFO, OBJ, CLS, LAYER, POOL, FIELD, SCREEN, walk, read, accessor };
})(typeof self !== 'undefined' ? self : globalThis);
