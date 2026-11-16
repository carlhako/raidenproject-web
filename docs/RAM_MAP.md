# Raiden II (jj3) RAM map

These are the parts of Raiden II's memory that the Modified mode reads, mainly for the AI wingman.
`web/mod/ram-map.js` encodes the same facts in code; when the two disagree, the code is right.

Addresses are PSX virtual addresses. Main RAM starts at `0x80000000` and the scratchpad at
`0x1f800000`.

## Coordinates

- Positions and velocities are **16.16 fixed point** (`s32 / 65536`).
- Coordinates are **world** coordinates. Y grows **upward**, and the camera scrolls up through the stage.
- The camera is stored in the scratchpad:
  - `0x1f80002c`: camera X.
  - `0x1f800030`: camera Y.

  Subtract the camera from a world position to get a camera-relative one.
- The ship is kept within camera + `[8, 232] x [16, 304]` (code at `0x8005a22c`). The playfield is
  240 wide and 320 tall.
- The playfield is drawn **squeezed 3/4 vertically** into the 240-line picture. On the 368x240 frame:
  - `screen_x ≈ rel_x + 58`
  - `screen_y ≈ (316 - rel_y) * 0.75`

## Player info: `0x800ecf20` (P1) and `0x800ecf60` (P2), 0x40 bytes each

| Offset | Type | Meaning |
|---|---|---|
| +0x00 | u8 | 1 while the player is in the game (also on the continue screen) |
| +0x02 | u8 | 1 from being shot down until the next ship flies in |
| +0x04 | u32 | pointer to this player's ship object |
| +0x18 | u8[] | bombs in stock, one byte per bomb (the value is the bomb kind), 0 = empty slot |
| +0x20 | u8 | lives |
| +0x28 | u32 | score |
| +0x38 | u32 | bomb timer (non-zero while a bomb is going off) |

Ship slots are handed out in join order. If player 2 joins first, P2's ship is `0x800f799c`.
**Always reach the ship through `+0x04`.**

## Objects

Every object type shares the header below.

| Offset | Type | Meaning |
|---|---|---|
| +0x04, +0x08 | s32 16.16 | world x, y |
| +0x0c, +0x10 | s32 16.16 | velocity. For ships this is the move speed (2.0). |
| +0x2e | u16 | flags |
| +0x31 | u8 | update/draw layer. `0x15` air, `0x0b` ground, `0x17` items, `0x1a` enemy bullets, `0x1b` ships. |
| +0x33 | u8 | class. `0x09` air enemy, `0x0a` ground enemy, `0x0b` bullet, `0x0c` item, `0x0d` ship. Boss parts are class 0 on layer `0x15`. |
| +0x34 | u32 | subtype. For items, this is which pickup. |
| +0x38, +0x3c | ptr | pool links: prev, next |
| +0x44 | ptr | next object in an update list (`0x800526f8` walks it) |
| +0x4c, +0x50 | ptr | per-frame handlers. 0 means none. `0x8004dc30` is the sprite animator. |
| +0x60 | ptr | ships only: player info |
| +0x80 | ptr | script resume address (coroutine-style enemy scripts) |

**Pools.** `0x8007bf90` holds `{start, count}` pairs, and `0x80083f8c` lists the pool starts.

- Each pool is a doubly linked list.
- A pointer called the **free head** marks where the free slots begin.
- The **live** objects are the ones reached by walking `prev` (+0x38) from the free head. The walk
  does not include the free head itself.

| Free-head pointer at | Pool start | Count x size | Contents |
|---|---|---|---|
| `0x80101400` | `0x800f799c` | 2 (+1) x 0x128 | ships |
| `0x80101404` | `0x800f7d14` | 44 x 0x128 | enemies (air and ground) |
| `0x80101414` | `0x800fb11c` | 10 x 0x128 | pickups (class `0x0c`) and score pop-ups (handler 0) |
| `0x80101418` | `0x800fbdd4` | 8 x 0x128 | group/controller objects (handler `0x800526f8`) |
| `0x801013a8` | `0x800eda18` | 36 x 0x90 | player shots |
| `0x801013ac` | `0x800eeee8` | 36 x 0x90 | player shots (second player / weapon) |
| `0x801013b0` | `0x800f03b8` | 30 x 0x90 | **enemy bullets** (handlers `0x80058ac4`, `0x80058a68`) |
| `0x801013b4` | `0x800f39b8` | 100 x 0x90 | explosions, debris, ground marks |
| `0x801013b8` | `0x800f1528` | 64 x 0x90 | rarely used; class-`0x0b` entries are treated as bullets |

## Functions

| Address | What |
|---|---|
| `0x8005a18c` | Ship update: `0x8005a22c` moves and clamps it. This is the first Modified-mode hook. |
| `0x8004dc30` | Sprite animation handler, shared by most visible objects |
| `0x800526f8` | Walks an object list (`+0x44`) and calls each object's `+0x4c` and `+0x50` handlers |
| `0x800227d8` and nearby | Enemy scripts. They resume at the address stored in `+0x80`. |

## Still unknown (needed for the weapons work)

- Where the current weapon type and power level live. The ship object and the player-info block
  are the first places to diff while collecting a pickup.
- Collision box sizes. The bot uses a 9 px radius for bullets and 18 px for air enemies, both
  measured by trial.
- Enemy hit points.

## How this was found

`tools/node-harness.mjs` can load a hook module (`HOOK=file.mjs`) that reads RAM every frame
through `ctx.ram`, overrides pads and saves screenshots. The steps were:

1. Diff RAM while the ship is moved left and right, which finds X at `0x800f79a0`.
2. `./raiden --watch 800f79a0` (native, `PSX_RAIDEN2=1`) shows the code that writes X, and with it
   the object base and the camera.
3. Scan RAM for pointers into the object area, which turns up the pool tables and free heads.
4. Walk the live objects and draw them over screenshots to tell enemies, bullets and pickups apart.
5. Diff the player-info block across a bomb and a player 2 join.
