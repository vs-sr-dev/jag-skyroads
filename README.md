# JagSkyroads

SkyRoads (Bluemoon Interactive, 1993) on the Atari Jaguar.

This repository contains **only tools and engine code — no game data.** You
supply your own copy of the freeware release; the build converts it into a
Jaguar cartridge image on your machine.

## Build

Requirements: Python 3 with Pillow, RMAC, Docker image `marsdev-32x`
(m68k-elf-gcc, from milestone M1 on).

```
build.bat path\to\skyroads.zip
```

Output: `build\skyroads.j64`. With a GameDrive on USB:
`python tools\jaggd.py run build\skyroads.j64` (the cartridge EEPROM, where
settings and completed roads are saved, is kept on the memory card as
`skyroads.e2p`; from the memory card itself the GameDrive handles it alone).

## Controls

D-pad steers and throttles (arrows in the menus); A, B or C jump; A/B confirm
in the menus, C or OPTION go back (ESC); OPTION in a road returns to the road
map; PAUSE pauses.

## Credits

- SkyRoads — Bluemoon Interactive. All game content is theirs.
- File formats and game logic reverse engineered by the skyroads-port project
  (Ammaar Reshi, MIT License).

Status and design: [docs/PLAN.md](docs/PLAN.md).
