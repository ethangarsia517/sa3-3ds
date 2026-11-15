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
or Circle Pad. **Y** switches the picture between 1:1 and 1.5x. **HOME** to
quit.

The bottom screen shows the frame rate. For a log, create an empty
`SD:/3ds/sa3-3ds/debug.txt`: the port then writes `port.log` next to it, with
a line every 5 seconds giving the milliseconds per frame spent on game logic,
drawing, copying to the screen and waiting. (Full speed is 16.7 ms in all.)

**Saves:** not yet. sa3's portable build doesn't save yet (it reports no
flash chip, as its SDL build does). The port loads and writes
`SD:/3ds/sa3-3ds/sa3.sav` as soon as the game writes to its flash image.
