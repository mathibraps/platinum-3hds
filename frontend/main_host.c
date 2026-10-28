/*
 * Desktop test harness: runs the same game code as the 3DS build for a number
 * of frames with scripted input and writes snapshots of both DS screens as
 * PNG files. Lets the port be checked without a 3DS or devkitPro.
 *
 * Usage: pokeplat_host <romfs dir> <out dir> [options]
 *   --frames N        frames to run (default 600)
 *   --dump F[,F...]   frames to snapshot (default: the last one)
 *   --every N         also snapshot every N frames
 *   --press KEY@F[:N] hold KEY (A B SELECT START RIGHT LEFT UP DOWN R L X Y)
 *                     from frame F for N frames (default 2); repeatable
 *   --touch X,Y@F[:N] touch the bottom screen at X,Y from frame F for N frames
 *   --script FILE     scripted input and snapshots (see frontend/input_script.c)
 *   --save FILE       save data file (default: <out dir>/pokeplat.sav)
 *   --render-all      render every frame (as a device does) and time it (with --gpu-compose: compose it)
 *   --widescreen      render at the 3DS screen sizes (400x240 top, 320x240 bottom)
 *   --rgba5551        render 16-bit pixels as the 3DS build does (snapshots are
 *                     expanded back to RGBA8)
 *   --wav FILE        write the sound output (16-bit stereo WAV)
 *   --gpu-compose     compose the screens through the GPU composition passes
 *                     (run in software, frontend/compose_ref.c), as the 3DS
 *                     build does, falling back to the software compositor
 *                     where the 3DS does
 * Snapshots are written as <out dir>/frameNNNN_top.png / _bottom.png.
 *
 * The game runs its own main loop (NitroMain); each VBlank wait ends a frame
 * here (NtrHost_VBlank). The sound driver runs on the game's thread: each
 * frame it makes that frame's share of a second of output.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "compose_ref.h"
#include "input_script.h"
#include "ntr_shim.h"
#include "port_settings.h"
#include "../platform/src/ntr_g3_internal.h"

void WritePngFile(const char *path, const u32 *rgba, int w, int h);

typedef struct Press {
    u32 key; /* 0 for a touch */
    int x, y;
    int start, length;
} Press;

static const char *sOutDir;
static int sFrames = 600, sEvery;
static int sDumps[256], sNumDumps;
static Press sPresses[256];
static int sNumPresses;
static int sFrame;
static clock_t sStart, sRenderTime;
static BOOL sRenderAll, sOut16, sScriptDumps, sGpuCompose;
static int sComposed[2], sFallbacks[2];
static int sTopW = 256, sTopH = 192, sBottomW = 256, sBottomH = 192;

/* Sound output: optionally to a WAV file; loudness is reported at the end. */
static FILE *sWav;
static u64 sSoundFrames, sSoundLoud, sSoundTicks;
static int sSoundPeak;

static void WriteWavHeader(FILE *f, u32 frames)
{
    u32 rate = (u32)NTR_SOUND_RATE, bytes = frames * 4;
    u8 h[44] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0 };
    u32 riff = 36 + bytes, byteRate = rate * 4;
    memcpy(h + 4, &riff, 4);
    memcpy(h + 24, &rate, 4);
    memcpy(h + 28, &byteRate, 4);
    h[32] = 4;
    h[34] = 16;
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &bytes, 4);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, sizeof(h), f);
    fseek(f, 0, SEEK_END);
}

/* This frame's sound: frame n ends at n / 60 s. */
static void RenderSound(void)
{
    static s16 buf[2 * 1024];
    u64 end = (u64)sFrame * 65457 / 120; /* NTR_SOUND_RATE / 60 per frame */
    int n = (int)(end - sSoundFrames);
    u64 t0 = NtrHost_Ticks();
    NtrSound_Render(buf, n);
    sSoundTicks += NtrHost_Ticks() - t0;
    for (int i = 0; i < 2 * n; i++) {
        int v = buf[i] < 0 ? -buf[i] : buf[i];
        sSoundPeak = v > sSoundPeak ? v : sSoundPeak;
        sSoundLoud += v > 256;
    }
    if (sWav != NULL) {
        fwrite(buf, 4, (size_t)n, sWav);
    }
    sSoundFrames = end;
    if (getenv("PORT_SND") && sFrame % atoi(getenv("PORT_SND")) == 0) {
        void Ntr_SpuDebugPrint(void);
        printf("frame %d sound:\n", sFrame);
        Ntr_SpuDebugPrint();
    }
}

