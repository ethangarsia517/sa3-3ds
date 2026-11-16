# Building sa3-3ds

The build has two halves, as pmd-red-3ds's does:

1. **Game half** (`make prelink`, Linux or WSL). Builds SAT-R/sa3 with
   `PLATFORM=3ds`: all of the game's C, its data and music, and the port's
   platform core (`port/core`), compiled for the 3DS CPU and partially linked
   into `build/sa3-game.o`. This needs your own ROM and contains game data
   from it: keep it to yourself.
2. **3DS half** (`make 3ds`, devkitPro). Compiles the small 3DS backend
   (`port/3ds/main.c`) and links it with `sa3-game.o` into
   `build/sa3-3ds.3dsx`.

## Game half (Linux or WSL)

Needs an `arm-none-eabi` GCC (the one you use for pmd-red-3ds's `make prelink`
is fine), host GCC/G++, libpng and Python 3, as sa3 itself does.

```sh
git clone https://github.com/ethangarsia517/sa3-3ds
cd sa3-3ds
tools/bootstrap.sh                                  # fetch SAT-R/sa3 at the pinned commit, apply the patch
cp /path/to/your-rom.gba build/upstream/baserom_sa3.gba
make prelink                                        # -> build/sa3-game.o
```

After `git pull`, just `make prelink` again: if `patches/sa3.patch` changed,
it applies it to `build/upstream` again by itself (and builds the game half
from clean, a few minutes). Your ROM stays where it is.

`make prelink WIDE=1` builds the 400x240 widescreen picture instead (sa3's
widescreen support, which fills the 3DS top screen 1:1). It has more visual
bugs than the GBA size and costs about 2.5x the drawing time. Switching
between the two needs `make clean` first.

## 3DS half on Windows (devkitPro)

1. In the devkitPro MSYS2 window:

   ```sh
   pacman -Syu
   pacman -S --needed 3ds-dev git make
   ```

2. Get this repository (anywhere without spaces in the path), put
   `sa3-game.o` in its `build` folder, then:

   ```sh
   make 3ds DEVKITPRO=/opt/devkitpro
   ```

   The result is `build/sa3-3ds.3dsx`.

## Running it

- **Azahar:** File → Load File → `sa3-3ds.3dsx`.
- **3DS:** copy it to `SD:/3ds/sa3-3ds/sa3-3ds.3dsx` and start it from the
  Homebrew Launcher. Sound needs `SD:/3ds/dspfirm.cdc`.

Controls are the GBA's: A (or X) jump, B, L, R (or ZL/ZR), START, SELECT, D-pad
or Circle Pad. **Y** switches the picture between 1:1 and 1.5x. **Touching the
bottom screen** switches frame skip on or off (on at first). **HOME** to quit.

**Frame skip:** the game itself always runs 60 frames a second, so it plays and
sounds at full speed; when there isn't time to draw a frame, it isn't drawn
(at most 3 in a row). The bottom screen shows both rates: Speed (the game's)
and Drawn.

**Video:** the GPU puts the picture on the screen and scales it. To have the
CPU copy it instead (as the first builds did), create an empty
`SD:/3ds/sa3-3ds/cpu-video.txt`.

For a log, create an empty `SD:/3ds/sa3-3ds/debug.txt`: the port then writes
`port.log` next to it, every 5 seconds two lines: the game's and the drawn
frame rates, the milliseconds of game logic and waiting per frame, of drawing
and presenting per drawn frame; then where the drawing time went
(backgrounds, sprites, compositing, the frame's setup, the game's HBlank
interrupts). Full speed is 16.7 ms a frame in all. (The profile costs a
little time itself, so frame rates are a bit lower with `debug.txt`.)
The bottom screen and the log say where the game's sound runs: on core 2
(New 3DS models, alongside drawing) or on the main core. Old 3DS models can
put it on the system core with an empty `SD:/3ds/sa3-3ds/syscore.txt`, but
that slows the system's own services (graphics, sound, SD card) down, and
the first try cost more than it saved.

In a level, each line also gives the level's time as the HUD shows it
(`level 1:23.45`), and any single frame over 40 ms gets a `spike:` line with
the level time and what the frame spent its time on.

**Saves:** not yet. sa3's portable build doesn't save yet (it reports no
flash chip, as its SDL build does). The port loads and writes
`SD:/3ds/sa3-3ds/sa3.sav` as soon as the game writes to its flash image.
