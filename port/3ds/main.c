// Nintendo 3DS backend (libctru), stage 1.
//
// The game's picture goes on the top screen, copied by the CPU: 1:1 in the
// middle, or 1.5x to 360x240 (Y switches; 1.5x at first). A WIDE=1 build's
// 400x240 picture fills the top screen 1:1. The bottom screen shows a short
// status text with the frame rate. Sound goes through the DSP (needs
// sdmc:/3ds/dspfirm.cdc, which most custom firmware setups already have).
//
// Files are in sdmc:/3ds/sa3-3ds/: the save (sa3.sav, a 128 KiB GBA flash
// image) and, if debug.txt is there, a log (port.log) with the time frames
// take.
//
// Based on pmd-red-3ds's backend (port/3ds/main.c), cut down to what sa3
// needs to boot.

#include <3ds.h>
#include <citro3d.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "../core/port.h"

#define DATA_DIR "sdmc:/3ds/sa3-3ds"
#define SAVE_PATH DATA_DIR "/sa3.sav"
#define SAVE_TMP_PATH DATA_DIR "/sa3.sav.tmp"
#define LOG_PATH DATA_DIR "/port.log"
#define DEBUG_FLAG DATA_DIR "/debug.txt"
#define CPU_VIDEO_FLAG DATA_DIR "/cpu-video.txt" // the picture copied by the CPU, not the GPU

const int Backend_Version4_UpdateRepoIfMissing = 1;

// The main thread's stack (libctru's default is 32 KiB; PC builds have MBs).
u32 __stacksize__ = 1024 * 1024;

static PrintConsole sConsole;
static volatile int sLogWriterRun; // (the log's writer thread runs: Backend_Log)
static void LogWrite(void);
static int sAudioOn;
static int sScaled = 1; // 1.5x (Y switches to 1:1)
static int sSkipOn = 1; // frame skip (touching the bottom screen switches it)
static bool sGpu;       // the GPU puts the picture on the screen (else the CPU copies it)
static int sWorkerCore = -1;    // the worker's core (the game's sound); -1: none
static volatile u64 sWorkTicks; // the work's time on its core (meter window)

// --- Timing / performance meter ---------------------------------------------

#define FRAME_TICKS (SYSCLOCK_ARM11 / 60) // (the 3DS refreshes at ~59.83 Hz; close enough for now)
static u64 sDeadline;
static int sLate;

// Each part of a frame, in ticks, summed over the meter's window: the game's
// code; drawing and presenting (drawn frames); a frame not drawn's lines; the
// VBlank interrupt; the rest (save check and file, OS); waiting for the screen.
// And the parts of the window's slowest frame (sWorst).
enum { T_GAME, T_DRAW, T_PRESENT, T_UNDRAWN, T_SOUNDWAIT, T_VBLANK, T_OTHER, T_WAIT, T_COUNT };
static const char *const sPartNames[T_COUNT] = { "game", "draw", "present", "undrawn", "soundwait", "vblank", "other", "wait" };
static u64 sPartTicks[T_COUNT];
static u64 sFrameTicks[T_COUNT], sWorst[T_COUNT];
static u64 sMarkTick;
static u32 sMeterFrames, sMeterLate, sMeterDrawn;
static u64 sMeterStart;
static float sFps, sDrawnFps;
static u64 sProfTotal[PROF_COUNT];

static void Part(int part)
{
    u64 now = svcGetSystemTick();
    sPartTicks[part] += now - sMarkTick;
    sFrameTicks[part] += now - sMarkTick;
    sMarkTick = now;
}

void Backend_Mark(int what)
{
    switch (what) {
    case MARK_GAME: Part(T_GAME); break;
    case MARK_UNDRAWN: Part(T_UNDRAWN); break;
    case MARK_VBLANK: Part(T_VBLANK); break;
    default: Part(T_OTHER); break;
    }
}

// "level 1:23.45" (the HUD's time) or "" outside a level
static const char *LevelTimeText(void)
{
    static char buf[32];
    int t = Port_LevelTime();
    if (t < 0)
        return "";
    snprintf(buf, sizeof(buf), " level %d:%02d.%02d", t / 3600, (t / 60) % 60, (t % 60) * 100 / 60);
    return buf;
}

static float TicksToMs(u64 ticks, u32 frames);

