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
- `port/core/pmd/`: pmd-red-3ds's renderer and background cache (its
  `ppu.c`, `bgcache.c`), nearly unchanged; `pmd/port.h` maps its names for
  the GBA's memory to sa3's arrays. `port/core/ppu_frame.c` draws a frame
  with it, the HBlank/VCount interrupts between lines. `make test` checks it
  against pmd-red-3ds's reference renderer, pixel for pixel, on random
  screens (`tools/test/test_renderer.c`).
- `port/core/ppu_sdl.c`: `sdl2.c`'s renderer, still used by `WIDE=1` builds
  (pmd-red-3ds's draws 240-pixel lines only).
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
2. **Speed**, guided by `port.log`. First Old 3DS run (`sdl2.c`'s
   renderer): game 4-6 ms a frame, drawing 50-100 ms, the 1.5x copy 9.6 ms;
   10-18 fps.
   - pmd-red-3ds's renderer (done: decoded-tile caches, whole tile rows,
     fast paths; also draws rotated backgrounds' mode 2, brighten/darken,
     the object window and mosaic properly);
   - present on the GPU (citro3d) with scaling, as pmd-red-3ds does, instead
     of the CPU copy;
   - the sound mixer on the second core (Old 3DS: the system core).
   If that isn't enough on Old 3DS, a GPU renderer like pmd-red-3ds's
   drawing list.
3. **Saves**: give the portable build a flash chip (an `agb_flash` patch that
   writes `FLASH_BASE` directly), so the port's save file is used.
4. **Widescreen** (`WIDE=1`): 400x240 1:1 on the top screen, on New 3DS.
5. **Extras**: the bottom screen, options, a ROM-based builder.

## Renderer notes

pmd-red-3ds's renderer draws modes 0-3 (not 4/5: neither game uses them),
both sprite tile mappings, windows and the object window, blending,
brighten/darken and mosaic. It reads the sprite list once a frame (OAM is
written in VBlank).

`sdl2.c`'s (WIDE=1 only now): modes 2-5 and 2-D sprite tile mapping aren't
drawn; mosaic on rotated backgrounds is approximate.