static void FinishSound(void)
{
    extern u64 gNtrSpuTicks;
    printf("sound: %.1f s, peak %d, %.1f%% of samples above -42 dB; made in %.0f ms (mixer %.0f ms)\n", sSoundFrames / NTR_SOUND_RATE,
        sSoundPeak, sSoundFrames ? 50.0 * sSoundLoud / sSoundFrames : 0.0, sSoundTicks / 1e6, gNtrSpuTicks / 1e6);
    if (sWav != NULL) {
        WriteWavHeader(sWav, (u32)sSoundFrames);
        fclose(sWav);
    }
}

static u32 KeyByName(const char *name)
{
    static const struct {
        const char *name;
        u32 bit;
    } keys[] = {
        { "A", PAD_BUTTON_A }, { "B", PAD_BUTTON_B }, { "SELECT", PAD_BUTTON_SELECT }, { "START", PAD_BUTTON_START },
        { "RIGHT", PAD_KEY_RIGHT }, { "LEFT", PAD_KEY_LEFT }, { "UP", PAD_KEY_UP }, { "DOWN", PAD_KEY_DOWN },
        { "R", PAD_BUTTON_R }, { "L", PAD_BUTTON_L }, { "X", PAD_BUTTON_X }, { "Y", PAD_BUTTON_Y },
    };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (strcmp(keys[i].name, name) == 0) {
            return keys[i].bit;
        }
    }
    fprintf(stderr, "unknown key %s\n", name);
    exit(1);
}

/* GPU composition of one engine's screen, run in software. */
static void ComposeScreen(int engine, u32 *out)
{
    static u16 bg[2][4][NTR_LAYER_W * NTR_LAYER_H];
    static u32 obj[2][NTR_LAYER_W * NTR_LAYER_H];
    static u16 objWin[2][NTR_LAYER_W * NTR_LAYER_H];
    static u32 img3d[NTR_MAX_SCREEN_W * NTR_MAX_SCREEN_H];
    static NtrComposition comp;
    NtrLayerImages images = { { bg[engine][0], bg[engine][1], bg[engine][2], bg[engine][3] }, obj[engine], NTR_LAYER_W, objWin[engine] };
    int w, h;
    Ntr_EngineScreenSize(engine, &w, &h);
    /* In two parts, as the 3DS build spreads them over its cores. */
    BOOL ok = Ntr_ComposeBegin(engine, &comp);
    if (ok) {
        Ntr_ComposeLines(engine, &images, 1, 2);
        Ntr_ComposeLines(engine, &images, 0, 2);
        ok = Ntr_ComposeEnd(engine, &images, &comp);
    }
    if (!ok) {
        sFallbacks[engine]++;
        printf("frame %d: engine %d composed in software (%s)\n", sFrame, engine, gNtrComposeFallback);
        Ntr_RenderEngine(engine, out, w);
        return;
    }
    sComposed[engine]++;
    /* The 3D frame as the GPU has it: RGBA8, alpha 0 where there is no pixel. */
    int w3d, h3d;
    Ntr_EngineScreenSize(0, &w3d, &h3d);
    for (int y = 0; y < h3d; y++) {
        for (int x = 0; x < w3d; x++) {
            u16 c = gNtr3DColor[y][x];
            u32 r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31, a = gNtr3DAlpha[y][x];
            img3d[y * w3d + x] = (c & 0x8000) ? 0
                                              : ((r << 3 | r >> 2) << 24) | ((g << 3 | g >> 2) << 16) | ((b << 3 | b >> 2) << 8) | (a * 255 / 31);
        }
    }
    ComposeRef_Run(&comp, &images, img3d, w3d, h3d, w3d, out, w);
}

