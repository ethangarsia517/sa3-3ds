// Renderer test, on the computer you build on: pmd-red-3ds's renderer as
// sa3-3ds builds it (port/core/pmd: ppu.c, bgcache.c, pmd_glue.c, through
// pmd/port.h's names for sa3's memory) against pmd-red-3ds's reference
// renderer (tools/test/ppu_ref.c), pixel for pixel, on random GBA screens.
//
// Each run is a series of frames, so the background cache is checked across
// frames too: between frames some tiles, map entries, palette colours and
// sprites change; during a frame, registers change every few lines (scroll,
// blending, windows, affine reference points) as HBlank effects do.
//
// build: cc -O2 -o build/test_renderer tools/test/test_renderer.c tools/test/ppu_ref.c
//          port/core/pmd/ppu.c port/core/pmd/bgcache.c port/core/pmd/pmd_glue.c -Iport/core/pmd
// usage: build/test_renderer [runs] [frames per run]   (make test does this)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "port.h"
#include "bgcache.h"

unsigned char REG_BASE[0x400] __attribute__((aligned(4)));
unsigned char VRAM[0x18000] __attribute__((aligned(4)));
uint16_t PLTT[0x200] __attribute__((aligned(4)));
unsigned char OAM[0x400] __attribute__((aligned(4)));

void Ref_BeginFrame(void);
void Ref_RenderLine(int line, uint16_t *dst);

void Backend_Fatal(const char *msg)
{
    fprintf(stderr, "fatal: %s\n", msg);
    exit(2);
}

static uint32_t sSeed;
static uint32_t Rand(void)
{
    sSeed ^= sSeed << 13;
    sSeed ^= sSeed >> 17;
    sSeed ^= sSeed << 5;
    return sSeed;
}
static int Chance(int percent) { return (int)(Rand() % 100) < percent; }

static void W16(unsigned off, uint16_t v) { REG_BASE[off] = (uint8_t)v; REG_BASE[off + 1] = (uint8_t)(v >> 8); }
static void W32(unsigned off, uint32_t v) { W16(off, (uint16_t)v); W16(off + 2, (uint16_t)(v >> 16)); }

// Tile data: mostly sparse pixels (lots of transparency, as real tiles), with
// whole-tile runs; maps: entries with a tile number in a small range, flips
// and palette banks.
static void RandomTiles(unsigned from, unsigned to)
{
    for (unsigned a = from; a < to; a++)
        VRAM[a] = Chance(40) ? 0 : (uint8_t)Rand();
}

static void RandomMap(unsigned base, unsigned bytes)
{
    for (unsigned a = base; a < base + bytes && a + 1 < 0x10000; a += 2) {
        uint16_t e = (uint16_t)((Rand() % 160) | (Rand() & 0xFC00));
        VRAM[a] = (uint8_t)e;
        VRAM[a + 1] = (uint8_t)(e >> 8);
    }
}

static void RandomPalette(void)
{
    for (int i = 0; i < 0x200; i++)
        PLTT[i] = (uint16_t)(Rand() & 0x7FFF);
}

static void RandomOam(void)
{
    for (int n = 0; n < 128; n++) {
        uint16_t a0 = (uint16_t)Rand(), a1 = (uint16_t)Rand(), a2 = (uint16_t)Rand();
        if (Chance(50))
            a0 = (uint16_t)((a0 & ~0x0300) | 0x0200); // disabled
        if (Chance(80))
            a0 &= (uint16_t)~0x0C00; // mostly normal sprites (some semi, window)
        if (Chance(70))
            a0 &= (uint16_t)~0x1000; // mostly no mosaic
        OAM[n * 8 + 0] = (uint8_t)a0; OAM[n * 8 + 1] = (uint8_t)(a0 >> 8);
        OAM[n * 8 + 2] = (uint8_t)a1; OAM[n * 8 + 3] = (uint8_t)(a1 >> 8);
        OAM[n * 8 + 4] = (uint8_t)a2; OAM[n * 8 + 5] = (uint8_t)(a2 >> 8);
    }
    for (int m = 0; m < 32; m++) { // affine matrices
        for (int k = 0; k < 4; k++) {
            uint16_t p = (uint16_t)((Rand() % 0x200) - 0x100);
            if (Chance(30)) p = (k == 0 || k == 3) ? 0x100 : 0;
            OAM[m * 32 + k * 8 + 6] = (uint8_t)p;
            OAM[m * 32 + k * 8 + 7] = (uint8_t)(p >> 8);
        }
    }
}

