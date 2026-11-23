# JagSkyroads — plan

A port of **SkyRoads** (Bluemoon Interactive, 1993) to the Atari Jaguar.

The repository holds **no Bluemoon content**. The user points the build at their
own copy of the freeware release (the zip as downloaded, or a folder) and the
tools convert it into a cartridge image. Converted blobs (`cart/`), previews and
ROMs (`build/`) are git-ignored.

## Sources

- **skyroads-port** (Ammaar Reshi, MIT): a full reverse engineering of
  `SKYROADS.EXE` and a portable C11 reimplementation whose physics replay the
  original `DEMO.REC` tick for tick. Its `docs/FORMATS.md` and `re/notes/` are
  the specification we work from. Its gameplay core (`play.c`, `tables.c`) is
  integer-only C with no allocation, and is meant to be compiled for the 68000
  as is, with its licence notice kept.
  We use it as a FROZEN reference: github.com/ammaarreshi/skyroads-port at
  commit d865dfe1 (June 2026; no activity since, open PRs unanswered).
  Discrepancies found against the original binary are fixed and documented
  HERE, not waited for upstream (the shadow stencils: its open PR #2 has the
  same fix).
- **Jaguar-KeenDreams** and **Prowl**: the hardware sequences (boot, video
  timing, OP list, interrupt, DSP audio, GameDrive USB) are copied from there,
  never re-derived. They are verified on the user's NTSC Jaguar.

## Rules carried over

- The port reproduces the GAME, not the code: physics, collisions, timing
  (36.0036 Hz tick) and everything a player can feel stay exact; how it is
  drawn and which chip does what is open.
- The whole machine works: the Object Processor composes the layers, the GPU
  rasterises the road, the DSP does the sound, the 68000 runs the game logic.
  Not "it would fit on the 68000".
- One truth, one place: every address comes from `tools/memory_map.py`.
- Build the game first; probes only for a precise open question, switched off
  when it is answered, and on the keypad, never on game controls.

## Who does what

| work | where | why |
|---|---|---|
| game logic, physics, input, menus | 68000 (C blob, from skyroads-port) | integer code, 36 Hz, cheap |
| road geometry: which records to draw (composers) | **GPU** (since session 2; the C stays as the reference) | ~80 tiles, ~6,400 GPU instructions a frame |
| road rasterisation: span fills | **GPU** | the only per-pixel work in the game |
| backdrop, road layers, ship, dashboard | **Object Processor** | layers instead of repainting |
| layer clears | Blitter | one large fill per layer per frame |
| SFX, music (OPL2 synthesis), engine hum | **DSP** | the hum follows the speed: it has to be synthesised live |

### The screen

The viewport is ONE 320x138 8-bit frame buffer, triple-buffered (see the
lessons), composed in
the original's painter's order: GPU (backdrop copy + records before the ship),
68000 (ship, shadow), GPU (records after the ship). The OP shows it and the
dashboard over it (transparent band on rows 129..137, opaque below).

The first M2 design stacked backdrop, road A, ship and road B as OP objects:
pixel-exact offline, but three full-width 8-bit objects on the same line
stopped the real Jaguar dead (blue screen, the 68000 never woke). Two per line
work (M1). Bisected on hardware: GPU off still dead, road objects off alive.

### Time

The VI runs at 59.94 (NTSC) or 50 (PAL) Hz; the game ticks at 36.0036 Hz
through a rational accumulator in the interrupt, as the reference port does.
A new frame is rendered once per tick.

## Milestones

- **M0 — DONE on hardware (session 1).** Converter, memory map, cartridge
  packer, boot from the cartridge, 240p video, OP list with backdrop and
  dashboard, all ten worlds cycling with their palettes.
- **M1 — DONE on hardware (session 1).** C blob (skyroads-port's `play.c`,
  run from DRAM); the attract demo (`DEMO.REC` on road 0) replays on the
  68000 and all 1775 ticks match the PC oracle (`tools/oracle/demo_oracle.c`,
  same `play.c` and `csrc/shared.h`, same cartridge blobs) byte for byte:
  position, speed, fuel, oxygen, ship frame and screen position. The ship is
  an OP object; 36.0036 Hz ticks from a rational accumulator in the VI.