static void Dump(int frame)
{
    static u32 top[NTR_MAX_SCREEN_W * NTR_MAX_SCREEN_H], bottom[NTR_MAX_SCREEN_W * NTR_MAX_SCREEN_H];
    char path[512];
    if (sGpuCompose) {
        BOOL mainOnTop = Ntr_MainEngineOnTop();
        ComposeScreen(0, mainOnTop ? top : bottom);
        ComposeScreen(1, mainOnTop ? bottom : top);
    } else {
        Ntr_RenderScreens(top, bottom);
    }
    if (sOut16) {
        /* Expand in place, from the end (u16 pixels in the front half). */
        u32 *bufs[2] = { top, bottom };
        int sizes[2] = { sTopW * sTopH, sBottomW * sBottomH };
        for (int b = 0; b < 2; b++) {
            const u16 *p16 = (const u16 *)bufs[b];
            for (int i = sizes[b] - 1; i >= 0; i--) {
                u16 c = p16[i];
                u32 r = c >> 11, g = (c >> 6) & 31, bl = (c >> 1) & 31;
                bufs[b][i] = ((r << 3 | r >> 2) << 24) | ((g << 3 | g >> 2) << 16) | ((bl << 3 | bl >> 2) << 8) | 0xff;
            }
        }
    }
    snprintf(path, sizeof(path), "%s/frame%04d_top.png", sOutDir, frame);
    WritePngFile(path, top, sTopW, sTopH);
    snprintf(path, sizeof(path), "%s/frame%04d_bottom.png", sOutDir, frame);
    WritePngFile(path, bottom, sBottomW, sBottomH);
    printf("frame %d -> %s/frame%04d_{top,bottom}.png\n", frame, sOutDir, frame);
}