// End of a frame's timing: the window's slowest frame (not counting the
// wait), and a line for a frame that took over SPIKE_MS (at most a few a
// window), with the level time, so it can be matched with what was on the
// screen.
#define SPIKE_MS 40
static int sSpikesLogged;
static FILE *sLog;
static void FrameTimed(void)
{
    u64 busy = 0, worst = 0;
    for (int i = 0; i < T_COUNT; i++)
        if (i != T_WAIT) {
            busy += sFrameTicks[i];
            worst += sWorst[i];
        }
    if (busy > worst)
        memcpy(sWorst, sFrameTicks, sizeof(sWorst));
    if (sLog != NULL && TicksToMs(busy, 1) > SPIKE_MS && sSpikesLogged < 5) {
        sSpikesLogged++;
        char parts[160];
        int n = 0;
        for (int i = 0; i < T_COUNT && n < (int)sizeof(parts); i++)
            if (i != T_WAIT)
                n += snprintf(parts + n, sizeof(parts) - n, " %s %.1f", sPartNames[i], (double)TicksToMs(sFrameTicks[i], 1));
        Backend_Log("spike: frame %lu%s, %.1f ms:%s", (unsigned long)Port_FrameCount(), LevelTimeText(),
                    (double)TicksToMs(busy, 1), parts);
    }
    memset(sFrameTicks, 0, sizeof(sFrameTicks));
}

static float TicksToMs(u64 ticks, u32 frames)
{
    return frames ? (float)ticks * 1000.0f / (float)SYSCLOCK_ARM11 / (float)frames : 0.0f;
}

static void PrintStatus(void);

static void MeterEndFrame(void)
{
    sMeterFrames++;
    sMeterLate += sLate;
    u64 now = svcGetSystemTick();
    if (sMeterStart == 0)
        sMeterStart = now;
    if (now - sMeterStart < SYSCLOCK_ARM11) // once a second
        return;
    sFps = (float)sMeterFrames * (float)SYSCLOCK_ARM11 / (float)(now - sMeterStart);
    sDrawnFps = (float)sMeterDrawn * (float)SYSCLOCK_ARM11 / (float)(now - sMeterStart);
    PrintStatus();
    if (sLog != NULL && Port_FrameCount() % 300 < 60) {
        // draw and present: per drawn frame; the rest per frame
        Backend_Log("frame %lu%s: %.1f fps (drawn %.1f), ms per frame: game %.2f sound %.2f (core %d, waited %.2f) vblank %.2f"
                    " undrawn %.2f other %.2f wait %.2f; per drawn frame: draw %.2f present %.2f; late %lu",
                    (unsigned long)Port_FrameCount(), LevelTimeText(), (double)sFps, (double)sDrawnFps,
                    (double)TicksToMs(sPartTicks[T_GAME], sMeterFrames), (double)TicksToMs(sWorkTicks, sMeterFrames),
                    sWorkerCore < 0 ? 0 : sWorkerCore, (double)TicksToMs(sPartTicks[T_SOUNDWAIT], sMeterFrames),
                    (double)TicksToMs(sPartTicks[T_VBLANK], sMeterFrames),
                    (double)TicksToMs(sPartTicks[T_UNDRAWN], sMeterFrames), (double)TicksToMs(sPartTicks[T_OTHER], sMeterFrames),
                    (double)TicksToMs(sPartTicks[T_WAIT], sMeterFrames),
                    (double)TicksToMs(sPartTicks[T_DRAW], sMeterDrawn), (double)TicksToMs(sPartTicks[T_PRESENT], sMeterDrawn),
                    (unsigned long)sMeterLate);
        char worst[160];
        int n = 0;
        for (int i = 0; i < T_COUNT && n < (int)sizeof(worst); i++)
            n += snprintf(worst + n, sizeof(worst) - n, " %s %.1f", sPartNames[i], (double)TicksToMs(sWorst[i], 1));
        Backend_Log("  slowest frame, ms:%s", worst);
        uint64_t prof[PROF_COUNT];
        Port_TakeProfile(prof);
        Backend_Log("  drawing, ms per drawn frame: backgrounds %.2f sprites %.2f compose %.2f prepare %.2f interrupts %.2f",
                    (double)TicksToMs(prof[PROF_BG], sMeterDrawn), (double)TicksToMs(prof[PROF_OBJ], sMeterDrawn),
                    (double)TicksToMs(prof[PROF_COMPOSE], sMeterDrawn), (double)TicksToMs(prof[PROF_PREPARE], sMeterDrawn),
                    (double)TicksToMs(prof[PROF_LINES_OTHER], sMeterDrawn));
        uint32_t st[STAT_COUNT];
        Port_TakeStats(st);
        float lines = st[STAT_LINES] ? (float)st[STAT_LINES] : 1.0f, frames = sMeterDrawn ? (float)sMeterDrawn : 1.0f;
        Backend_Log("  lines with: alpha %.0f%% brighten %.0f%% darken %.0f%% (no change %.0f%%) windows %.0f%% semi-sprites %.0f%%;"
                    " per frame: sprite rows %.0f, pixels %.0f, rotated rows %.0f",
                    (double)(100.0f * st[STAT_ALPHA] / lines), (double)(100.0f * st[STAT_BRIGHTEN] / lines),
                    (double)(100.0f * st[STAT_DARKEN] / lines), (double)(100.0f * st[STAT_NO_EFFECT] / lines),
                    (double)(100.0f * st[STAT_WINDOWS] / lines), (double)(100.0f * st[STAT_SEMI] / lines),
                    (double)(st[STAT_OBJ_ROWS] / frames), (double)(st[STAT_OBJ_PIXELS] / frames),
                    (double)(st[STAT_OBJ_AFFINE_ROWS] / frames));
        Backend_Log("  background lines per frame: cached %.0f decoded %.0f mosaic %.0f rotated %.0f",
                    (double)(st[STAT_BG_CACHED] / frames), (double)(st[STAT_BG_DIRECT] / frames),
                    (double)(st[STAT_BG_MOSAIC] / frames), (double)(st[STAT_BG_AFFINE] / frames));
    } else if (sLog != NULL) {
        uint64_t prof[PROF_COUNT];
        uint32_t st[STAT_COUNT];
        Port_TakeProfile(prof); // (only the logged second's)
        Port_TakeStats(st);
    }
    (void)sProfTotal;
    memset(sPartTicks, 0, sizeof(sPartTicks));
    memset(sWorst, 0, sizeof(sWorst));
    sWorkTicks = 0;
    sSpikesLogged = 0;
    sMeterFrames = sMeterLate = sMeterDrawn = 0;
    sMeterStart = now;
}

