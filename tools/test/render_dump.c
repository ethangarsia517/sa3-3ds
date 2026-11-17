// Renders a frame dump from the 3DS (dump-NNN.bin: port/core/port.h, "Frame
// dump") on the computer you build on, to find out where a graphics bug
// comes from:
//   port.ppm      the frame by the port's renderer (port/core/pmd)
//   ref.ppm       the same by pmd-red-3ds's reference renderer (simple and
//                 slow; tools/test/ppu_ref.c)
//   diff.ppm      where they differ, in red (none: the renderer is faithful
//                 to the state, so a wrong picture means the game's state is)
//   bg0..bg3.ppm  one background alone, obj.ppm the sprites alone (port's
//                 renderer, colour effects off; the windows still apply)
//   report.txt    the display registers (line 0 and wherever they change),
//                 and every sprite that's on
//
// build: make render_dump
// usage: build/render_dump dump-001.bin [output folder, default: dump-001/]

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

#define REGS 0x60
#define W 240
#define H 160

static struct {
    char magic[4];
    uint32_t version, width, height;
    uint8_t vram[0x18000];
    uint8_t pltt[0x400];
    uint8_t oam[0x400];
    uint8_t regs[H][REGS];
    uint8_t plttEnd[0x400];
} sDump;

static uint8_t sLineRegs[H][REGS];
static uint16_t sOut[W * H];

static uint16_t R16(const uint8_t *r, int off) { return (uint16_t)(r[off] | (r[off + 1] << 8)); }
static void W16(uint8_t *r, int off, uint16_t v) { r[off] = (uint8_t)v; r[off + 1] = (uint8_t)(v >> 8); }

static void Render(int reference)
{
    memset(REG_BASE, 0, sizeof(REG_BASE));
    memcpy(REG_BASE, sLineRegs[0], REGS);
    if (reference)
        Ref_BeginFrame();
    else {
        BgCache_Prepare(REG_BASE, 0);
        Ppu_BeginFrame();
    }
    for (int y = 0; y < H; y++) {
        memcpy(REG_BASE, sLineRegs[y], REGS);
        if (reference)
            Ref_RenderLine(y, &sOut[y * W]);
        else
            Ppu_RenderLine(y, &sOut[y * W]);
    }
}

