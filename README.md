# sa3-3ds

A native Nintendo 3DS port of [SAT-R/sa3](https://github.com/SAT-R/sa3), the
decompilation of *Sonic Advance 3*.

The game's C code is compiled for the 3DS's ARM11 processor instead of running in a
GBA emulator. It uses sa3's own portable build (the one its SDL PC port uses), with
a 3DS platform layer in place of SDL. The approach follows
[pmd-red-3ds](https://github.com/ethangarsia517/pmd-red-3ds).

**Status:** stage 1, untested on hardware. The whole game compiles for the 3DS
CPU and links with the 3DS backend; nothing has run yet. See
[docs/BUILDING.md](docs/BUILDING.md) and [docs/PLAN.md](docs/PLAN.md).

## Goals

1. Boot and be playable from start to finish, with the original 240 × 160 game on the
   top screen (1:1 or 1.5x).
2. Full speed on Old 3DS (the software renderer will need work: docs/PLAN.md).
3. Later: a 400 × 240 widescreen build (`WIDE=1`, using sa3's widescreen support),
   saves, and use of the bottom screen.
4. Later, a shareable setup: a builder that makes the game data from each player's
   own ROM, so no game data is ever distributed.

## Requirements

- Your own *Sonic Advance 3* (Europe) ROM, SHA-1
  `685af3a2dc0f1b3f8922e73ec42c1dc92e210e39`. No ROM or game data is included in this
  repository, and none ever will be.
- [devkitPro](https://devkitpro.org/wiki/Getting_Started) with the 3DS Development
  component.

## Credits

- [SAT-R](https://github.com/SAT-R/sa3) for the sa3 decompilation and its portable
  build.
- Kurausukun, camthesaxman, Pidgey and NT_x86 for the pokeemerald `pc_port` code that
  sa3's SDL layer, and so this port's renderer and BIOS functions, come from.
- pmd-red-3ds and the projects it credits (Twin Embers, pokeemerald-3Ds-dualscreen)
  for the 3DS porting approach.