static void WaitForFrame(void)
{
    u64 now = svcGetSystemTick();
    if (sDeadline == 0)
        sDeadline = now + FRAME_TICKS;
    sLate = now > sDeadline;
    if (sLate && now - sDeadline > 2 * FRAME_TICKS)
        sDeadline = now; // too far behind to catch up: carry on from now
    if (sLate) {
        sDeadline += FRAME_TICKS;
        return;
    }
    gspWaitForVBlank();
    sDeadline = svcGetSystemTick() + FRAME_TICKS;
}

// --- Worker on another core (the game's sound) -------------------------------------------
//
// New 3DS models: core 2, free for apps. Old ones: core 1, the system's,
// which an app may use up to a share of its time (APT_SetAppCpuTimeLimit,
// 80%). If neither can be had, the core runs the work itself.

static Thread sWorker;
static LightEvent sWorkStart, sWorkDone;
static void (*volatile sWorkFn)(void *);
static void *volatile sWorkArg;
static void WorkerMain(void *unused)
{
    for (;;) {
        LightEvent_Wait(&sWorkStart);
        u64 t0 = svcGetSystemTick();
        sWorkFn(sWorkArg);
        sWorkTicks += svcGetSystemTick() - t0;
        LightEvent_Signal(&sWorkDone);
    }
}

static void WorkerInit(void)
{
    LightEvent_Init(&sWorkStart, RESET_ONESHOT);
    LightEvent_Init(&sWorkDone, RESET_ONESHOT);
    bool n3ds = false;
    APT_CheckNew3DS(&n3ds);
    if (n3ds) {
        sWorker = threadCreate(WorkerMain, NULL, 64 * 1024, 0x20, 2, true);
        if (sWorker)
            sWorkerCore = 2;
    }
    if (sWorkerCore < 0 && R_SUCCEEDED(APT_SetAppCpuTimeLimit(80))) {
        sWorker = threadCreate(WorkerMain, NULL, 64 * 1024, 0x20, 1, true);
        if (sWorker)
            sWorkerCore = 1;
    }
    if (sWorkerCore < 0)
        Backend_Log("worker: none (sound on the main core)");
    else
        Backend_Log("worker: core %d (sound)", sWorkerCore);
}

bool Backend_StartWorker(void (*fn)(void *), void *arg)
{
    if (sWorkerCore < 0)
        return false;
    sWorkFn = fn;
    sWorkArg = arg;
    LightEvent_Signal(&sWorkStart);
    return true;
}

void Backend_WaitWorker(void)
{
    LightEvent_Wait(&sWorkDone);
    Part(T_SOUNDWAIT);
}

// --- Frame skip -----------------------------------------------------------------------
//
// The game always runs every frame (so it plays and sounds at full speed);
// drawing a frame is left out when, at the time drawing has been taking (an
// average), it would end more than a frame past this frame's deadline. A
// frame drawn late is made up by the one(s) after it not being drawn: with
// drawing at 20-33 ms, every other frame is drawn (30 a second). Never more
// than MAX_SKIP frames in a row (15 drawn a second at worst).

#define MAX_SKIP 3
static u64 sDrawCost = FRAME_TICKS / 2; // ticks: drawing and presenting a frame (average)
static u64 sDrawStartTick;
static int sSkipRun;

bool Backend_ShouldDraw(void)
{
    u64 now = svcGetSystemTick();
    bool skip = sSkipOn && sDeadline != 0 && now + sDrawCost > sDeadline + FRAME_TICKS && sSkipRun < MAX_SKIP;
    sSkipRun = skip ? sSkipRun + 1 : 0;
    return !skip;
}