void NtrHost_VBlank(void)
{
    sFrame++;
    RenderSound();

    BOOL dump = sEvery > 0 && sFrame % sEvery == 0;
    for (int d = 0; d < sNumDumps; d++) {
        dump |= sDumps[d] == sFrame;
    }
    dump |= InputScript_Dump(sFrame);
    if (getenv("PORT_VBTASKS") && sFrame % 500 == 0) {
        void Port_DebugVBlankTasks(int *freeSlots, int *locked);
        int fs, lk;
        Port_DebugVBlankTasks(&fs, &lk);
        printf("frame %d: vblank tasks sp %d max %d locked %d\n", sFrame, fs & 0xffff, fs >> 16, lk);
    }
    if (getenv("PORT_GESTATS")) {
        /* Geometry commands since the last 3D swap, printed at each swap. */
        extern u32 gNtrGeOpCount[256];
        int every = atoi(getenv("PORT_GESTATS"));
        if (gNtrGeOpCount[0x50] != 0 && every > 0 && sFrame % every < 2) {
            printf("frame %d geometry:", sFrame);
            for (int op = 0; op < 256; op++) {
                if (gNtrGeOpCount[op]) {
                    printf(" %02x:%u", op, gNtrGeOpCount[op]);
                }
            }
            extern u32 gNtrGePath[4], gNtrGeBlocks[8], gNtrGeBlockBegin, gNtrGeBlockWords;
            printf(" polys %d verts %d paths %u/%u/%u/%u blocks(<=4,16,64,256,1k,4k,16k,more) %u %u %u %u %u %u %u %u begin %u words %u\n",
                gNtrG3Polys.count, gNtrG3Polys.vertexCount, gNtrGePath[0], gNtrGePath[1], gNtrGePath[2], gNtrGePath[3], gNtrGeBlocks[0],
                gNtrGeBlocks[1], gNtrGeBlocks[2], gNtrGeBlocks[3], gNtrGeBlocks[4], gNtrGeBlocks[5], gNtrGeBlocks[6], gNtrGeBlocks[7],
                gNtrGeBlockBegin, gNtrGeBlockWords);
            extern u32 gNtrDlCulled, gNtrDlDrawn, gNtrDlState[4], gNtrDlChunksSkipped, gNtrDlChunksDrawn;
            extern u32 gNtrGeFifoWords, gNtrGeFifoOps[256], gNtrGeFifoFromBlocks;
            printf("  one word at a time: %u words (%u in blocks), commands:", gNtrGeFifoWords, gNtrGeFifoFromBlocks);
            for (int op = 1; op < 256; op++) {
                if (gNtrGeFifoOps[op]) {
                    printf(" %02x:%u", op, gNtrGeFifoOps[op]);
                }
            }
            printf("\n");
            extern u32 gNtrGeSyncs[16];
            printf("  read-backs: clip %u vec %u postest %u vectest %u box %u pv %u pj %u; resets %u %u %u\n", gNtrGeSyncs[0], gNtrGeSyncs[1],
                gNtrGeSyncs[2], gNtrGeSyncs[3], gNtrGeSyncs[4], gNtrGeSyncs[5], gNtrGeSyncs[6], gNtrGeSyncs[7], gNtrGeSyncs[8], gNtrGeSyncs[9]);
            printf("  display lists: %u skipped, %u drawn (chunks %u skipped, %u drawn); large blocks by FIFO state %u %u %u %u\n",
                gNtrDlCulled, gNtrDlDrawn, gNtrDlChunksSkipped, gNtrDlChunksDrawn, gNtrDlState[0], gNtrDlState[1], gNtrDlState[2],
                gNtrDlState[3]);
        }
        if (gNtrGeOpCount[0x50] != 0) {
            extern u32 gNtrGePath[4];
            memset(gNtrGeOpCount, 0, sizeof(gNtrGeOpCount));
            memset(gNtrGePath, 0, sizeof(gNtrGePath));
            extern u32 gNtrGeBlocks[8], gNtrGeBlockBegin, gNtrGeBlockWords;
            memset(gNtrGeBlocks, 0, sizeof(gNtrGeBlocks));
            gNtrGeBlockBegin = gNtrGeBlockWords = 0;
            extern u32 gNtrDlCulled, gNtrDlDrawn;
            gNtrDlCulled = gNtrDlDrawn = 0;
            extern u32 gNtrDlChunksSkipped, gNtrDlChunksDrawn;
            gNtrDlChunksSkipped = gNtrDlChunksDrawn = 0;
            extern u32 gNtrDlState[4];
            memset(gNtrDlState, 0, sizeof(gNtrDlState));
            extern u32 gNtrGeSyncs[16];
            memset(gNtrGeSyncs, 0, sizeof(gNtrGeSyncs));
            extern u32 gNtrGeFifoWords, gNtrGeFifoOps[256], gNtrGeFifoFromBlocks;
            gNtrGeFifoWords = gNtrGeFifoFromBlocks = 0;
            memset(gNtrGeFifoOps, 0, sizeof(gNtrGeFifoOps));
        }
    }
    if (getenv("PORT_FSLOG")) {
        printf("vblank %d\n", sFrame);
    }
    if (getenv("PORT_FTIME")) {
        /* Time since the last VBlank (game code, as nothing is drawn here
           without --dump), printed for every frame from PORT_FTIME on. */
        static double last;
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        double now = ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
        if (sFrame >= atoi(getenv("PORT_FTIME")) && last != 0) {
            printf("ftime %d %.3f\n", sFrame, now - last);
        }
        last = now;
    }
    if (getenv("PORT_POS") && sFrame % atoi(getenv("PORT_POS")) == 0) {
        BOOL Port_DebugPlayerPos(int *map, int *x, int *z, int *dir);
        int map, x, z, dir;
        if (Port_DebugPlayerPos(&map, &x, &z, &dir)) {
            printf("frame %d: player map %d at %d,%d facing %d\n", sFrame, map, x, z, dir);
        }
    }
    if (getenv("PORT_TOUCH")) {
        void Port_DebugTouchState(u16 *x, u16 *y, u16 *pressed, u16 *held, BOOL *autoSampling);
        u16 x, y, pr, he;
        BOOL as;
        Port_DebugTouchState(&x, &y, &pr, &he, &as);
        if (pr || he) {
            printf("frame %d: touch %u,%u pressed %u held %u auto %d\n", sFrame, x, y, pr, he, as);
        }
    }
    if (dump && getenv("PORT_REGS")) {
        for (int e = 0; e < 2; e++) {
            u32 io = e ? 0x1000 : 0;
            printf("frame %d %s: dispcnt %08x bgcnt %04x %04x %04x %04x bldcnt %04x alpha %04x bldy %04x mb %04x win %04x/%04x"
                   " win0 %04x %04x win1 %04x %04x\n",
                sFrame, e ? "sub" : "main", NTR_IO32(io), NTR_IO16(io + 8), NTR_IO16(io + 10), NTR_IO16(io + 12), NTR_IO16(io + 14),
                NTR_IO16(io + 0x50), NTR_IO16(io + 0x52), NTR_IO16(io + 0x54), NTR_IO16(io + 0x6c), NTR_IO16(io + 0x48),
                NTR_IO16(io + 0x4a), NTR_IO16(io + 0x40), NTR_IO16(io + 0x44), NTR_IO16(io + 0x42), NTR_IO16(io + 0x46));
            printf("frame %d %s: bg0 scroll %d,%d\n", sFrame, e ? "sub" : "main", NTR_IO16(io + 0x10), NTR_IO16(io + 0x12));
            if (e == 0) {
                printf("frame %d main: 3D clear %08x disp3dcnt %04x\n", sFrame, NTR_IO32(0x350), NTR_IO16(0x60));
            }
        }
    }
    if (dump) {
        Dump(sFrame);
    } else if (!sRenderAll) {
        Ntr_TrackFrame(); /* as a device, which renders every frame, follows scrolling */
    }
    if (dump) {
    } else if (sRenderAll) {
        static u32 top[NTR_MAX_SCREEN_W * NTR_MAX_SCREEN_H], bottom[NTR_MAX_SCREEN_W * NTR_MAX_SCREEN_H];
        clock_t t = clock();
        if (sGpuCompose) {
            BOOL mainOnTop = Ntr_MainEngineOnTop();
            extern u64 gNtrExtTicks[4];
            u64 st[2][3], ex[4];
            memcpy(st, gNtrRenderTicks, sizeof(st));
            memcpy(ex, gNtrExtTicks, sizeof(ex));
            u64 c0 = NtrHost_Ticks();
            ComposeScreen(0, mainOnTop ? top : bottom);
            u64 c1 = NtrHost_Ticks();
            ComposeScreen(1, mainOnTop ? bottom : top);
            u64 c2 = NtrHost_Ticks();
            if (getenv("PORT_COMPOSETIME")) {
                /* Per frame, in microseconds: each engine's whole composition, its BG / OBJ / pass stages,
                   and the widescreen stages (column search, columns, rows, regions and passes). */
                double us = 1e6 / NtrHost_TicksPerSecond();
                printf("compose %d: main %.0f sub %.0f | main bg %.0f obj %.0f pass %.0f | sub bg %.0f obj %.0f pass %.0f | ext %.0f %.0f %.0f %.0f\n",
                    sFrame, (c1 - c0) * us, (c2 - c1) * us, (gNtrRenderTicks[0][0] - st[0][0]) * us, (gNtrRenderTicks[0][1] - st[0][1]) * us,
                    (gNtrRenderTicks[0][2] - st[0][2]) * us, (gNtrRenderTicks[1][0] - st[1][0]) * us, (gNtrRenderTicks[1][1] - st[1][1]) * us,
                    (gNtrRenderTicks[1][2] - st[1][2]) * us, (gNtrExtTicks[0] - ex[0]) * us, (gNtrExtTicks[1] - ex[1]) * us,
                    (gNtrExtTicks[2] - ex[2]) * us, (gNtrExtTicks[3] - ex[3]) * us);
            }
        } else {
            Ntr_RenderScreens(top, bottom);
        }
        sRenderTime += clock() - t;
    }
    if (sFrame >= sFrames) {
        double secs = (double)(clock() - sStart) / CLOCKS_PER_SEC;
        double render = (double)sRenderTime / CLOCKS_PER_SEC;
        if (sRenderAll) {
            double raster = (double)gNtrRasterTicks / NtrHost_TicksPerSecond();
            printf("%d frames in %.2fs: game %.2f ms/frame (3D rasterizer %.2f), 2D rendering %.2f ms/frame\n", sFrame, secs,
                (secs - render) * 1000 / sFrame, raster * 1000 / sFrame, render * 1000 / sFrame);
        } else {
            printf("%d frames in %.2fs (%.2f ms/frame, without rendering)\n", sFrame, secs, secs * 1000 / sFrame);
        }
        extern u32 gNtrFsGameOpens, gNtrFsGameReads;
        printf("files: the game opened %u and read %u times; %u reads of %u KB from the host\n", gNtrFsGameOpens, gNtrFsGameReads,
            gNtrFsHostReads, gNtrFsHostBytes / 1024);
        if (sGpuCompose) {
            printf("GPU composition: main %d snapshots (%d by the software compositor), sub %d (%d)\n", sComposed[0] + sFallbacks[0],
                sFallbacks[0], sComposed[1] + sFallbacks[1], sFallbacks[1]);
        }
        FinishSound();
        exit(0);
    }

    /* Input for the next frame. */
    u32 keys = 0;
    BOOL touching = FALSE;
    u16 tx = 0, ty = 0;
    int next = sFrame + 1;
    for (int p = 0; p < sNumPresses; p++) {
        if (next >= sPresses[p].start && next < sPresses[p].start + sPresses[p].length) {
            if (sPresses[p].key != 0) {
                keys |= sPresses[p].key;
            } else {
                touching = TRUE;
                tx = (u16)sPresses[p].x;
                ty = (u16)sPresses[p].y;
            }
        }
    }
    u16 k16 = (u16)keys;
    InputScript_Input(next, &k16, &touching, &tx, &ty);
    Ntr_SetInput(k16, touching, tx, ty);
}