- **M2 — DONE on hardware (session 1).** The 68000 runs the original's
  composers (`csrc/road.c`) and emits a command list; the GPU
  (`src/gpu_road.s`) copies the backdrop and fills the spans; the 68000 draws
  the ship and its shadow between the GPU's two launches; the OP shows one
  320x138 frame buffer plus the dashboard. Offline, the whole frame matches
  skyroads-port's renderer pixel for pixel on all 1775 demo ticks, except
  the shadow, where the port is wrong (below).
- **M3 — DONE on hardware (session 1).** Pad (Keen Dreams' ReadPad), the 30
  roads in sequence, death and retry, road completed, every gauge; keypad
  `*`/`#` step roads (test instrument), `0` shows frame timings. The whole
  screen is one 320x200 frame buffer: Blitter (pristine viewport) -> GPU
  (records before the ship) -> 68000 (ship, shadow) -> GPU (records after)
  -> 68000 (gauges, incremental). Offline: equal to skyroads-port's renderer
  + HUD on the demo and on synthetic sweeps of all 31 roads (35,840 frames).

### Performance (M3, measured on hardware, half-lines of 31.8 us per frame)

| phase | first | after Blitter + incremental HUD + no per-pixel mulsi3 |
|---|---|---|
| physics, per tick | ~70 | ~220 (catch-up inflated) |
| composers (68000) | 500-800 | 620-700 |
| GPU 1 | 650-760 | 150-230 |
| ship + shadow (68000) | 320-430 | 230-250 |
| GPU 2 | 20-190 | 120-390 |
| gauges (68000) | 450-480 | 45-83 |

"Sensibly better, already feels like the DOS version" (user), with drops in
heavy scenes. The 68000 is the limit.

### M3, session 2: the composers on the GPU

- **The composers run on the GPU** (`src/gpu_road.s`, `gc_*`), a call-for-call
  transcription of `road_compose`, which stays in C as the reference. Each
  frame the 68000 writes a small input block (`COMP_IN`: position, frame base,
  the colour quads split per half, the 11 grid rows a frame looks at as
  longs); the first launch composes the whole list, waits for the Blitter
  and fills list 1; it leaves list 2's address in `GPU_CMD` for the second.
  `trekg.bin` became longs only (directory as u32, records as
  `colour << 24 | span block`).
- **Spans are filled with long stores** (bytes to the boundary, longs, bytes).
- **Checks.** `tools/oracle/gpu_check.py` runs the REAL GPU binary (from
  `build/main.bin`) in a JRISC interpreter (`tools/oracle/jrisc.py`, from
  Prowl) on a DRAM image: lists against `road_compose`, and on a sample the
  pixels of both launches against `raster.h`. Every position of every road
  (37,295 frames) matched; `build.bat` runs every 37th (all phases, ~9 s).
  The interpreter makes `movei` change the flags, as the real TOM was seen
  to, so code that relies on flags across a `movei` fails on the PC too. A
  deliberate mutation (`cmpq #4` -> `#3`) is caught. On hardware, keypad `9`
  recomposes every frame on the 68000 and compares: 400+ frames, 0 different.
- **Result: 19-26 fps** shown on hardware (was 8-11). Part of the old figure
  was the instrument itself: its digits cost ~800 half-lines a frame (a
  32-bit multiply per cell is a libgcc call on the 68000), uncounted by any
  phase, and the slower frames then ran 4 ticks of catch-up each. The
  numbers are now drawn once a second into `OVL_BUF` and the Blitter copies
  them into every frame.
- Measured with the GPU composers, before the long stores and the cheap
  overlay (so ticks inflated): input+pose ~120, GPU 1 212-239, ship+shadow
  301-317, GPU 2 106-245, gauges 42-46.

- **Three frame buffers** (`ROAD_VIEW` tells SetView which): with two,
  the frame being drawn was the one still on screen (see the lessons).

### M3, session 3: the JaguarDocs knowledge base applied, the physics

Measured through COM8 (`tools/comlog.py`: one line a second from the
Jaguar, `src/gd.s` + `lg_frame` in csrc/game_main.c), which also sees what
the on-screen numbers cannot: their own cost (~100 half-lines a frame) and
the time no phase covers.

- **GPU rasteriser** (JaguarDocs hw/01, hw/06): spans filled with `storep`
  (every DRAM store costs ~4.7 clk whatever its width), the next span word
  loaded a line ahead (a DRAM load lands ~16 clk later), `G_HIDATA` set
  after the prefetch has landed (every external load rewrites it). The
  interpreter models storep and poisons G_HIDATA on every external load.
  GPU 1 + GPU 2: 187 -> 114 half-lines a frame on the same scene.
- **68000 waits off the bus**: the poll of GPU_DONE pauses with MULUs (a
  68000 polling DRAM slows GPU loads ~3x); the VI handler acknowledges
  first (INT1, then INT2: before it GPU and Blitter cannot touch the bus);
  OBF written once at boot.
- **No VI wait per frame**: the handler puts a finished frame on screen
  itself, the main loop STOPs only when no tick is due, C waits only when a
  buffer is still shown. Peak 27 -> 33 fps.
- **The drops were the physics**, not the drawing: on blocks and tunnels a
  tick cost up to ~700 half-lines (22 ms; 10 fps). `swept_move`'s
  refinements asked `sr_solid` up to ~50 times a grounded tick, each with a
  libgcc `__modsi3`, plus 64-bit `__muldi3`/`__divdi3` every tick. play.c
  now (changes marked `JAG:`): the column phase from a 512-entry table;
  no probe repeated in the same row (z refine) or column (x refine) -
  sr_solid sees z only through z >> 16 and x only through x >> 7; 32-bit
  arithmetic in the coarse sweep. `tools/oracle/play_equiv.c` (build.bat
  step 2b) runs it against the frozen reference on every road: pure
  queries, 200,000 random ship states per road, random games, the demo -
  the whole state equal after every tick (8.7 M ticks); real mutations are
  caught. Worst demo second 10 -> 29 fps; playing, 33-37 fps throughout,
  the slide on a tube roof included.

Where a frame goes now (36 fps, the cap: one frame per tick): ship+shadow
~300, physics ~190, input+pose ~120, GPU ~160, gauges ~46.

- **Ship and shadow on the GPU**, in the same launch as the road: list 1,
  then the ship and its shadow (`gs_*`/`gh_*` in src/gpu_road.s), then
  list 2. The 68000 only writes `SHIP_IN` (road_ship_input: frame row,
  position, the shadow's stencil as 9 row masks made once at boot, one clip
  long per row for the cowl). gpu_check draws a ship and a shadow at varied
  poses on every checked screen, pixel for pixel against road_ship /
  road_shadow_draw (mutations caught). The demo now sits at 35-37 fps in
  every second, with 10-47 % of the time idle.
- **compose_tun_high was wrong in skyroads-port** (roads 8 and 9, seen by
  the user against the original): render.c draws kind 4, the tunnel's arch,
  over the high block with a bore; the EXE (0x2fb0) draws kind 3's split
  tops when the nearer tile is low, as compose_tun_low ends. Fixed in
  road.c and gc_c_tun_high; every other composer re-read against the
  disassembly and equal.

Next, in order: the road-start hitch (~1 s at 7-9 fps, 20 ticks dropped: the dashboard
redrawn from scratch into all three buffers); the ~120 half-lines of input
+ ship pose.
- **M4 — sound.** SFX through the DSP PCM driver (Keen Dreams, verified on
  hardware), resampled at build time to the I2S rate. Music: **an OPL2
  synthesised on the DSP** (9 channels x 2 operators = 18 FM operators),
  fed with MUZAX's register writes at 180 Hz. Chosen over pre-rendering the
  songs to PCM because the engine hum (OPL channels 7-8) follows the ship's
  speed and cannot be recorded in advance, and because 13 songs of PCM
  would weigh on the cartridge. Budget: JagLBA measured ~13 enveloped
  operators at 20.8 kHz on hardware, so the synth runs at ~11-15 kHz (SCLK
  from the I2S formula, never a "familiar" 11025/22050). Code and tables in
  Jerry's 8 KB local RAM (its DRAM path is 16-bit and contended). Oracle:
  the same song rendered on the PC by our synth's C model and by a reference
  OPL emulator, compared sample by sample; the pre-render stays as a
  listening reference only.
- **M5 — the rest of the game.** Intro animation, menus, road selection,
  settings, completed roads saved to the cartridge EEPROM.
- **M6 — distribution.** BYOA package (`make_rom.py` + prebuilt engine),
  optional aspect correction (OP scaled objects: mode 13h pixels are 1.2:1).

## Data that lives in SKYROADS.EXE

skyroads-port's `tables.c` holds tables extracted from the EXE (colour quads,
cowl silhouette, shadow stencils, digit stencils, jump-o-master stamps). They
are Bluemoon data, so they are NOT copied here: when M2/M3 need them,
`tools/sr2jag.py` extracts them from the user's `SKYROADS.EXE` at the data
segment addresses `re/notes/` gives, and checks them. Small gameplay
constants inside `play.c` stay where the reimplementation put them.