// --- Video (GPU) ------------------------------------------------------------------------
//
// From pmd-red-3ds (port/3ds/main.c, "GPU"): the CPU converts the picture to
// the 3DS's colours (RGB565) into a plain buffer, which the GPU's transfer
// engine copies into a texture (in the GPU's tiled layout); a rectangle with
// that texture is then drawn on the top screen, scaled by the GPU (bilinear).
// With sdmc:/3ds/sa3-3ds/cpu-video.txt (or if citro3d won't start), the CPU
// copies the picture to the screen instead, as before.

#include "present_shbin.h"

#define TEX_W 512 // the texture: wide and high enough for 400x240
#define TEX_H 256
#define TEX_TRANSFER_FLAGS                                                                  \
    (GX_TRANSFER_FLIP_VERT(1) | GX_TRANSFER_OUT_TILED(1) | GX_TRANSFER_RAW_COPY(0) |        \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB565) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) | \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))
#define SCREEN_TRANSFER_FLAGS                                                               \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |        \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) | \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

static C3D_RenderTarget *sTargetTop;
static C3D_Mtx sProjTop;
static DVLB_s *sShaderDvlb;
static shaderProgram_s sShader;
static int sProjLoc;
static C3D_Tex sTex;
static uint16_t *sStage; // the converted picture, TEX_W pixels a row (linear memory)
static void *sCopiedTo;  // the top screen's buffer the last copy went into (GpuWaitForSwap)

static void GpuInit(void)
{
    FILE *flag = fopen(CPU_VIDEO_FLAG, "r");
    if (flag != NULL) {
        fclose(flag);
        Backend_Log("video: CPU (cpu-video.txt)");
        return;
    }
    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE)) {
        Backend_Log("video: CPU (citro3d did not start)");
        return;
    }
    sTargetTop = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    sShaderDvlb = DVLB_ParseFile(sPresentShader, PRESENT_SHADER_SIZE);
    sStage = linearAlloc(TEX_W * TEX_H * 2);
    if (sTargetTop == NULL || sShaderDvlb == NULL || sStage == NULL || !C3D_TexInitVRAM(&sTex, TEX_W, TEX_H, GPU_RGB565)) {
        Backend_Log("video: CPU (GPU setup failed)");
        C3D_Fini();
        return;
    }
    memset(sStage, 0, TEX_W * TEX_H * 2);
    C3D_TexSetFilter(&sTex, GPU_LINEAR, GPU_LINEAR); // (bilinear when scaled; 1:1, each pixel as it is)
    C3D_TexSetWrap(&sTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    C3D_RenderTargetSetOutput(sTargetTop, GFX_TOP, GFX_LEFT, SCREEN_TRANSFER_FLAGS);

    shaderProgramInit(&sShader);
    shaderProgramSetVsh(&sShader, &sShaderDvlb->DVLE[0]);
    C3D_BindProgram(&sShader);
    sProjLoc = shaderInstanceGetUniformLocation(sShader.vertexShader, "projection");
    C3D_AttrInfo *attr = C3D_GetAttrInfo();
    AttrInfo_Init(attr);
    AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 3); // v0: position
    AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 2); // v1: texture coordinates
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_CullFace(GPU_CULL_NONE);
    C3D_TexEnv *env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_TEXTURE0, GPU_TEXTURE0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    // Screen pixels, y downwards (the 3DS's screens are turned sideways)
    Mtx_OrthoTilt(&sProjTop, 0.0f, 400.0f, 240.0f, 0.0f, 1.0f, -1.0f, true);
    sGpu = true;
    Backend_Log("video: GPU");
}

// GBA BGR555 to RGB565, two pixels at a time (as ToRgb565)
static void GpuConvert(const uint16_t *src, int w, int h)
{
    for (int y = 0; y < h; y++) {
        const uint32_t *s = (const uint32_t *)(src + y * w);
        uint32_t *d = (uint32_t *)(sStage + y * TEX_W);
        for (int x = 0; x < w / 2; x++) {
            uint32_t c = s[x];
            d[x] = ((c & 0x001F001F) << 11) | ((c & 0x03E003E0) << 1) | ((c >> 4) & 0x00200020) | ((c >> 10) & 0x001F001F);
        }
    }
}

// The screen's swap (asked for when the GPU finished copying the picture to
// the framebuffer) happens at its next VBlank; until then it still shows the
// other buffer, the one the next copy goes into: wait for that swap first,
// or the screen tears (pmd-red-3ds, GpuWaitForSwap).
static void GpuWaitForSwap(void)
{
    if (sCopiedTo == NULL)
        return;
    u64 giveUp = svcGetSystemTick() + SYSCLOCK_ARM11 / 30;
    while (gspIsPresentPending(GFX_TOP) && svcGetSystemTick() < giveUp)
        gspWaitForAnyEvent();
    sCopiedTo = NULL;
}