static void WritePpm(const char *dir, const char *name, const uint16_t *px, const uint8_t *mark)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        exit(1);
    }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) {
        uint8_t rgb[3];
        if (mark && mark[i]) {
            rgb[0] = 255; rgb[1] = 0; rgb[2] = 0;
        } else {
            uint16_t c = px[i];
            rgb[0] = (uint8_t)((c & 31) * 255 / 31);
            rgb[1] = (uint8_t)(((c >> 5) & 31) * 255 / 31);
            rgb[2] = (uint8_t)(((c >> 10) & 31) * 255 / 31);
            if (mark) { rgb[0] /= 3; rgb[1] /= 3; rgb[2] /= 3; } // (dimmed under the marks)
        }
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

// Each line's registers with only some layers on and the colour effects off
static void OnlyLayers(unsigned layerBits)
{
    for (int y = 0; y < H; y++) {
        memcpy(sLineRegs[y], sDump.regs[y], REGS);
        uint16_t dispcnt = R16(sLineRegs[y], 0x00);
        dispcnt = (uint16_t)((dispcnt & ~0x1F00) | (dispcnt & (layerBits << 8)));
        W16(sLineRegs[y], 0x00, dispcnt);
        W16(sLineRegs[y], 0x50, 0); // BLDCNT
    }
}

static void Report(const char *dir)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/report.txt", dir);
    FILE *f = fopen(path, "w");
    static const struct { int off; const char *name; } regs[] = {
        { 0x00, "DISPCNT" }, { 0x08, "BG0CNT" }, { 0x0A, "BG1CNT" }, { 0x0C, "BG2CNT" }, { 0x0E, "BG3CNT" },
        { 0x10, "BG0HOFS" }, { 0x12, "BG0VOFS" }, { 0x14, "BG1HOFS" }, { 0x16, "BG1VOFS" },
        { 0x18, "BG2HOFS" }, { 0x1A, "BG2VOFS" }, { 0x1C, "BG3HOFS" }, { 0x1E, "BG3VOFS" },
        { 0x20, "BG2PA" }, { 0x22, "BG2PB" }, { 0x24, "BG2PC" }, { 0x26, "BG2PD" }, { 0x28, "BG2X_L" }, { 0x2A, "BG2X_H" },
        { 0x2C, "BG2Y_L" }, { 0x2E, "BG2Y_H" }, { 0x30, "BG3PA" }, { 0x38, "BG3X_L" }, { 0x3C, "BG3Y_L" },
        { 0x40, "WIN0H" }, { 0x42, "WIN1H" }, { 0x44, "WIN0V" }, { 0x46, "WIN1V" }, { 0x48, "WININ" }, { 0x4A, "WINOUT" },
        { 0x4C, "MOSAIC" }, { 0x50, "BLDCNT" }, { 0x52, "BLDALPHA" }, { 0x54, "BLDY" },
    };
    fprintf(f, "Display registers: line 0, then each line where one changes.\n\n");
    for (int y = 0; y < H; y++) {
        int printed = 0;
        for (size_t k = 0; k < sizeof(regs) / sizeof(regs[0]); k++) {
            uint16_t v = R16(sDump.regs[y], regs[k].off);
            if (y > 0 && v == R16(sDump.regs[y - 1], regs[k].off))
                continue;
            if (!printed)
                fprintf(f, "line %3d:", y);
            fprintf(f, " %s=%04x", regs[k].name, v);
            printed = 1;
        }
        if (printed)
            fprintf(f, "\n");
    }
    uint16_t d = R16(sDump.regs[0], 0);
    fprintf(f, "\nLine 0: mode %d, BG0-3 %s%s%s%s, OBJ %s, WIN0 %s, WIN1 %s, OBJWIN %s, sprite tiles %s\n", d & 7,
            d & 0x100 ? "0" : "-", d & 0x200 ? "1" : "-", d & 0x400 ? "2" : "-", d & 0x800 ? "3" : "-",
            d & 0x1000 ? "on" : "off", d & 0x2000 ? "on" : "off", d & 0x4000 ? "on" : "off", d & 0x8000 ? "on" : "off",
            d & 0x40 ? "1-D" : "2-D");
    uint16_t b = R16(sDump.regs[0], 0x50);
    static const char *const eff[] = { "none", "alpha", "brighten", "darken" };
    fprintf(f, "Line 0: BLDCNT effect %s, first target %02x, second target %02x; BLDALPHA eva %d evb %d; BLDY %d\n",
            eff[(b >> 6) & 3], b & 0x3F, (b >> 8) & 0x3F, R16(sDump.regs[0], 0x52) & 31,
            (R16(sDump.regs[0], 0x52) >> 8) & 31, R16(sDump.regs[0], 0x54) & 31);
    fprintf(f, "Palette changed during the frame: %s\n",
            memcmp(sDump.pltt, sDump.plttEnd, 0x400) ? "yes (rendered with the frame's first)" : "no");

    static const char *const shapes[3][4] = { { "8x8", "16x16", "32x32", "64x64" },
                                              { "16x8", "32x8", "32x16", "64x32" },
                                              { "8x16", "8x32", "16x32", "32x64" } };
    static const char *const modes[] = { "normal", "semi-transparent", "OBJ-window", "(mode 3)" };
    fprintf(f, "\nSprites that are on (index: x,y size mode prio tile pal flags):\n");
    for (int i = 0; i < 128; i++) {
        uint16_t a0 = R16(sDump.oam, i * 8), a1 = R16(sDump.oam, i * 8 + 2), a2 = R16(sDump.oam, i * 8 + 4);
        int affine = a0 & 0x100;
        if (!affine && (a0 & 0x200))
            continue; // off
        int shape = (a0 >> 14) & 3, size = (a1 >> 14) & 3;
        int y = a0 & 0xFF, x = a1 & 0x1FF;
        if (x >= 256) x -= 512;
        fprintf(f, "%3d: %4d,%3d %-5s %-16s prio %d tile %3d pal %2d%s%s%s%s%s\n", i, x, y,
                shape < 3 ? shapes[shape][size] : "?", modes[(a0 >> 10) & 3], (a2 >> 10) & 3, a2 & 0x3FF, (a2 >> 12) & 15,
                a0 & 0x2000 ? " 256-colour" : "", affine ? (a0 & 0x200 ? " affine double-size" : " affine") : "",
                a0 & 0x1000 ? " mosaic" : "", !affine && (a1 & 0x1000) ? " hflip" : "",
                !affine && (a1 & 0x2000) ? " vflip" : "");
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s dump-NNN.bin [output folder]\n", argv[0]);
        return 1;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f || fread(&sDump, 1, sizeof(sDump), f) != sizeof(sDump) || memcmp(sDump.magic, "SA3D", 4) != 0 ||
        sDump.version != 1 || sDump.width != W || sDump.height != H) {
        fprintf(stderr, "%s: not a version-1 240x160 sa3-3ds frame dump\n", argv[1]);
        return 1;
    }
    fclose(f);
    char dir[512];
    if (argc > 2)
        snprintf(dir, sizeof(dir), "%s", argv[2]);
    else {
        snprintf(dir, sizeof(dir), "%s", argv[1]);
        char *dot = strrchr(dir, '.');
        if (dot) *dot = 0;
    }
    mkdir(dir, 0777);

    memcpy(VRAM, sDump.vram, sizeof(VRAM));
    memcpy(PLTT, sDump.pltt, sizeof(PLTT));
    memcpy(OAM, sDump.oam, sizeof(OAM));

    static uint16_t port[W * H], ref[W * H];
    memcpy(sLineRegs, sDump.regs, sizeof(sLineRegs));
    Render(0);
    memcpy(port, sOut, sizeof(port));
    Render(1);
    memcpy(ref, sOut, sizeof(ref));
    static uint8_t mark[W * H];
    int diff = 0;
    for (int i = 0; i < W * H; i++) {
        mark[i] = (port[i] & 0x7FFF) != (ref[i] & 0x7FFF);
        diff += mark[i];
    }
    WritePpm(dir, "port.ppm", port, NULL);
    WritePpm(dir, "ref.ppm", ref, NULL);
    WritePpm(dir, "diff.ppm", port, mark);

    static const char *const names[] = { "bg0.ppm", "bg1.ppm", "bg2.ppm", "bg3.ppm", "obj.ppm" };
    for (int l = 0; l < 5; l++) {
        OnlyLayers(1u << l);
        Render(0);
        WritePpm(dir, names[l], sOut, NULL);
    }
    Report(dir);
    printf("%s: %d pixels differ between the port's renderer and the reference. Pictures and report.txt in %s/\n",
           argv[1], diff, dir);
    return 0;
}