static void RandomAffine(int i)
{
    W16(0x20 + i * 0x10, (uint16_t)(Chance(40) ? 0x100 : (Rand() % 0x300) - 0x180)); // PA
    W16(0x22 + i * 0x10, (uint16_t)(Chance(40) ? 0 : (Rand() % 0x200) - 0x100));     // PB
    W16(0x24 + i * 0x10, (uint16_t)(Chance(40) ? 0 : (Rand() % 0x200) - 0x100));     // PC
    W16(0x26 + i * 0x10, (uint16_t)(Chance(40) ? 0x100 : (Rand() % 0x300) - 0x180)); // PD
    W32(0x28 + i * 0x10, (Rand() % 0x40000) - 0x10000);
    W32(0x2C + i * 0x10, (Rand() % 0x40000) - 0x10000);
}

static void RandomRegs(void)
{
    int mode = (int)(Rand() % 10);
    mode = mode < 6 ? 0 : mode < 9 ? 1 : 2; // mostly mode 0 (as sa3), some 1 and 2
    uint16_t dispcnt = (uint16_t)(mode | (Rand() & 0x7F00)); // BGs, OBJ, windows
    if (Chance(50)) dispcnt |= 0x40; // 1-D sprite tiles (sa3's)
    if (Chance(60)) dispcnt &= (uint16_t)~0x6000; // windows 0/1 off
    if (Chance(70)) dispcnt &= (uint16_t)~0x8000; // object window off
    W16(0x00, dispcnt);
    for (int b = 0; b < 4; b++) {
        uint16_t cnt = (uint16_t)(Rand() & 0xFFCF);
        cnt = (uint16_t)((cnt & ~0x1F00) | ((8 + (Rand() % 24)) << 8)); // map bases above the first 16K
        if (Chance(70)) cnt &= (uint16_t)~0x40; // mostly no mosaic
        if (Chance(70)) cnt &= (uint16_t)~0x80; // mostly 16 colours
        cnt = (uint16_t)((cnt & ~0x0C) | ((Rand() % 2) << 2)); // tile data in the first 32K
        W16(0x08 + b * 2, cnt);
        W16(0x10 + b * 4, (uint16_t)(Rand() & 0x1FF));
        W16(0x12 + b * 4, (uint16_t)(Rand() & 0x1FF));
    }
    RandomAffine(0);
    RandomAffine(1);
    W16(0x40, (uint16_t)Rand()); W16(0x42, (uint16_t)Rand()); // WIN0H, WIN1H
    W16(0x44, (uint16_t)Rand()); W16(0x46, (uint16_t)Rand()); // WIN0V, WIN1V
    W16(0x48, (uint16_t)(Rand() & 0x3F3F)); W16(0x4A, (uint16_t)(Rand() & 0x3F3F));
    W16(0x4C, Chance(80) ? 0 : (uint16_t)Rand()); // MOSAIC
    W16(0x50, (uint16_t)(Rand() & 0x3FFF));        // BLDCNT
    W16(0x52, (uint16_t)(((Rand() % 17) << 8) | (Rand() % 17)));
    W16(0x54, (uint16_t)(Rand() % 17));
}

// What an HBlank effect might change on a line.
static void LineChange(void)
{
    switch (Rand() % 5) {
    case 0: { int b = (int)(Rand() % 4); W16(0x10 + b * 4, (uint16_t)(Rand() & 0x1FF)); break; }
    case 1: { int b = (int)(Rand() % 4); W16(0x12 + b * 4, (uint16_t)(Rand() & 0x1FF)); break; }
    case 2: W16(0x52, (uint16_t)(((Rand() % 17) << 8) | (Rand() % 17))); break;
    case 3: W16(0x40, (uint16_t)Rand()); break;
    case 4: W32(0x28 + (Rand() % 2) * 0x10, (Rand() % 0x40000) - 0x10000); break;
    }
}