static void GpuPresent(const uint16_t *frame, int w, int h, int x, int y, int outW, int outH)
{
    GpuConvert(frame, w, h);
    if (R_FAILED(svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)sStage, TEX_W * h * 2)))
        GSPGPU_FlushDataCache(sStage, TEX_W * h * 2);

    C3D_FrameBegin(0); // (waits for the last frame's copies)
    // The copy into the texture: in the frame's queue, before its drawing
    C3D_SyncDisplayTransfer((u32 *)sStage, GX_BUFFER_DIM(TEX_W, TEX_H), (u32 *)sTex.data, GX_BUFFER_DIM(TEX_W, TEX_H),
                            TEX_TRANSFER_FLAGS);
    GpuWaitForSwap();

    // The picture's first row is at v = 0, its last at v = h / TEX_H
    float x0 = (float)x, y0 = (float)y, x1 = (float)(x + outW), y1 = (float)(y + outH);
    float u0 = 0.0f, u1 = (float)w / TEX_W, v0 = 0.0f, v1 = (float)h / TEX_H;
    if (outW != w || outH != h) {
        // Scaled (bilinear): the edges' pixels at the edges' texel centres
        u0 = 0.5f / TEX_W; u1 = (w - 0.5f) / TEX_W;
        v0 = 0.5f / TEX_H; v1 = (h - 0.5f) / TEX_H;
    }
    C3D_RenderTargetClear(sTargetTop, C3D_CLEAR_ALL, 0x000000FF, 0); // black borders
    C3D_FrameDrawOn(sTargetTop);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, sProjLoc, &sProjTop);
    C3D_TexBind(0, &sTex);
    C3D_ImmDrawBegin(GPU_TRIANGLE_STRIP);
    C3D_ImmSendAttrib(x0, y0, 0.5f, 0.0f); C3D_ImmSendAttrib(u0, v0, 0.0f, 0.0f);
    C3D_ImmSendAttrib(x1, y0, 0.5f, 0.0f); C3D_ImmSendAttrib(u1, v0, 0.0f, 0.0f);
    C3D_ImmSendAttrib(x0, y1, 0.5f, 0.0f); C3D_ImmSendAttrib(u0, v1, 0.0f, 0.0f);
    C3D_ImmSendAttrib(x1, y1, 0.5f, 0.0f); C3D_ImmSendAttrib(u1, v1, 0.0f, 0.0f);
    C3D_ImmDrawEnd();
    sCopiedTo = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
    C3D_FrameEnd(GX_CMDLIST_FLUSH); // (only the command list: the picture was flushed)
}

// --- Video (CPU copy) -----------------------------------------------------------
//
// The top screen's framebuffer is 240 pixels per column, columns left to
// right, each column from the bottom of the screen up: pixel (x, y) is at
// fb[x * 240 + 239 - y]. RGB565.

#define TOP_W 400
#define TOP_H 240

static inline uint16_t ToRgb565(uint16_t c)
{
    return (uint16_t)(((c & 0x1F) << 11) | ((c & 0x3E0) << 1) | ((c >> 4) & 0x20) | ((c >> 10) & 0x1F));
}

static uint16_t sScaleX[TOP_W], sScaleY[TOP_H]; // output pixel -> source pixel (1.5x)

static void BuildScaleMaps(void)
{
    for (int i = 0; i < TOP_W; i++)
        sScaleX[i] = (uint16_t)(i * 2 / 3);
    for (int i = 0; i < TOP_H; i++)
        sScaleY[i] = (uint16_t)(i * 2 / 3);
}

static void ClearTop(void)
{
    if (sGpu)
        return; // (the GPU clears around the picture every frame)
    for (int i = 0; i < 2; i++) {
        memset(gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL), 0, TOP_W * TOP_H * 2);
        gfxScreenSwapBuffers(GFX_TOP, false);
    }
}

void Backend_DrawStart(void)
{
    Part(T_GAME);
    sDrawStartTick = sMarkTick;
}

