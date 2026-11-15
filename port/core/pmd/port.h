// Stand-in for pmd-red-3ds's port/core/port.h, so its renderer (ppu.c) and
// background cache (bgcache.c) build in sa3-3ds unchanged but for a line or
// two (see pmd_glue.c).
//
// pmd-red-3ds keeps the GBA's memory in arrays named gPortRegs, gPortVram,
// gPortPltt and gPortOam; sa3's portable build has the same memory, laid out
// the same way, named REG_BASE, VRAM, PLTT and OAM (src/platform/shared).
// No sa3 header is included here: these files are plain C on stdint.

#ifndef PMD_PORT_SHIM_H
#define PMD_PORT_SHIM_H

#include <stdbool.h>
#include <stdint.h>
#include "port_view.h"

#define GBA_W 240
#define GBA_H 160

extern unsigned char REG_BASE[];
extern unsigned char VRAM[];
extern uint16_t PLTT[];
extern unsigned char OAM[];
#define gPortRegs REG_BASE
#define gPortVram VRAM
#define gPortPltt ((unsigned char *)PLTT)
#define gPortOam OAM

void Ppu_BeginFrame(void);
void Ppu_RenderLine(int line, uint16_t *dst);
void Ppu_SetView(int wide);
void Ppu_SetLineRegs(const uint8_t *regs);
int Ppu_ListLine(int line);
int Ppu_ListObjects(int lines);

// sa3-3ds's profile (pmd_glue.c): when gPpuTicks is set, Ppu_RenderLine adds
// its time in the backgrounds, the sprites and compositing to gPpuProf[0-2].
extern uint64_t (*gPpuTicks)(void);
extern uint64_t gPpuProf[3];
// ... and counts: lines drawn; lines with a colour effect (alpha, brighten,
// darken), of them how many change nothing; lines with windows; lines with
// semi-transparent sprites; sprite rows drawn, their pixels, rotated rows.
enum {
    PPU_ST_LINES, PPU_ST_EFFECT1, PPU_ST_EFFECT2, PPU_ST_EFFECT3, PPU_ST_EFFECT_NONE, PPU_ST_WINDOWS, PPU_ST_SEMI,
    PPU_ST_OBJ_ROWS, PPU_ST_OBJ_PIXELS, PPU_ST_OBJ_AFFINE_ROWS, PPU_ST_COUNT
};
extern uint32_t gPpuStat[PPU_ST_COUNT];

#endif