## Known lessons (this project)

- skyroads-port's shadow stencils (ds:0x65e) are ROW-major 9x29 (each is the
  ship's silhouette from above), not column-major as its render.c reads them:
  vertical bars instead of a shadow. Seen on hardware by the user; the offline
  check could not see it because it compares WITH the port.
- At most two full-width 8-bit objects per line on the OP (see "The screen").
- The GRAV-O-METER stencils (ds:0x13c) are not glyphs: they are the cells to
  switch OFF over an LCD "8" already in the dashboard art (fn_0fc6, fn_0eb5,
  fn_41e5 disassembled). skyroads-port draws them right.
- On the 68000, `y * 320` with 32-bit ints is a libgcc call per pixel; walk
  row pointers. Instruments drawn on screen must show latched averages
  (numbers changing 36 times a second cannot be read off a TV) and cost
  little themselves (the first version cost ~100 ms a frame).

- A false branch object falls through to the NEXT PHRASE, not to its link:
  the two head branches and the first object must be adjacent (M0, hardware).
- The dashboard is transparent only over the viewport (rows 129..137); below,
  its zero pixels show palette entry 0. One transparent object let the BG
  register through (M1, hardware): two objects over the same pixels.
- A swap of frame buffers takes effect one video interrupt LATE: the
  interrupt arms the OP list from OBJ_SHADOW, and SetView updates the shadow
  after it. With two buffers the next frame was drawn into the one still on
  screen; it showed once frames got fast (road flickering on jumps, session
  2, hardware). Three buffers.