void Backend_Present(const uint16_t *frame, int width, int height)
{
    Part(T_DRAW);
    sMeterDrawn++;
    int scaled = sScaled && width * 3 / 2 <= TOP_W && height * 3 / 2 <= TOP_H;
    int outW = scaled ? width * 3 / 2 : width;
    int outH = scaled ? height * 3 / 2 : height;
    if (outW > TOP_W)
        outW = TOP_W;
    if (outH > TOP_H)
        outH = TOP_H;
    int ox = (TOP_W - outW) / 2;
    int oy = (TOP_H - outH) / 2;

    if (sGpu) {
        GpuPresent(frame, width, height, ox, oy, outW, outH);
        Part(T_PRESENT);
        sDrawCost = (sDrawCost * 7 + (sMarkTick - sDrawStartTick)) / 8;
        return;
    }

    uint16_t *fb = (uint16_t *)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
    for (int x = 0; x < outW; x++) {
        int sx = scaled ? sScaleX[x] : x;
        // column x + ox, from screen row oy + outH - 1 (bottom) up to oy
        uint16_t *col = fb + (x + ox) * TOP_H + (TOP_H - 1 - (oy + outH - 1));
        const uint16_t *src = frame + sx;
        if (scaled) {
            for (int y = outH - 1; y >= 0; y--)
                *col++ = ToRgb565(src[sScaleY[y] * width]);
        } else {
            for (int y = outH - 1; y >= 0; y--)
                *col++ = ToRgb565(src[y * width]);
        }
    }
    gfxScreenSwapBuffers(GFX_TOP, false); // (shown at the next VBlank)
    Part(T_PRESENT);
    sDrawCost = (sDrawCost * 7 + (sMarkTick - sDrawStartTick)) / 8;
}

// --- Bottom screen: status text ---------------------------------------------------

static void PrintStatus(void)
{
    consoleSelect(&sConsole);
    printf("\x1b[1;1H\x1b[1mSonic Advance 3\x1b[0m\n");
    printf("sa3-3ds (early test build)\n\n");
    printf("Picture: %dx%d %s   \n", Port_DisplayWidth(), Port_DisplayHeight(),
           (sScaled && Port_DisplayWidth() * 3 / 2 <= TOP_W) ? "1.5x" : "1:1 ");
    printf("Video  : %s\n", sGpu ? "GPU" : "CPU");
    if (sWorkerCore < 0)
        printf("Sound  : main core\n");
    else
        printf("Sound  : core %d\n", sWorkerCore);
    printf("Y      : 1:1 / 1.5x\n");
    printf("Touch  : frame skip %s  \n", sSkipOn ? "on " : "off");
    printf("HOME   : exit\n\n");
    printf("Speed  : %5.1f fps   \n", (double)sFps);
    printf("Drawn  : %5.1f fps   \n", (double)sDrawnFps);
    if (!sAudioOn)
        printf("\n\x1b[33mNo sound: sdmc:/3ds/dspfirm.cdc missing.\x1b[0m\n");
    if (sLog != NULL)
        printf("\nLogging to %s\n", LOG_PATH);
}

// --- Input -------------------------------------------------------------------------

uint16_t Backend_PollKeys(void)
{
    hidScanInput();
    u32 down = hidKeysDown();
    if (down & KEY_Y) {
        sScaled = !sScaled;
        ClearTop();
        PrintStatus();
    }
    if (down & KEY_TOUCH) {
        sSkipOn = !sSkipOn;
        PrintStatus();
    }
    u32 h = hidKeysHeld();
    uint16_t k = 0;
    if (h & KEY_A) k |= PORT_KEY_A;
    if (h & KEY_B) k |= PORT_KEY_B;
    if (h & KEY_SELECT) k |= PORT_KEY_SELECT;
    if (h & KEY_START) k |= PORT_KEY_START;
    if (h & KEY_RIGHT) k |= PORT_KEY_RIGHT; // (KEY_RIGHT etc. cover the D-pad and the Circle Pad)
    if (h & KEY_LEFT) k |= PORT_KEY_LEFT;
    if (h & KEY_UP) k |= PORT_KEY_UP;
    if (h & KEY_DOWN) k |= PORT_KEY_DOWN;
    if (h & (KEY_R | KEY_ZR)) k |= PORT_KEY_R;
    if (h & (KEY_L | KEY_ZL)) k |= PORT_KEY_L;
    if (h & KEY_X) k |= PORT_KEY_A; // (a second jump button)
    // The GBA can't press opposite directions together; the game may not expect it.
    if ((k & (PORT_KEY_LEFT | PORT_KEY_RIGHT)) == (PORT_KEY_LEFT | PORT_KEY_RIGHT))
        k &= ~(PORT_KEY_LEFT | PORT_KEY_RIGHT);
    if ((k & (PORT_KEY_UP | PORT_KEY_DOWN)) == (PORT_KEY_UP | PORT_KEY_DOWN))
        k &= ~(PORT_KEY_UP | PORT_KEY_DOWN);
    return k;
}

// --- Audio -------------------------------------------------------------------------

#define AUDIO_BUFS 8
#define AUDIO_MAX_SAMPLES 1024

static int sAudioRate;
static ndspWaveBuf sWave[AUDIO_BUFS];
static int16_t *sWaveData;

