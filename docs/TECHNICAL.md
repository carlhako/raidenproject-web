# The Raiden Project (PSX, SCUS-94402) — native Linux build

The PlayStation game *The Raiden Project* (USA), statically recompiled from its MIPS R3000
executables to C and running natively on x86-64 Linux with SDL2.

The game logic is native code generated from the executables on the disc. The PlayStation
hardware it talks to is supplied by a small runtime: GPU (software renderer), SPU, CD-ROM with XA
streaming audio, MDEC, GTE, DMA, timers, SIO pad port and a high-level emulation (HLE) of the BIOS.
No BIOS image or emulator is needed.

## Build and run

Requirements: gcc, make, python3, SDL2 (found via `sdl2-config`/`pkg-config`, or override with
`SDL_CFLAGS=` / `SDL_LIBS=`) and zlib.

```bash
make -j$(nproc)
```

```bash
./raiden
```

At start-up the binary looks next to itself for:

- `disc/Raiden Project, The (USA).bin`, the original disc image.
- `extracted/SCUS_944.02`, the boot executable, extracted with `tools/extract_iso.py`.

You can pass another disc image path as an argument, or use `--data <dir>` to set the data
directory. Saves go to `memcard/bu00/` in the data directory.

## How it was made

### 1. Extraction

`tools/extract_iso.py` reads the MODE2/2352 image's ISO9660 filesystem. The disc holds:

| File | Role |
|------|------|
| `SCUS_944.02` | Launcher ("The Raiden Project" menu, settings, high scores). Entry `0x8003e6a0`. |
| `JJ2X/PSJJ2H.EXE`, `JJ2X/PSJJ2V.EXE` | *Raiden*, two builds of the same program (H and V screen orientation). |
| `JJ3X/PSJJ3.EXE` | *Raiden II*. |
| `JJ2X/*`, `JJ3X/*` | Level data (`.PIM` images), music (`SEQ`/`VAB`) and, for Raiden II, per-stage `R?M.DAT` files that contain code. |

The launcher starts a game with `cd("cdrom:\JJ2X")` and `LoadExec`/`Load`+`Exec`, so the whole
program image is replaced. The recompiler therefore treats each executable as its own *program*
(`prog_scus`, `prog_jj2h`, `prog_jj2v`, `prog_jj3`) and the runtime picks the right one by CRC of
the image that was loaded into RAM (`exec_program` in `runtime/core.c`).

### 2. Analysis with REA (Reverse Engineer Anything)

