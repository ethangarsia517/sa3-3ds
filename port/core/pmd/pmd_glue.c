// What pmd-red-3ds's renderer and background cache need from the rest of its
// core, for sa3-3ds: the picture is always the GBA's 240x160 (no wide view),
// and the drawing list is never on, so the list's functions are never called
// (they stop the game with a message if they ever are).

#include <stdint.h>
#include "port.h"
#include "bgcache.h"
#include "gpulist.h"

void Backend_Fatal(const char *msg) __attribute__((noreturn));

int gPortScreenW = GBA_W, gPortScreenH = GBA_H;
uint64_t (*gPpuTicks)(void);
uint64_t gPpuProf[3];
int16_t gPortOamY[128];
struct PortWinX gPortWin1X[SCREEN_H_MAX];

// Drawing list: not used (yet)
GpuList gGpuList;
uint16_t gGpuObj[GPU_OBJ_H][GPU_OBJ_W];
uint32_t gGpuObjTileChanged[1024];

static void __attribute__((noreturn)) NoList(void)
{
    Backend_Fatal("pmd renderer: drawing list used, but sa3-3ds has none");
}

void GpuList_BgLine(int line, int bg, uint16_t key, int flags, int y, int x0, int wPx, int xa, int xb) { NoList(); }
int GpuList_RepeatLine(int line) { NoList(); }
void GpuList_StripLine(int line, int bg, uint16_t key, int flags, const uint16_t *px, int xa, int xb) { NoList(); }
int GpuList_ObjOverlap(const GpuQuad *a, const GpuQuad *b) { NoList(); }
void GpuList_ObjTiles(const uint8_t *objVram) { NoList(); }
uint16_t *GpuList_Obj(int x, int y, int w, int h, int flags, uint16_t key, uint32_t look, uint32_t changed,
                      uint16_t bank, int *made) { NoList(); }