static void AudioInit(void)
{
    if (R_FAILED(ndspInit()))
        return; // no dspfirm.cdc: run silently
    sWaveData = linearAlloc(AUDIO_BUFS * AUDIO_MAX_SAMPLES * 2 * sizeof(int16_t));
    if (!sWaveData) {
        ndspExit();
        return;
    }
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(0);
    ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
    ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
    float mix[12] = { 1.0f, 1.0f };
    ndspChnSetMix(0, mix);
    memset(sWave, 0, sizeof(sWave));
    for (int i = 0; i < AUDIO_BUFS; i++) {
        sWave[i].data_pcm16 = sWaveData + i * AUDIO_MAX_SAMPLES * 2;
        sWave[i].status = NDSP_WBUF_DONE;
    }
    sAudioOn = 1;
}

void Backend_QueueAudio(const int16_t *stereo, int count, int rate)
{
    if (!sAudioOn || count <= 0)
        return;
    if (count > AUDIO_MAX_SAMPLES)
        count = AUDIO_MAX_SAMPLES;
    if (rate != sAudioRate) {
        sAudioRate = rate;
        ndspChnSetRate(0, (float)rate);
    }
    for (int i = 0; i < AUDIO_BUFS; i++) {
        ndspWaveBuf *wb = &sWave[i];
        if (wb->status != NDSP_WBUF_DONE && wb->status != NDSP_WBUF_FREE)
            continue;
        memcpy(wb->data_pcm16, stereo, (size_t)count * 4);
        wb->nsamples = (u32)count;
        DSP_FlushDataCache(wb->data_pcm16, (u32)count * 4);
        ndspChnWaveBufAdd(0, wb);
        return;
    }
    // Every buffer is still queued: the game is running ahead; drop this chunk.
}

// --- Saves -------------------------------------------------------------------------

static const uint8_t *sSaveData;
static uint32_t sSaveSize;
static int sSaveDirty;

bool Backend_LoadSave(uint8_t *data, uint32_t size)
{
    FILE *f = fopen(SAVE_PATH, "rb");
    if (!f)
        return false;
    size_t n = fread(data, 1, size, f);
    fclose(f);
    Backend_Log("save: loaded %u bytes", (unsigned)n);
    return n == size;
}

void Backend_StoreSave(const uint8_t *data, uint32_t size)
{
    // Written at the end of the frame, once.
    sSaveData = data;
    sSaveSize = size;
    sSaveDirty = 1;
}

static void FlushSave(void)
{
    if (!sSaveDirty)
        return;
    sSaveDirty = 0;
    // Write a new file and swap it in, so a power cut can't leave half a save.
    FILE *f = fopen(SAVE_TMP_PATH, "wb");
    if (!f) {
        Backend_Log("save: can't write %s", SAVE_TMP_PATH);
        return;
    }
    size_t n = fwrite(sSaveData, 1, sSaveSize, f);
    fclose(f);
    if (n != sSaveSize) {
        Backend_Log("save: short write");
        return;
    }
    remove(SAVE_PATH);
    rename(SAVE_TMP_PATH, SAVE_PATH);
    Backend_Log("save: written");
}

// --- Frame end, log, errors ---------------------------------------------------------

static void Shutdown(void)
{
    FlushSave();
    if (sAudioOn) {
        ndspExit();
        linearFree(sWaveData);
    }
    if (sGpu)
        C3D_Fini();
    sLogWriterRun = 0;
    LogWrite();
    if (sLog)
        fclose(sLog);
    gfxExit();
}

void Backend_EndFrame(void)
{
    FlushSave();
    gfxFlushBuffers(); // (the bottom screen's text)
    Part(T_OTHER);
    WaitForFrame();
    Part(T_WAIT);
    FrameTimed();
    MeterEndFrame();
    if (!aptMainLoop()) {
        Shutdown();
        exit(0);
    }
}

static LightLock sLogLock;

// The log's lines are kept in memory and written to the SD card by a thread
// of the lowest priority (it runs while the game waits for the screen), so
// the game doesn't stop for the card (50-60 ms a write). If the buffer
// fills before the thread gets to it, the line is written right away.
#define LOG_BUF 16384
static char sLogBuf[LOG_BUF];
static int sLogLen;

static void LogWrite(void)
{
    static char out[LOG_BUF];
    LightLock_Lock(&sLogLock);
    int n = sLogLen;
    memcpy(out, sLogBuf, n);
    sLogLen = 0;
    LightLock_Unlock(&sLogLock);
    if (n > 0 && sLog != NULL) {
        fwrite(out, 1, n, sLog);
        fflush(sLog);
    }
}

static void LogWriter(void *arg)
{
    while (sLogWriterRun) {
        svcSleepThread(500 * 1000 * 1000LL);
        LogWrite();
    }
}

