# JagSkyroads

SkyRoads (Bluemoon Interactive, 1993) on the Atari Jaguar.

This repository contains **only tools and engine code — no game data.** You
supply your own copy of the freeware release; the build converts it into a
Jaguar cartridge image on your machine. Nothing it produces (`cart/`,
`build/`) is meant to be committed or shared.

The whole machine works: the 68000 runs the game logic and the menus, the
GPU draws the road, the Object Processor shows it, the DSP synthesises the
original AdLib music (an OPL2 written in DSP code) and plays the effects.
The intro, menus and road map follow the original program's own code; the
physics replay the original attract demo tick for tick.

## Build

Windows. Requirements:

- Python 3 with Pillow and numpy (pyusb and pyserial too for the GameDrive
  tools);
- [RMAC](https://rmac.is-slick.com/) and Atari's `jaguar.inc`;
- MSYS2's MinGW-w64 gcc (the PC-side checks);
- Docker, with an image named `marsdev-32x` that provides `m68k-elf-gcc` in
  `/marsdev/mars/m68k-elf/bin` (a [marsdev](https://github.com/andwn/marsdev)
  build; the C is compiled with GCC 15.2).

`build.bat` looks for them in this machine's folders; set `RMAC` (the
executable), `JAGINC` (the folder holding `jaguar.inc`) and `MINGW` (MinGW's
`bin`) to point elsewhere.

```
build.bat path\to\skyroads.zip
```

The argument is the freeware release's zip as downloaded, or a folder with
its files. Output: `build\skyroads.j64` (4 MB).

The build checks itself as it goes: the attract demo against a PC run of the
same physics, the GPU and DSP binaries in an interpreter against their C
models. Two more checks compare with skyroads-port itself; they run when its
source, at commit `d865dfe1`, is unpacked into `ref\skyroads-port-main`, and
are skipped otherwise.

## Running

On a Jaguar with a GameDrive on USB:

```
python tools\jaggd.py run build\skyroads.j64
```

The cartridge EEPROM, where the settings and the completed roads are saved,
is kept on the memory card as `skyroads.e2p` (`--eeprom NAME` for another
file). Copied to the memory card and started from the GameDrive's menu, the
ROM saves without any of this.

## Controls

D-pad steers and throttles (arrows in the menus); A, B or C jump; A/B confirm
in the menus, C or OPTION go back (ESC); OPTION in a road returns to the road
map; PAUSE pauses.

## Credits

- SkyRoads — Bluemoon Interactive. All game content is theirs, and none of
  it is in this repository.
- File formats, game logic and the reference reimplementation: the
  [skyroads-port](https://github.com/ammaarreshi/skyroads-port) project
  (Ammaar Reshi, MIT License); its `play.c` is compiled for the 68000 with
  its licence notice (`csrc/port/`).
- The cartridge EEPROM routines are Atari's (Dave Staugas, 1993), as shipped
  to Jaguar developers.

## License

Our code is MIT-licensed ([LICENSE](LICENSE)). The parts that are not ours
keep their own terms:

- `csrc/port/` (play.c, play.h, sr.h, assets.h) is skyroads-port's code, MIT
  License, Copyright (c) 2026 Ammaar Reshi
  ([its licence](csrc/port/LICENSE-skyroads-port.txt)); `tools/lzs.py`
  follows its decompressor (attribution in the file).
- `src/eeprom.s` is Atari's EEPROM routines (Dave Staugas, Atari Corp., 1993),
  distributed to Jaguar developers.
- SkyRoads itself, and everything the build converts, is Bluemoon
  Interactive's and is not in this repository.

Status and design: [docs/PLAN.md](docs/PLAN.md).
