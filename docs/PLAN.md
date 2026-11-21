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
| road geometry: which records to draw (composers) | 68000 | ~80 tiles x a few tests per tick |
| road rasterisation: span fills, ship shadow | **GPU** | the only per-pixel work in the game |
| backdrop, road layers, ship, dashboard | **Object Processor** | layers instead of repainting |
| layer clears | Blitter | one large fill per layer per frame |
| SFX, music (OPL2 synthesis), engine hum | **DSP** | the hum follows the speed: it has to be synthesised live |

### The screen

The viewport is ONE 320x138 8-bit frame buffer, double-buffered, composed in
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
- **M3 — playable.** Pad input, all 30 roads, death/explosion, road complete,
  dashboard gauges (fuel, oxygen, speed, gravity, jump-o-master).
- **M4 — sound.** SFX through the DSP PCM driver (Keen Dreams); music as OPL2
  synthesised on the DSP; the engine hum.
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

- A false branch object falls through to the NEXT PHRASE, not to its link:
  the two head branches and the first object must be adjacent (M0, hardware).
- The dashboard is transparent only over the viewport (rows 129..137); below,
  its zero pixels show palette entry 0. One transparent object let the BG
  register through (M1, hardware): two objects over the same pixels.
- BigPEmu's frame-event script did not run here (no frame ever reached); the
  hardware, with the BG-register stage colours, was the faster instrument.
