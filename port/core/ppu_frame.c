// The frame drawn with pmd-red-3ds's renderer (pmd/ppu.c, pmd/bgcache.c):
// line by line, with the VCount and HBlank interrupts and the HBlank DMAs in
// between, in the same order sa3's own renderer has them (ppu_sdl.c), and
// the renderer's frame setup first, as pmd-red-3ds's frame.c does it.

#include <stdint.h>

#include "global.h"
#include "core.h"
#include "gba/defines.h"
#include "gba/io_reg.h"
#include "platform/shared/dma.h"
#include "port.h"

#if !SA3_3DS_WIDE

extern IntrFunc gIntrTable[16];

// pmd/port.h and pmd/bgcache.h (not included: they're plain stdint C, and
// sa3's headers name the same memory differently)
void Ppu_BeginFrame(void);
void Ppu_RenderLine(int line, uint16_t *dst);
void BgCache_Prepare(const uint8_t *regs, int wide);

void Ppu_DrawFramePmd(uint16_t *out)
{
    BgCache_Prepare(REG_BASE, 0); // (before the first line)
    Ppu_BeginFrame();

    for (int line = 0; line < DISPLAY_HEIGHT; line++) {
        REG_VCOUNT = line;
        if (((REG_DISPSTAT >> 8) & 0xFF) == REG_VCOUNT) {
            REG_DISPSTAT |= INTR_FLAG_VCOUNT;
            if (REG_DISPSTAT & DISPSTAT_VCOUNT_INTR)
                gIntrTable[INTR_INDEX_VCOUNT]();
        }

        Ppu_RenderLine(line, &out[line * DISPLAY_WIDTH]);

        REG_DISPSTAT |= INTR_FLAG_HBLANK;
        RunDMAs(DMA_HBLANK);
        if (REG_DISPSTAT & DISPSTAT_HBLANK_INTR)
            gIntrTable[INTR_INDEX_HBLANK]();
        REG_DISPSTAT &= ~INTR_FLAG_HBLANK;
        REG_DISPSTAT &= ~INTR_FLAG_VCOUNT;
    }
}

#endif