[REA](https://github.com/morluto/rea) with its Ghidra provider was used as follows:

- `tools/psxexe2elf.py` wraps a PS-X EXE as a MIPS32 little-endian ELF (REA's Ghidra MIPS lane
  only accepts the legacy `EF_MIPS_ARCH_32` header).
- `rea search <elf> --kind procedures` produced the function inventories
  (`analysis/rea_scus.txt`, `rea_jj2h.txt`, `rea_jj2v.txt`, `rea_jj3.txt`, converted by
  `tools/rea_funcs.py`). They seed the recompiler and bracket each program's code range.
- `tools/rea_mcp.py` drives REA's MCP server (`rea --mcp`) so one Ghidra session serves many
  `batch_decompile` calls. Decompilations answered the questions that blocked the port:
  - **The task launcher** (`0x8001d8b4` in Raiden, `0x800426a4` in Raiden II): saves
    `ra/s0..s7/sp/fp` in globals, runs `fn(obj)` on the same stack and restores them. A second
    entry point (`0x8001d918` / `0x800426c8`) restores the saved context from *deep inside* the task
    and returns to the launcher's caller. Native code cannot resume a discarded frame, so
    `analysis/patch_*.c` replace the pair with a `setjmp`/`longjmp` version.
  - **The scripting VM** (`0x8002509c`): `do p = table[*p](obj, p + 1); while (1)`. Its handlers
    and the object update loops showed which function pointers are resume points in the middle of
    functions.
  - **Object update loops** in Raiden II (`0x800526f8`): objects have per-state function
    pointers (`+0x4c`, `+0x50`, `+0x70`) and a handler is sometimes *called* with `jr $t9` after
    loading `$ra` with an address inside the caller. See "The `jr` call" below.
  - **PsyQ libraries:** libetc interrupts (`setjmp` + `SetCustomExitFromException`), libcd,
    libgpu, the old BIOS pad API and libcard's kernel patch.

### 3. Static recompilation (`tools/recomp.py <name> <exe>`)

- **Function discovery:** recursive descent from the REA entries, every `jal` target, code
  pointers found in data and `lui`/`addiu` pairs, code after `jr ra` gaps, and
  `analysis/extra_<name>.txt` (addresses the interpreter had to handle in a test run).
- **One C function per guest function:** `void <name>_XXXXXXXX(void)` working on the global
  register file `R[32]`. Branches become `goto`s with delay slots handled, and a branch into
  another function becomes a tail call.
- **Jump tables** are resolved by constant tracking and become `switch` statements. Other
  indirect jumps and calls go through an address→function table of the current program.
- **Timing:** every basic block adds its instruction count to a virtual 33.8688 MHz clock.
  Function entries and loop back-edges check for due hardware events and interrupts.
- **Overrides:** `analysis/patch_<name>.c` can replace functions with hand-written C (the task
  launcher). The generated body is skipped, the table slot is kept.
- **The `jr` call:** `jr $t9` with `$ra` set to an address in the same function is a call whose
  callee returns into the middle of the caller. The generated code calls the target and then, if
  `R[31]` is not the function's own entry `$ra`, continues at the address the callee returned to
  (`JR_CALL` in `runtime/recomp.h`). The interpreter does the same.
- **Fallback interpreter:** code that was never discovered statically (most importantly Raiden
  II's per-stage code, loaded into RAM at `0x8011c000`) runs in `runtime/interp.c`. Every address
  it had to take is logged, and `tools/notes2extra.py` turns such a log into
  `analysis/extra_*.txt` for the next recompile.

The output is about 283,000 MIPS instructions across about 5,300 functions in `gen/*.c`.

### 4. Runtime (`runtime/`)

| File | Role |
|------|------|
| `core.c` | CPU state, dispatcher, COP0, event scheduler, program switching (`Exec`), start-up |
| `bios.c` | HLE kernel: events, exception/interrupt delivery, old pad API (`B0:15/16`), libc string functions, `cd`/`Load`/`LoadExec`/`Exec` on the disc, memory-card files on the host |
| `hw.c` | Bus, interrupt controller, DMA with completion latency, root counters |
| `gpu.c` | GP0/GP1 and software rasteriser (polygons, sprites, lines, blending, textures, VRAM copies), horizontal display range |
| `cdrom.c` | CD controller (commands, INT1–5, data FIFO) reading the BIN, XA-ADPCM decoding, ISO9660 lookup for the BIOS file calls |
| `spu.c` | 24-voice ADPCM SPU with ADSR, noise, pitch modulation, CD-audio input |
| `mdec.c` | MDEC RLE→IDCT→RGB decoding |
| `gte.c` | Geometry coprocessor |
| `sio.c` | SIO0 with a digital pad on port 1 |
| `interp.c` | Fallback interpreter |
| `platform.c` | SDL2 video/audio/input and 60 Hz NTSC pacing |

The game is NTSC (60 Hz). The recompiled game runs about 8–10× faster than real time on one core,
so frames are paced to 60 Hz.

Details worth knowing:

- The launcher and both games read the pad through the **old BIOS pad API** (`OutdatedPadInitAndStart`
  `B0:15`, `PAD_dr` `B0:16`): the BIOS writes a 32-bit word with the libetc button layout
  (select/start/d-pad in bits 15..8, L2..square in bits 7..0, active low).
- The games draw into a 224-pixel-wide playfield in a 368-pixel mode and rely on the GP1(06h)
  horizontal display range to blank the rest of the line.
- `Exec` clears in-flight DMA, pending CD commands, interrupt flags, kernel events and interrupt
  chains of the previous program (`bios_reset_for_exec`, `cd_reset_for_exec`, `hw_reset_for_exec`).
- Memory-card files are plain files: `memcard/bu00/BASCUS-94402RAIDENP` holds settings and scores.

## Testing and debugging

Headless runs are scriptable:

```bash
PSX_HEADLESS=1 PSX_FRAMES=3600 PSX_SHOT_EVERY=300 PSX_INPUT="700:circle*10" ./raiden
python3 tools/contact.py      # tile shots/frame_*.png into shots/contact.png
```

| Variable / option | Effect |
|-------------------|--------|
| `PSX_HEADLESS=1` | No window or audio device; runs unthrottled |
| `PSX_FRAMES=n` | Exit after `n` frames |
| `PSX_SHOT_EVERY=n` | Save `shots/frame_N.png` every `n` frames; `PSX_SHOT_FRAMES="a,b,c"` saves exactly those frames |
| `PSX_INPUT="frame:button[*hold],…"` | Scripted pad input. Buttons: `start select up down left right cross circle square triangle l1 r1 l2 r2` |
| `PSX_WAV=file.wav` | Record the audio output |
| `PSX_DUMPRAM=frame` | Write `shots/ram.bin` at that frame; `PSX_DUMP_ON_FATAL=1` dumps RAM + registers on a fatal error |
| `PSX_LOAD_REMAP="PSJJ2H.EXE=PSJJ2V.EXE"` | Make the launcher load another executable (e.g. to run the panorama build of Raiden) |
| `-v` (repeatable), `-q` | Log level. `-v -v -v` traces BIOS calls, CD commands and DMA |
| `--watch <addr>` | Report every write to a RAM word |
| `make EXTRA=-DTRACE_FUNCS` | Keep a ring of the last 128 functions entered and print it on a fatal error; `PSX_TRACE=<func>` logs arguments |
| `make EXTRA=-DCHECK_SP` | Report calls that return with a different `$sp` (found the `jr` call bug) |
| `PSX_INTERP_INDIRECT=1` | Self-test: route every indirect call through the interpreter |

`tools/dis.py <exe> <start> <end>` is a small MIPS disassembler for the extracted executables.
`tools/randinput.py` writes random input scripts, which found most of the bugs listed above.

To extend coverage after changing the runtime, run a long randomised session and feed its log back:

```bash
PSX_HEADLESS=1 PSX_FRAMES=30000 PSX_INPUT="700:circle*10,$(python3 tools/randinput.py 1 1000 30000)" ./raiden -v 2>&1 | python3 tools/notes2extra.py
```

## Status

Verified in headless runs with scripted and randomised input, and briefly in a real window:

- Launcher, settings (including saving to the memory card), high scores, attract demos.
- *Raiden* (both executables) and *Raiden II*: starting a game, stage play, pause/interception
  menu, continue, game over, returning to the launcher and starting the other game.
- Music (SEQ/VAB through the SPU) and sound effects, recorded and checked in WAVs.

Not exercised yet: 1P vs 2P with two gamepads, and the end of Raiden II.

The game data comes from your own disc. The generated code in `gen/` is derived from the game's
executables, so don't redistribute it.


## Web port and netplay

- **Raiden II only:** the web `main()` loads `PSJJ3.EXE` (embedded in the wasm) and starts it directly with the BIOS
  directory set to `\JJ3X`. Any later `Load`/`LoadExec` of another executable (interception menu: main menu, switch
  game) is turned into a reload of `PSJJ3.EXE` (`bios_boot_raiden2`, `r2only` in `runtime/bios.c`). Natively,
  `PSX_RAIDEN2=1 ./raiden` does the same.
- **Build:** `make web` compiles `gen/*.c` and `runtime/*.c` (all but `platform.c`) with emcc into `dist/raiden.{js,wasm}`;
  `web/` supplies the page. The recompiler emits `TAILCALL(f())` for inter-function jumps (`musttail` under clang) so the
  small browser stack never grows. Native and wasm runs are deterministic (identical state hashes over 20,000 random-input frames).
- **Threading:** the game never yields, so it runs in a Web Worker (`web/game-worker.js`); video, audio and controls move
  through SharedArrayBuffers, so the page needs cross-origin isolation (headers set by `server/index.js`). Pacing is NTSC 60 Hz.
- **Disc:** the 56 MB image is stored in OPFS and read synchronously by the worker. The CD emulation delivers data sectors
  8× faster than a real double-speed drive (`FAST_CD_SPS` in `runtime/cdrom.c`; XA audio streaming keeps the real rate), so the
  boot loading screen lasts ~2 s instead of ~9 s and stage loads are shorter too.
- **Sessions:** the server keeps an authoritative per-frame input log `(p1 | p2 << 16)` for each game. While the host plays
  alone, it streams its pad per frame and the server appends and fans out. A **watcher** receives the log from frame 0 and
  replays it (the recompiled game cannot snapshot its state, but it is deterministic), running unthrottled and muted while
  more than 20 frames behind, then in real time ~6 frames behind live.
- **Joining:** a watcher caught up to the live game presses Start. The server picks `lockFrom = head + 90`
  and an input delay from the measured round trips. From then on host and player 2 send pad state for frame `n + delay`;
  the server merges both for each frame and broadcasts the confirmed frame to host, P2 and watchers. If P2 leaves the
  host continues (P2 pad idle); a new watcher can take the slot later. If the host leaves the game ends for everyone.
  A Start pulse is injected for the first P2 frames so a P2 joining at the title screen starts a 2-player game.
- **Desync check:** every 120 frames everybody sends a RAM/register hash; the server compares them with the host's.
- **Limits:** about 2.4 hours per game (2^19 frames), 60 games, one memory card snapshot per game (the host's).
- **Testing:** `node tools/node-harness.mjs <disc.bin> <frames> "300:start,3500:2start" [outdir]` runs the wasm headless
  under Node (`2` prefix = pad 2, `HOLD=n`, `RANDOM_INPUT=1`, `SHOT_EVERY=n`, `MODE=1` Modified mode, `HOOK=file.mjs`
  per-frame RAM/pad hook module, see `tools/hooks/bot.mjs`).

## Modified mode and the AI wingman

- **Two modes.** The landing screen starts either **Raiden II** (original) or **Raiden II Modified**.
  - The mode is chosen when the game is hosted. The server stores it, the lobby shows it, and
    watchers receive it, so everybody boots the same game.
  - The worker calls `_psx_set_mode()` before `main`. This sets `mod_mode` (`runtime/mod.c`).
    Natively, `PSX_MOD=1` does the same.
- **Mode-gated hooks** are the groundwork for new weapons, levels and difficulty.
  - Each address in `analysis/hooks_jj3.txt` makes `tools/recomp.py` rename that function's
    generated body to `jj3_<addr>_orig`.
  - It also emits a wrapper in `gen/jj3_hooks.c`. The wrapper calls `mod_jj3_<addr>()`
    (`runtime/mod_jj3.c`) when `mod_mode` is set, and the original otherwise.
  - Original mode therefore runs exactly the old code. Its state hashes are identical to the build
    without hooks.
  - Hooks must be deterministic, because netplay replays only inputs.
- **RAM map:** see [RAM_MAP.md](RAM_MAP.md).
  - `web/mod/ram-map.js` reads players, enemies, enemy bullets and pickups from the game's object
    pools.
  - The worker reads RAM through `_psx_ram_ptr` and `_psx_scratch_ptr`.
- **AI wingman** (`web/mod/bot.js`). It runs on the host only, in Modified games.
  - **Perception.** Every frame it reads the enemy bullets and the air enemies. It measures their
    velocities from the previous frame.
  - **Plan search.** It predicts where everything will be over the next 22 frames and scores
    243 two-step movement plans. A plan costs more for coming near danger and earns credit for
    where it would like to be:
    - under a target, in a different lane from player 1;
    - near a pickup that player 1 is not already taking;
    - low on the screen, away from the walls and from player 1.
  - **Actions.** It takes the first step of the best plan, keeps rapid fire held, and bombs when
    every plan gets hit.
  - **Start.** It presses Start only when it is not in play (title, join, continue), because Start
    pauses a game in progress.
  - **Results.** In headless 30k-frame games it loses about 1 life per 10k frames. It clears
    several stages on its own.
- **Wingman netplay.** The host's worker sends the wingman's pad along with its own, as an 8-byte
  input `[frame][p1][p2]`.
  - The server accepts it only in Modified games, and only for frames that no human player 2 owns.
  - The pad goes into the input log, so watchers replay the wingman without running it.
  - When a watcher presses Start, the server turns the wingman off (`botoff`) and the person takes
    over the same ship. The usual join Start pulse is skipped when player 2 is already in the game.
  - When that person leaves, the wingman comes back if the host still has it switched on.
- **Tuning.**
  - `HOOK=tools/hooks/bot.mjs node --stack-size=984 tools/node-harness.mjs <disc> 30000 "300:start,600:start,900:start,1200:start"`
    plays the wingman headless. It reports frames flown, deaths, bombs and score, and saves a
    screenshot and a state dump at each death.
    - `BOT_CFG='{"margin":18}'` overrides parameters.
    - `BOT_RANDOM=1` gives a random-input baseline.
  - In the browser, `?botdebug` draws what the wingman sees over the game.