static void BetweenFrames(void)
{
    int n = (int)(Rand() % 4);
    for (int i = 0; i < n * 16; i++) VRAM[Rand() % 0x8000] = (uint8_t)Rand();            // tiles
    for (int i = 0; i < n * 8; i++) { unsigned a = (0x4000 + (Rand() % 0xC000)) & ~1u; VRAM[a] = (uint8_t)Rand(); }
    if (Chance(30)) for (int i = 0; i < 16; i++) PLTT[Rand() % 0x200] = (uint16_t)(Rand() & 0x7FFF);
    if (Chance(10)) RandomPalette(); // a fade
    if (Chance(50)) RandomOam();
    for (int b = 0; b < 4; b++) { W16(0x10 + b * 4, (uint16_t)(Rand() & 0x1FF)); W16(0x12 + b * 4, (uint16_t)(Rand() & 0x1FF)); }
    if (Chance(15)) RandomRegs();
}

static uint8_t sLineRegs[GBA_H][0x400];
static uint16_t sOut[2][GBA_W * GBA_H];

int main(int argc, char **argv)
{
    int runs = argc > 1 ? atoi(argv[1]) : 200;
    int frames = argc > 2 ? atoi(argv[2]) : 30;
    long bad = 0, total = 0;
    double tFast = 0, tRef = 0;

    for (int run = 0; run < runs; run++) {
        sSeed = 0x9E3779B9u ^ (uint32_t)(run * 7919 + 1);
        RandomTiles(0, 0x18000);
        RandomPalette();
        RandomOam();
        RandomRegs();
        for (int b = 0; b < 4; b++)
            RandomMap(0x4000 + 0x800 * (unsigned)b * 6, 0x2000);
        RandomMap(0x4000, 0xC000);

        for (int f = 0; f < frames; f++) {
            BetweenFrames();
            // The frame's registers line by line, made once and replayed into both renderers
            for (int line = 0; line < GBA_H; line++) {
                if (Chance(15))
                    LineChange();
                memcpy(sLineRegs[line], REG_BASE, 0x400);
            }

            for (int which = 0; which < 2; which++) {
                memcpy(REG_BASE, sLineRegs[0], 0x400);
                clock_t t0 = clock();
                if (which == 0) { BgCache_Prepare(REG_BASE, 0); Ppu_BeginFrame(); }
                else Ref_BeginFrame();
                for (int line = 0; line < GBA_H; line++) {
                    memcpy(REG_BASE, sLineRegs[line], 0x400);
                    if (which == 0) Ppu_RenderLine(line, &sOut[0][line * GBA_W]);
                    else Ref_RenderLine(line, &sOut[1][line * GBA_W]);
                }
                double dt = (double)(clock() - t0) / CLOCKS_PER_SEC;
                if (which == 0) tFast += dt; else tRef += dt;
            }
            memcpy(REG_BASE, sLineRegs[GBA_H - 1], 0x400); // (the next frame starts from where this one ended)

            total++;
            int diff = 0, fx = 0, fy = 0;
            for (int i = 0; i < GBA_W * GBA_H; i++)
                if ((sOut[0][i] & 0x7FFF) != (sOut[1][i] & 0x7FFF)) {
                    if (!diff) { fx = i % GBA_W; fy = i / GBA_W; }
                    diff++;
                }
            if (diff) {
                bad++;
                if (bad <= 10)
                    printf("run %d frame %d: %d pixels differ (first at %d,%d: %04x, reference %04x; mode %d)\n",
                           run, f, diff, fx, fy, sOut[0][fy * GBA_W + fx] & 0x7FFF, sOut[1][fy * GBA_W + fx] & 0x7FFF,
                           sLineRegs[fy][0] & 7);
            }
        }
    }
    printf("%ld of %ld frames differ. Time: renderer %.2f ms/frame, reference %.2f ms/frame (this computer)\n",
           bad, total, tFast * 1000 / total, tRef * 1000 / total);
    return bad != 0;
}