void Backend_Log(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    svcOutputDebugString(buf, (s32)strlen(buf));
    if (sLog != NULL) {
        int len = (int)strlen(buf);
        LightLock_Lock(&sLogLock);
        bool full = sLogLen + len + 1 > LOG_BUF;
        if (!full) {
            memcpy(sLogBuf + sLogLen, buf, len);
            sLogBuf[sLogLen + len] = '\n';
            sLogLen += len + 1;
        }
        LightLock_Unlock(&sLogLock);
        if (full) {
            LogWrite();
            LightLock_Lock(&sLogLock);
            if (sLogLen + len + 1 <= LOG_BUF) {
                memcpy(sLogBuf + sLogLen, buf, len);
                sLogBuf[sLogLen + len] = '\n';
                sLogLen += len + 1;
            }
            LightLock_Unlock(&sLogLock);
        }
        if (!sLogWriterRun)
            LogWrite(); // (no writer thread: at once)
    }
}

// --- Watchdog -------------------------------------------------------------------------
//
// A higher-priority thread that notices when frames stop coming. Then it
// shows on the bottom screen (and logs) what the game was doing, sampled a
// few times so a loop between several tasks shows up, and lets START+SELECT
// quit, since HOME can't work while the main thread is stuck.

static volatile int sWatchdogRun = 1;
static volatile int sSuspended; // in the Home Menu or asleep: no frames, and that's fine
static aptHookCookie sAptHook;

static void OnApt(APT_HookType hook, void *param)
{
    if (hook == APTHOOK_ONSUSPEND || hook == APTHOOK_ONSLEEP)
        sSuspended = 1;
    else if (hook == APTHOOK_ONRESTORE || hook == APTHOOK_ONWAKEUP)
        sSuspended = 0;
}

static void Watchdog(void *arg)
{
    u32 lastFrame = Port_FrameCount();
    u64 lastChange = svcGetSystemTick();
    int reported = 0;
    while (sWatchdogRun) {
        svcSleepThread(250 * 1000 * 1000LL); // 0.25 s
        u32 frame = Port_FrameCount();
        u64 now = svcGetSystemTick();
        if (frame != lastFrame || sSuspended) {
            lastFrame = frame;
            lastChange = now;
            reported = 0;
            continue;
        }
        if (now - lastChange < 3ULL * SYSCLOCK_ARM11)
            continue;

        // Stuck for 3 s or more. Report once, then again every 10 s.
        if (!reported || now - lastChange > (u64)(3 + 10 * reported) * SYSCLOCK_ARM11) {
            char line[160];
            consoleSelect(&sConsole);
            printf("\x1b[17;1H\x1b[31mNo frame for %lu s:\x1b[0m        \n",
                   (unsigned long)((now - lastChange) / SYSCLOCK_ARM11));
            Backend_Log("watchdog: no frame for %lu s", (unsigned long)((now - lastChange) / SYSCLOCK_ARM11));
            for (int i = 0; i < 4; i++) {
                Port_Describe(line, sizeof(line));
                printf("%s\n", line);
                Backend_Log("watchdog: %s", line);
                svcSleepThread(50 * 1000 * 1000LL);
            }
            printf("\nSTART+SELECT: quit\n");
            reported++;
        }
        hidScanInput();
        if ((hidKeysHeld() & (KEY_START | KEY_SELECT)) == (KEY_START | KEY_SELECT)) {
            Backend_Log("watchdog: quit");
            LogWrite();
            if (sLog)
                fclose(sLog);
            svcExitProcess();
        }
    }
}

void Backend_Fatal(const char *msg)
{
    Backend_Log("FATAL: %s", msg);
    consoleSelect(&sConsole);
    consoleClear();
    printf("\x1b[31msa3-3ds stopped:\x1b[0m\n%s\n\nPress START to exit.\n", msg);
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START)
            break;
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
    Shutdown();
    exit(1);
}

int main(void)
{
    gfxInitDefault();
    osSetSpeedupEnable(true); // 804 MHz and the L2 cache on New 3DS models
    gfxSetScreenFormat(GFX_TOP, GSP_RGB565_OES);
    gfxSetDoubleBuffering(GFX_TOP, true);
    consoleInit(GFX_BOTTOM, &sConsole);

    mkdir("sdmc:/3ds", 0777);
    mkdir(DATA_DIR, 0777);
    FILE *flag = fopen(DEBUG_FLAG, "r");
    if (flag) {
        fclose(flag);
        sLog = fopen(LOG_PATH, "w");
    }

    LightLock_Init(&sLogLock);
    aptHook(&sAptHook, OnApt, NULL);
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    if (!threadCreate(Watchdog, NULL, 16 * 1024, prio - 1, -2, true))
        Backend_Log("watchdog: could not start");

    if (sLog) {
        Port_SetProfiling(svcGetSystemTick); // (the drawing profile in port.log)
        sLogWriterRun = 1;
        if (!threadCreate(LogWriter, NULL, 16 * 1024, 0x3F, -2, true))
            sLogWriterRun = 0;
    }
    GpuInit();
    WorkerInit();
    BuildScaleMaps();
    ClearTop();
    AudioInit();
    PrintStatus();
    sMarkTick = svcGetSystemTick();

    Port_Main();
}
