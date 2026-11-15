// The frame drawn with pmd-red-3ds's renderer (pmd/ppu.c, pmd/bgcache.c):
// line by line, with the VCount and HBlank interrupts and the HBlank DMAs in
// between, in the same order sa3's own renderer has them (ppu_sdl.c), and
// the renderer's frame setup first, as pmd-red-3ds's frame.c does it.
// Also: a frame not drawn (frame skip), and the drawing profile.

#include <stdint.h>

#include "global.h"
#include "core.h"
#include "gba/defines.h"
#include "gba/io_reg.h"
#include "platform/shared/dma.h"
#include "port.h"

extern IntrFunc gIntrTable[16];

// pmd/ppu.c's profile (pmd_glue.c): the renderer's parts, summed
extern uint64_t (*gPpuTicks)(void);
extern uint64_t gPpuProf[3];

extern uint32_t gPpuStat[STAT_COUNT];

static uint64_t sProf[PROF_COUNT];

void Port_TakeStats(uint32_t out[STAT_COUNT])
{
    for (int i = 0; i < STAT_COUNT; i++) {
        out[i] = gPpuStat[i];
        gPpuStat[i] = 0;
    }
}

void Port_SetProfiling(uint64_t (*ticks)(void))
{
    gPpuTicks = ticks;
}

void Port_TakeProfile(uint64_t out[PROF_COUNT])
{
    sProf[PROF_BG] = gPpuProf[0];
    sProf[PROF_OBJ] = gPpuProf[1];
    sProf[PROF_COMPOSE] = gPpuProf[2];
    for (int i = 0; i < PROF_COUNT; i++)
        out[i] = sProf[i];
    for (int i = 0; i < PROF_COUNT; i++)
        sProf[i] = 0;
    gPpuProf[0] = gPpuProf[1] = gPpuProf[2] = 0;
}

static inline void LineStart(int line)
{
    REG_VCOUNT = line;
    if (((REG_DISPSTAT >> 8) & 0xFF) == REG_VCOUNT) {
        REG_DISPSTAT |= INTR_FLAG_VCOUNT;
        if (REG_DISPSTAT & DISPSTAT_VCOUNT_INTR)
            gIntrTable[INTR_INDEX_VCOUNT]();
    }
}

static inline void LineEnd(void)
{
    REG_DISPSTAT |= INTR_FLAG_HBLANK;
    RunDMAs(DMA_HBLANK);
    if (REG_DISPSTAT & DISPSTAT_HBLANK_INTR)
        gIntrTable[INTR_INDEX_HBLANK]();
    REG_DISPSTAT &= ~INTR_FLAG_HBLANK;
    REG_DISPSTAT &= ~INTR_FLAG_VCOUNT;
}

void Ppu_RunFrameUndrawn(void)
{
    for (int line = 0; line < DISPLAY_HEIGHT; line++) {
        LineStart(line);
        LineEnd();
    }
}

#if !SA3_3DS_WIDE

// pmd/port.h and pmd/bgcache.h (not included: they're plain stdint C, and
// sa3's headers name the same memory differently)
void Ppu_BeginFrame(void);
void Ppu_RenderLine(int line, uint16_t *dst);
void BgCache_Prepare(const uint8_t *regs, int wide);

void Ppu_DrawFramePmd(uint16_t *out)
{
    uint64_t (*ticks)(void) = gPpuTicks;
    uint64_t t0 = ticks ? ticks() : 0;
    BgCache_Prepare(REG_BASE, 0); // (before the first line)
    Ppu_BeginFrame();
    uint64_t t1 = ticks ? ticks() : 0;
    sProf[PROF_PREPARE] += t1 - t0;

    for (int line = 0; line < DISPLAY_HEIGHT; line++) {
        uint64_t a = ticks ? ticks() : 0;
        LineStart(line);
        uint64_t b = ticks ? ticks() : 0;
        Ppu_RenderLine(line, &out[line * DISPLAY_WIDTH]);
        uint64_t c = ticks ? ticks() : 0;
        LineEnd();
        uint64_t d = ticks ? ticks() : 0;
        sProf[PROF_LINES_OTHER] += (b - a) + (d - c); // the interrupts and DMAs
    }
}

#endif