- **The video interrupt can be lost to where the code sits** (session 2,
  hardware). It fires on VC == VI and HC == HDE (bit 10 set), so only if
  hc[10] == VC & 1 holds for an odd VI; InitVideoTiming writes HC = VC = 0
  to lock that, but HC and VC are clocked by vclk (vxclk) and a 68000 write
  can miss the load depending on the cycle it lands on. A 12-byte shift of
  main.s (nothing else changed: the triple-buffering SetView was 12 bytes
  shorter) left the machine on COL_LOOP for ever; bisected with nop padding
  on hardware down to InitVideoTiming / VideoOnMinimal, 8-byte shifts were
  harmless. Fix: `LockVi` reads VC, HC, VC after the video is on and puts VI
  on the parity the counters actually have (VI_FLIP = 1: yellow margins for
  2 s at boot). Verified: the bad alignment re-created on purpose boots, with
  yellow margins. Keen Dreams, Chess and Prowl share the hazard.
- An instrument that draws every frame must be measured too: the timing
  digits cost more than the phases they showed (session 2). Draw once, copy
  with the Blitter.
- skyroads-port's renderer is not the truth either: compose_tun_high drew
  the tunnel's arch over a block (session 3). Our checks compare WITH the
  port, so only the user's eye against the original, then the EXE, can
  catch such errors: read each composer against the disassembly.
- **Measure the 68000 code you did not write before blaming the drawing**
  (session 3): the drops the eye tied to "how much is on screen" were the
  physics probing blocks and tunnels; the on-screen averages even hid it
  (latched seconds, ~2300 half-lines of ticks shown as 338). COM8 lines with
  a total by the clock leave nothing unaccounted.
- A `stop` that waits for an interrupt must never run with the interrupts
  off: frame 1 is drawn by c_init before INT1 is enabled, and an unsigned
  `k - 2` sent it to sleep for ever (green COL_BOOT screen, session 3).
- BigPEmu's frame-event script did not run here (no frame ever reached); the
  hardware, with the BG-register stage colours, was the faster instrument.
