// A headless backend for sa3-3ds's core on the computer you build on: runs
// the game (built for this computer: tools/host/build.sh) as fast as it can,
// with no window and no sound, following a script of button presses, and
// saves pictures of frames (the 3DS renderer's own output) and frame dumps.
// For finding graphics bugs without a 3DS.
//
// usage: sa3host script.txt [output folder, default: host-out]
// script lines (frames count from 1; '#' starts a comment):
//   <frame> press <keys> [frames, default 6]   keys: A B START SELECT UP DOWN
//                                              LEFT RIGHT L R, joined with +
//   <frame> shot <name>        that frame's picture -> <folder>/<name>.ppm
//   <frame> dump <name>        a frame dump of the next frame -> <name>.bin
//   <frame> quit

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "../../port/core/port.h"

const int Backend_Version5_UpdateRepoIfMissing = 1;

enum { CMD_PRESS, CMD_SHOT, CMD_DUMP, CMD_QUIT };
struct Cmd {
    uint32_t frame;
    int type;
    uint16_t keys;
    uint32_t frames;
    char name[64];
};
static struct Cmd sCmds[4096];
static int sCmdCount;
static const char *sOutDir = "host-out";
static char sDumpName[64];

// Keys held until frame sHeldUntil[i] (exclusive), per key bit
static uint32_t sHeldUntil[10];

static uint16_t ParseKeys(const char *s)
{
    static const char *const names[10] = { "A", "B", "SELECT", "START", "RIGHT", "LEFT", "UP", "DOWN", "R", "L" };
    uint16_t k = 0;
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", s);
    for (char *t = strtok(buf, "+"); t; t = strtok(NULL, "+")) {
        int found = 0;
        for (int i = 0; i < 10; i++)
            if (strcmp(t, names[i]) == 0) {
                k |= (uint16_t)(1 << i);
                found = 1;
            }
        if (!found) {
            fprintf(stderr, "script: unknown key %s\n", t);
            exit(1);
        }
    }
    return k;
}

static void LoadScript(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        perror(path);
        exit(1);
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *hash = strchr(line, '#');
        if (hash)
            *hash = 0;
        char what[32] = "", arg[128] = "";
        unsigned frame, n = 6;
        int got = sscanf(line, "%u %31s %127s %u", &frame, what, arg, &n);
        if (got < 2)
            continue;
        struct Cmd *c = &sCmds[sCmdCount++];
        memset(c, 0, sizeof(*c));
        c->frame = frame;
        if (strcmp(what, "press") == 0) {
            c->type = CMD_PRESS;
            c->keys = ParseKeys(arg);
            c->frames = got >= 4 ? n : 6;
        } else if (strcmp(what, "shot") == 0) {
            c->type = CMD_SHOT;
            snprintf(c->name, sizeof(c->name), "%s", arg);
        } else if (strcmp(what, "dump") == 0) {
            c->type = CMD_DUMP;
            snprintf(c->name, sizeof(c->name), "%s", arg);
        } else if (strcmp(what, "quit") == 0) {
            c->type = CMD_QUIT;
        } else {
            fprintf(stderr, "script: unknown command %s\n", what);
            exit(1);
        }
    }
    fclose(f);
}

// --- The frame -------------------------------------------------------------------

static uint16_t sFrame[400 * 240];
static int sW, sH;

static void SavePpm(const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.ppm", sOutDir, name);
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", sW, sH);
    for (int i = 0; i < sW * sH; i++) {
        uint16_t c = sFrame[i];
        uint8_t rgb[3] = { (uint8_t)((c & 31) * 255 / 31), (uint8_t)(((c >> 5) & 31) * 255 / 31),
                           (uint8_t)(((c >> 10) & 31) * 255 / 31) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

bool Backend_ShouldDraw(void) { return true; }
void Backend_DrawStart(void) { }
void Backend_Mark(int what) { (void)what; }
bool Backend_StartWorker(void (*fn)(void *), void *arg) { (void)fn; (void)arg; return false; }
void Backend_WaitWorker(void) { }

void Backend_Present(const uint16_t *frame, int width, int height)
{
    sW = width;
    sH = height;
    memcpy(sFrame, frame, (size_t)width * height * 2);
}

uint16_t Backend_PollKeys(void)
{
    uint32_t f = Port_FrameCount() + 1; // (the frame about to be made)
    uint16_t k = 0;
    for (int i = 0; i < 10; i++)
        if (f < sHeldUntil[i])
            k |= (uint16_t)(1 << i);
    return k;
}

void Backend_EndFrame(void)
{
    uint32_t f = Port_FrameCount(); // (just made)
    for (int i = 0; i < sCmdCount; i++) {
        struct Cmd *c = &sCmds[i];
        if (c->frame != f)
            continue;
        switch (c->type) {
        case CMD_PRESS:
            for (int b = 0; b < 10; b++)
                if (c->keys & (1 << b))
                    sHeldUntil[b] = f + 1 + c->frames;
            break;
        case CMD_SHOT:
            SavePpm(c->name);
            printf("frame %u: shot %s\n", f, c->name);
            break;
        case CMD_DUMP:
            snprintf(sDumpName, sizeof(sDumpName), "%s", c->name);
            Port_RequestDump();
            break;
        case CMD_QUIT:
            printf("frame %u: quit\n", f);
            fflush(stdout);
            exit(0);
        }
    }
}

void Backend_SaveDump(const void *data, uint32_t size)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.bin", sOutDir, sDumpName[0] ? sDumpName : "dump");
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(data, 1, size, f);
        fclose(f);
        printf("frame %u: dump %s\n", Port_FrameCount() + 1, path);
    }
}

void Backend_QueueAudio(const int16_t *stereo, int count, int rate) { (void)stereo; (void)count; (void)rate; }
bool Backend_LoadSave(uint8_t *data, uint32_t size) { (void)data; (void)size; return false; }
void Backend_StoreSave(const uint8_t *data, uint32_t size) { (void)data; (void)size; }

void Backend_Log(const char *fmt, ...)
{
    if (getenv("SA3HOST_QUIET"))
        return;
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

void Backend_Fatal(const char *msg)
{
    fprintf(stderr, "FATAL: %s\n", msg);
    exit(2);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s script.txt [output folder]\n", argv[0]);
        return 1;
    }
    LoadScript(argv[1]);
    if (argc > 2)
        sOutDir = argv[2];
    mkdir(sOutDir, 0777);
    Port_Main();
}
