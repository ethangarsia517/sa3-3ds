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

const int Backend_Version1_UpdateRepoIfMissing = 1;

// The main thread's stack (libctru's default is 32 KiB; PC builds have MBs).
u32 __stacksize__ = 1024 * 1024;

static PrintConsole sConsole;
static FILE *sLog;
static int sAudioOn;
static int sScaled = 1; // 1.5x (Y switches to 1:1)

// --- Timing / performance meter ---------------------------------------------

#define FRAME_TICKS (SYSCLOCK_ARM11 / 60) // (the 3DS refreshes at ~59.83 Hz; close enough for now)
static u64 sDeadline;
static int sLate;

// Each part of a frame, in ticks, summed over the meter's window.
enum { T_GAME, T_DRAW, T_PRESENT, T_WAIT, T_COUNT };
static u64 sPartTicks[T_COUNT];
static u64 sMarkTick;
static u32 sMeterFrames, sMeterLate;
static u64 sMeterStart;
static float sFps;

static void Part(int part)
{
    u64 now = svcGetSystemTick();
    sPartTicks[part] += now - sMarkTick;
    sMarkTick = now;
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
    PrintStatus();
    if (sLog != NULL && Port_FrameCount() % 300 < 60)
        Backend_Log("frame %lu: %.1f fps, ms/frame game %.2f draw %.2f present %.2f wait %.2f, late %lu",
                    (unsigned long)Port_FrameCount(), (double)sFps, (double)TicksToMs(sPartTicks[T_GAME], sMeterFrames),
                    (double)TicksToMs(sPartTicks[T_DRAW], sMeterFrames), (double)TicksToMs(sPartTicks[T_PRESENT], sMeterFrames),
                    (double)TicksToMs(sPartTicks[T_WAIT], sMeterFrames), (unsigned long)sMeterLate);
    memset(sPartTicks, 0, sizeof(sPartTicks));
    sMeterFrames = sMeterLate = 0;
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
    for (int i = 0; i < 2; i++) {
        memset(gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL), 0, TOP_W * TOP_H * 2);
        gfxScreenSwapBuffers(GFX_TOP, false);
    }
}

void Backend_DrawStart(void)
{
    Part(T_GAME);
}

void Backend_Present(const uint16_t *frame, int width, int height)
{
    Part(T_DRAW);
    uint16_t *fb = (uint16_t *)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
    int scaled = sScaled && width * 3 / 2 <= TOP_W && height * 3 / 2 <= TOP_H;
    int outW = scaled ? width * 3 / 2 : width;
    int outH = scaled ? height * 3 / 2 : height;
    if (outW > TOP_W)
        outW = TOP_W;
    if (outH > TOP_H)
        outH = TOP_H;
    int ox = (TOP_W - outW) / 2;
    int oy = (TOP_H - outH) / 2;

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
    Part(T_PRESENT);
}

// --- Bottom screen: status text ---------------------------------------------------

static void PrintStatus(void)
{
    consoleSelect(&sConsole);
    printf("\x1b[1;1H\x1b[1mSonic Advance 3\x1b[0m\n");
    printf("sa3-3ds (early test build)\n\n");
    printf("Picture: %dx%d %s   \n", Port_DisplayWidth(), Port_DisplayHeight(),
           (sScaled && Port_DisplayWidth() * 3 / 2 <= TOP_W) ? "1.5x" : "1:1 ");
    printf("Y      : 1:1 / 1.5x\n");
    printf("HOME   : exit\n\n");
    printf("Speed  : %5.1f fps   \n", (double)sFps);
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
    if (sLog)
        fclose(sLog);
    gfxExit();
}

void Backend_EndFrame(void)
{
    FlushSave();
    gfxFlushBuffers();
    gfxScreenSwapBuffers(GFX_TOP, false);
    WaitForFrame();
    Part(T_WAIT);
    MeterEndFrame();
    if (!aptMainLoop()) {
        Shutdown();
        exit(0);
    }
}

static LightLock sLogLock;

void Backend_Log(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    svcOutputDebugString(buf, (s32)strlen(buf));
    if (sLog != NULL) {
        LightLock_Lock(&sLogLock);
        fputs(buf, sLog);
        fputc('\n', sLog);
        fflush(sLog);
        LightLock_Unlock(&sLogLock);
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
            printf("\x1b[12;1H\x1b[31mNo frame for %lu s:\x1b[0m        \n",
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

    BuildScaleMaps();
    ClearTop();
    AudioInit();
    PrintStatus();
    sMarkTick = svcGetSystemTick();

    Port_Main();
}
