# Plan

## How the port is put together

sa3 already has a portable build (`PORTABLE=1`): C replacements for its
assembly, a C m4a sound mixer, the GBA's RAM and registers as plain arrays.
Its SDL build adds one platform file, `src/platform/pret_sdl/sdl2.c`, which
does the rest: the frame loop, the GBA BIOS calls, a software renderer, sound
and input through SDL.

sa3-3ds adds a `PLATFORM=3ds` to sa3's Makefile (`patches/sa3.patch`) and
replaces `sdl2.c` with:

- `port/core/platform.c`: the frame loop (`VBlankIntrWait`: draw the frame
  line by line, running HBlank/VCount interrupts between lines; show it; the
  VBlank interrupt), the BIOS functions (`sdl2.c`'s), the sound hand-off
  (float to 16-bit) and the save image.
- `port/core/ppu.c`: `sdl2.c`'s renderer.
- `port/3ds/main.c`: the libctru backend (screen, DSP sound, buttons, SD card).

The patch also:

- assembles the songs with their GOTO/PATT jump targets self-relative
  (`tools/port_song.py`; the mixer adds the address back), since `.3dsx`
  can't relocate the unaligned pointers songs keep there (same fix as
  pmd-red-3ds);
- aligns the data objects' sections to 4 bytes (the 3DS CPU faults on
  misaligned multi-word accesses that the GBA ignores);
- fixes two declarations that only matched on 64-bit hosts (`RunDMAs`,
  `sub_80AE110`: an enum against `s32`/`u32`, which are `long` on 32-bit
  ARM);
- adds the 3DS picture size to `include/gba/defines.h` (240x160, or 400x240
  with `WIDE=1`).

## Checked so far (without a ROM or devkitPro)

- With a zero-filled stand-in ROM, the whole game half builds with
  `PLATFORM=3ds` (arm-none-eabi GCC 14): 0 warnings, 0 errors after the two
  fixes above.
- `sa3-game.o`'s undefined symbols are only the backend's `Backend_*`
  functions and the C library.
- No absolute relocation in it is at an unaligned address (3dsxtool's
  limit), so the songs' rewrite covers everything.
- `port/3ds/main.c` compiles against libctru's headers, and linked with
  `sa3-game.o` leaves only libctru and C library symbols undefined.

Not checked: the final devkitARM link, 3dsxtool, and anything at run time.

## Stages

1. **Boot** (this): runs at 240x160 on the top screen, with sound, via the
   CPU-copied picture. First test on hardware: does it boot, how fast is it
   (`debug.txt` → `port.log`: game/draw ms per frame) on Old and New 3DS.
2. **Speed**, renderer first, guided by `port.log`:
   - draw backgrounds a tile at a time instead of a pixel at a time;
   - list each line's sprites once per frame instead of checking all 128 on
     every line; a fast path for unrotated sprites;
   - skip layer clears and window/blend work when they're off;
   - present on the GPU (citro3d) with scaling, as pmd-red-3ds does, instead
     of the CPU copy;
   - the sound mixer on the second core (Old 3DS: the system core).
   If that isn't enough on Old 3DS, a GPU renderer like pmd-red-3ds's
   drawing list.
3. **Saves**: give the portable build a flash chip (an `agb_flash` patch that
   writes `FLASH_BASE` directly), so the port's save file is used.
4. **Widescreen** (`WIDE=1`): 400x240 1:1 on the top screen, on New 3DS.
5. **Extras**: the bottom screen, options, a ROM-based builder.

## Renderer notes (from reading `sdl2.c`)

- Video modes 2-5 aren't drawn (logged once); nor 2-D sprite tile mapping.
- Mosaic on rotated backgrounds is approximate.
- Fixed here: object-window sprites now mask every sprite, not only those
  drawn after them in OAM order.
