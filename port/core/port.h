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
// The level's time (as the HUD shows it), in frames; -1 outside a level.
int Port_LevelTime(void);
// A line describing what the game is doing right now (frame, which step of
// the frame loop, scanline, the running task), for the backend's watchdog.
// Only reads memory: safe to call from another thread while the game hangs.
void Port_Describe(char *buf, int size);

// --- Renderer (ppu.c) ---------------------------------------------------------

// Draws a whole frame, line by line, running the HBlank/VCount interrupts
// and HBlank DMAs between lines as the GBA does. `out` is DISPLAY_WIDTH x
// DISPLAY_HEIGHT GBA colours (BGR555; bit 15 not meaningful).
// Ppu_DrawFramePmd: pmd-red-3ds's renderer (ppu_frame.c, pmd/), 240x160 only.
// Ppu_DrawFrameSdl: sa3's own (ppu_sdl.c), any size.
void Ppu_DrawFramePmd(uint16_t *out);
void Ppu_DrawFrameSdl(uint16_t *out);
// A frame not drawn (frame skip): the same line-by-line interrupts and HBlank
// DMAs, so the game's mid-frame effects run as always, but no pixels.
void Ppu_RunFrameUndrawn(void);

// --- Profiling (only while switched on: it costs a little) ----------------------
// Where drawing time goes, summed since the last Port_TakeProfile: the parts
// below, in ticks of the function given.
enum { PROF_BG, PROF_OBJ, PROF_COMPOSE, PROF_PREPARE, PROF_LINES_OTHER, PROF_COUNT };
void Port_SetProfiling(uint64_t (*ticks)(void)); // NULL: off
void Port_TakeProfile(uint64_t out[PROF_COUNT]);
// ... and the renderer's counts since the last call (same order as
// pmd/port.h's PPU_ST_*): lines; lines with alpha / brighten / darken; of
// those, effects that change nothing; lines with windows; with
// semi-transparent sprites; sprite rows, their pixels, rotated sprite rows.
enum {
    STAT_LINES, STAT_ALPHA, STAT_BRIGHTEN, STAT_DARKEN, STAT_NO_EFFECT, STAT_WINDOWS, STAT_SEMI,
    STAT_OBJ_ROWS, STAT_OBJ_PIXELS, STAT_OBJ_AFFINE_ROWS,
    STAT_BG_CACHED, STAT_BG_DIRECT, STAT_BG_MOSAIC, STAT_BG_AFFINE, STAT_COUNT
};
void Port_TakeStats(uint32_t out[STAT_COUNT]);

// --- Frame dump ------------------------------------------------------------------
// The next drawn frame's video state, for tools/test/render_dump on the
// computer: Backend_SaveDump gets it when the frame is drawn. Layout (all
// little-endian): "SA3D", u32 version (1), u32 width, u32 height, then VRAM
// (0x18000), the palette (0x400) and OAM (0x400) as the frame began, the
// display registers (the first 0x60 bytes of the I/O registers) as each line
// was drawn (height x 0x60), and the palette as the frame ended (0x400: the
// game may change it during the frame).
#define DUMP_REGS 0x60
void Port_RequestDump(void);
void Backend_SaveDump(const void *data, uint32_t size);

// --- Backend (implemented once per platform) ------------------------------------

// The frame's drawing starts now (the performance meter: game logic until
// here, drawing from here to Backend_Present).
void Backend_DrawStart(void);
// The performance meter's other marks: what ran since the last mark.
enum {
    MARK_GAME,     // the game's code (a frame not drawn: Backend_DrawStart isn't called)
    MARK_UNDRAWN,  // a frame not drawn: its lines' interrupts and DMAs
    MARK_VBLANK,   // the VBlank interrupt (the game's handler) and VBlank DMAs
    MARK_SAVE,     // the save image's check
};
void Backend_Mark(int what);
// Runs fn(arg) on another core, if there is one for it: false if not (the
// caller then runs it itself). Backend_WaitWorker waits for it to end.
bool Backend_StartWorker(void (*fn)(void *), void *arg);
void Backend_WaitWorker(void);
// Whether to draw this frame (frame skip): asked before each frame; when
// not, Backend_DrawStart and Backend_Present are not called for it.
bool Backend_ShouldDraw(void);
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
extern const int Backend_Version5_UpdateRepoIfMissing;

#endif
