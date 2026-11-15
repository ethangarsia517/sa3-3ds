// Interface between sa3-3ds's platform core and a backend.
//
// The core (this folder) is plain C built together with sa3's game code by
// sa3's own Makefile (PLATFORM=3ds): bootstrap.sh links this folder in as
// build/upstream/src/platform/n3ds. It replaces sa3's SDL layer
// (src/platform/pret_sdl/sdl2.c): the GBA BIOS calls, the renderer, the frame
// loop and the sound hand-off. It has no 3DS dependencies; a backend
// (port/3ds/main.c, built with devkitARM) supplies the Backend_* functions.

#ifndef SA3_3DS_PORT_H
#define SA3_3DS_PORT_H

#include <stdbool.h>
#include <stdint.h>

// GBA key bits, active high (A = bit 0 ... L = bit 9), as REG_KEYINPUT's
// inverse.
enum {
    PORT_KEY_A = 1 << 0,
    PORT_KEY_B = 1 << 1,
    PORT_KEY_SELECT = 1 << 2,
    PORT_KEY_START = 1 << 3,
    PORT_KEY_RIGHT = 1 << 4,
    PORT_KEY_LEFT = 1 << 5,
    PORT_KEY_UP = 1 << 6,
    PORT_KEY_DOWN = 1 << 7,
    PORT_KEY_R = 1 << 8,
    PORT_KEY_L = 1 << 9,
};

// --- Core (called by the backend) --------------------------------------------

// Sets up the core and runs the game; does not return.
void Port_Main(void) __attribute__((noreturn));
// The game's picture size (DISPLAY_WIDTH x DISPLAY_HEIGHT: 240x160, or
// 400x240 in a WIDE=1 build).
int Port_DisplayWidth(void);
int Port_DisplayHeight(void);
uint32_t Port_FrameCount(void);

// --- Renderer (ppu.c) ---------------------------------------------------------

// Draws a whole frame, line by line, running the HBlank/VCount interrupts
// and HBlank DMAs between lines as the GBA does. `out` is DISPLAY_WIDTH x
// DISPLAY_HEIGHT GBA colours (BGR555, bit 15 set).
void Ppu_DrawFrame(uint16_t *out);

// --- Backend (implemented once per platform) ------------------------------------

// The frame's drawing starts now (the performance meter: game logic until
// here, drawing from here to Backend_Present).
void Backend_DrawStart(void);
// The finished picture: width x height GBA colours (BGR555).
void Backend_Present(const uint16_t *frame, int width, int height);
uint16_t Backend_PollKeys(void); // PORT_KEY_* bits, active high
// End of a frame: shows it, waits for the screen's refresh, services the OS
// (may exit the program).
void Backend_EndFrame(void);
// One frame of sound: `count` interleaved 16-bit stereo samples at `rate` Hz.
void Backend_QueueAudio(const int16_t *stereo, int count, int rate);
// The GBA's flash save image (128 KiB).
bool Backend_LoadSave(uint8_t *data, uint32_t size);
void Backend_StoreSave(const uint8_t *data, uint32_t size);
void Backend_Log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void Backend_Fatal(const char *msg) __attribute__((noreturn));

// Renamed whenever the backend and the core must change together: linking an
// old port/3ds/main.c with a newer sa3-game.o fails naming this symbol.
extern const int Backend_Version1_UpdateRepoIfMissing;

#endif