u64 NtrHost_Ticks(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000u + (u64)ts.tv_nsec;
}

u64 NtrHost_TicksPerSecond(void)
{
    return 1000000000u;
}

void NtrHost_Reset(u32 parameter)
{
    printf("frame %d: the game reset (parameter %u)\n", sFrame, parameter);
    FinishSound();
    exit(0);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <romfs dir> <out dir> [--frames N] [--dump F,F...] [--every N] [--press KEY@F[:N]]... "
                        "[--touch X,Y@F[:N]]... [--save FILE]\n",
            argv[0]);
        return 1;
    }
    sOutDir = argv[2];
    char savePath[512];
    snprintf(savePath, sizeof(savePath), "%s/pokeplat.sav", sOutDir);

    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            sFrames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--every") == 0 && i + 1 < argc) {
            sEvery = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--dump") == 0 && i + 1 < argc) {
            for (char *tok = strtok(argv[++i], ","); tok && sNumDumps < 256; tok = strtok(NULL, ",")) {
                sDumps[sNumDumps++] = atoi(tok);
            }
        } else if (strcmp(argv[i], "--press") == 0 && i + 1 < argc && sNumPresses < 256) {
            char name[16] = { 0 };
            int start = 0, length = 2;
            if (sscanf(argv[++i], "%15[A-Z]@%d:%d", name, &start, &length) < 2) {
                fprintf(stderr, "bad --press %s\n", argv[i]);
                return 1;
            }
            sPresses[sNumPresses++] = (Press){ KeyByName(name), 0, 0, start, length };
        } else if (strcmp(argv[i], "--touch") == 0 && i + 1 < argc && sNumPresses < 256) {
            int x, y, start = 0, length = 2;
            if (sscanf(argv[++i], "%d,%d@%d:%d", &x, &y, &start, &length) < 3) {
                fprintf(stderr, "bad --touch %s\n", argv[i]);
                return 1;
            }
            sPresses[sNumPresses++] = (Press){ 0, x, y, start, length };
        } else if (strcmp(argv[i], "--widescreen") == 0) {
            sTopW = 400, sTopH = 240, sBottomW = 320, sBottomH = 240;
        } else if (strcmp(argv[i], "--rgba5551") == 0) {
            sOut16 = TRUE;
        } else if (strcmp(argv[i], "--gpu-compose") == 0) {
            sGpuCompose = TRUE;
        } else if (strcmp(argv[i], "--render-all") == 0) {
            sRenderAll = TRUE;
        } else if (strcmp(argv[i], "--script") == 0 && i + 1 < argc) {
            FILE *f = fopen(argv[++i], "rb");
            if (f == NULL) {
                fprintf(stderr, "cannot open %s\n", argv[i]);
                return 1;
            }
            static char text[1 << 20];
            size_t n = fread(text, 1, sizeof(text) - 1, f);
            text[n] = 0;
            fclose(f);
            if (!InputScript_Parse(text)) {
                return 1;
            }
            Ntr_SetDeterministicTime(TRUE);
            if (InputScript_Frames() > 0) {
                sFrames = InputScript_Frames();
                sScriptDumps = TRUE;
            }
        } else if (strcmp(argv[i], "--wav") == 0 && i + 1 < argc) {
            sWav = fopen(argv[++i], "wb");
            if (sWav == NULL) {
                fprintf(stderr, "cannot write %s\n", argv[i]);
                return 1;
            }
            WriteWavHeader(sWav, 0);
        } else if (strcmp(argv[i], "--save") == 0 && i + 1 < argc) {
            snprintf(savePath, sizeof(savePath), "%s", argv[++i]);
        } else {
            fprintf(stderr, "unknown option %s\n", argv[i]);
            return 1;
        }
    }
    if (sGpuCompose && sOut16) {
        fprintf(stderr, "--gpu-compose renders RGBA8 snapshots; leave out --rgba5551\n");
        return 1;
    }
    if (sNumDumps == 0 && sEvery == 0 && !sScriptDumps) {
        sDumps[sNumDumps++] = sFrames;
    }

    NtrFS_SetRoot(argv[1]);
    Ntr_SetBackupFile(savePath);
    char settingsPath[600];
    snprintf(settingsPath, sizeof(settingsPath), "%s/settings.txt", sOutDir);
    Port_SetSettingsFile(settingsPath);
    Ntr_SetScreenSizes(sTopW, sTopH, sBottomW, sBottomH);
    Ntr_SetOutputRGBA5551(sOut16);
    NtrSound_Start(FALSE, NULL, NULL);
    sStart = clock();
    NitroMain(); /* never returns */
    return 0;
}
