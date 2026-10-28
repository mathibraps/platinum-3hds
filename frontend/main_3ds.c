/*
 * Nintendo 3DS entry point.
 *
 * Widescreen: the game renders at the 3DS screen sizes (400x240 top, 320x240
 * bottom), 1:1 pixels; the DS picture is in the middle and 3D scenes fill the
 * rest (see Ntr_SetScreenSizes). The DS touch screen is the middle 256x192 of
 * the bottom screen. The extracted ROM file system is read from romfs.
 * Save data is kept in sdmc:/3ds/pokeplat/. The game's soft reset (START +
 * SELECT + L + R) quits to the HOME menu.
 *
 * The game runs its own main loop (NitroMain); each time it waits for VBlank,
 * NtrHost_VBlank shows the frame and reads the buttons for the next one.
 *
 * Composition is on the GPU (gpu2d_3ds.c): the CPU decodes the 2D layers of
 * each DS screen into images and the GPU combines them with the 3D frame
 * (gpu3d_3ds.c) in the screen's framebuffer. The New 3DS runs at its fast
 * clock and decodes the two screens on two CPU cores. Holding SELECT + R shows
 * how long frames take (left of the top screen).
 *
 * Sound: the DS sound driver (platform/src/ntr_snd.c) runs on an audio
 * thread, as it ran on the DS's second CPU, and makes the output as NDSP
 * plays it. NDSP needs the DSP firmware (sdmc:/3ds/dspfirm.cdc, which the
 * DSP1 homebrew dumps); without it the game runs silent.
 */
#include <errno.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>

#include "gpu2d_3ds.h"
#include "gpu3d_3ds.h"
#include "input_script.h"
#include "ntr_shim.h"
#include "port_settings.h"

#define DS_W 256
#define DS_H 192

/* Framebuffer to screen, as citro2d does it. */
#define DISPLAY_TRANSFER_FLAGS                                                                      \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0)                  \
        | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) \
        | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

typedef struct Screen {
    int w, h; /* the screen: 400x240 or 320x240 */
    C3D_RenderTarget *target; /* RGBA8 with a stencil buffer, for composition */
} Screen;

/* The game was written for small DS stacks, but GCC's frames differ from
 * Metrowerks'; give the main thread plenty (libctru reads this symbol). */
u32 __stacksize__ = 0x100000;

static Screen sTop, sBottom;
static u64 sFrameStart;
static float sFrameMs;

/* Frame time breakdown, averaged over the last 60 frames, in microseconds.
 * Read by tools/emu_perf.py from the emulator's memory (see the .map file). */
typedef struct PortPerf {
    u32 magic; /* 'PERF' */
    u32 frames;
    u32 frameUs, gameUs, rasterUs, renderUs, presentUs;
    u32 bgUs, objUs, composeUs;
    u32 gpu3dUs, waitUs; /* GPU 3D draw + readback; waiting for the GPU/VBlank before it */
    u32 flags; /* bit 0: render worker on a second core; bit 1: New 3DS; bits 2, 3: main, sub
                  engine composed on the CPU in the last frame */
    u32 cpuMHz; /* measured CPU clock (268 or 804 on hardware) */
    u32 maxGameUs, maxFrameUs; /* the longest of the 60 frames */
    u32 slowFrames; /* frames so far that took longer than two VBlanks */
    u32 soundMode; /* 0: silent (no NDSP), 1: driven by the game's thread, 2: audio thread */
    u32 soundFrames; /* sound output made so far (stereo frames) */
    u32 soundUs; /* audio thread time so far (driver and mixer) */
    u32 soundStarved; /* times NDSP had played everything queued */
    u32 heapKB, heapUsedKB, linearFreeKB; /* the malloc heap and how much of it is in use; free linear memory */
    u32 fileCacheKB, prefetchedKB; /* the ROM file cache, and what the prefetching thread read into it */
    u32 heapFailures; /* game allocations that found no memory */
} PortPerf;

PortPerf gPortPerf = { 0x46524550 };
static u32 sCacheBytes; /* the ROM file cache's size (StartFiles) */

/* The times of the last FRAME_LOG frames, in units of 10 us, for
 * tools/emu_frames.py: the whole frame (end to end), the time it was busy
 * (not waiting for its turn), the game's logic, the 2D work, drawing and
 * submitting after it, and the cache flushes for the GPU within that. */
#define FRAME_LOG 4096
typedef struct PortFrameLog {
    u32 magic; /* 'FLOG' */
    u32 count; /* frames logged so far; frame n is at n % FRAME_LOG */
    u16 frame[FRAME_LOG], busy[FRAME_LOG], game[FRAME_LOG], render[FRAME_LOG], flush[FRAME_LOG];
    u16 geo[FRAME_LOG], raster[FRAME_LOG]; /* in the game's time: geometry commands, 3D frame setup at the swap */
    u16 fs[FRAME_LOG], lz[FRAME_LOG]; /* file reads; LZ77 decompression and waits for the sound driver */
    u16 compose[FRAME_LOG], draw2d[FRAME_LOG], present[FRAME_LOG]; /* in the 2D work: finishing the layers and
                                                                        passes, drawing them, ending the GPU frame */
    u16 stage[4][FRAME_LOG]; /* builds with -DNTR_PROFILE_STAGES: both engines' BG lines, OBJ lines and passes,
                                and the widescreen extension (summed over the cores that did them) */
    u16 tex[3][FRAME_LOG]; /* 3D textures decoded, their texels (in thousands), and hash checks */
} PortFrameLog;

PortFrameLog gPortFrameLog = { 0x474f4c46 };
static u64 sLastFlushTicks, sLastGeTicks, sLastRasterTicks, sLastFsTicks, sLastLzTicks, sLastSoundWaitTicks;

static u16 To10Us(u64 ticks)
{
    u64 v = ticks * 100000 / SYSCLOCK_ARM11;
    return (u16)(v > 0xffff ? 0xffff : v);
}

/* Fixed benchmark windows of the opening (no input): the same frames for every
 * build, so builds can be compared (tools/emu_perf.py prints them). Per window:
 * frame, game, 2D render, GPU 3D, then (with -DNTR_PROFILE_STAGES) the BG,
 * OBJ and composition stages of both engines, in microseconds per frame. */
#define BENCH_FRAMES 60
#define BENCH_VALUES 16 /* ... then the longest game and the longest work (all but waiting) of a frame,
                          then (NTR_PROFILE_STAGES) composition: column search, columns, rows, regions and passes */
extern u64 gNtrExtTicks[4];
static u64 sBenchExt[4];
#define BENCH_WINDOWS 4
static const u32 sBenchStart[BENCH_WINDOWS] = { 600, 1500, 2250, 3600 }; /* copyright, logo, field, sprite bands */
u32 gPortBench[BENCH_WINDOWS][BENCH_VALUES];
/* The game time of each frame of the field window, in microseconds. */
u32 gPortBenchGame[BENCH_FRAMES];
static u64 sBenchAcc[BENCH_VALUES];
static u32 sFrameCount;
static u64 sBenchStage[2][3];
static u64 sAccGame, sAccRaster, sAccRender, sAccPresent, sAccFrame, sLastRaster, sAccGpu3D, sAccWait;
static u32 sAccFrames;
static u64 sMaxGame, sMaxFrame;
static u64 sLastStage[3];

/* The New 3DS has a spare CPU core: a worker decodes half of the 2D layers
 * there while the main thread does the other half, then finishes the sub
 * engine's composition while the main thread finishes the main engine's. */
static Thread sWorker;
static LightEvent sWorkStart, sWorkDone;
static volatile bool sWorkerQuit;
static volatile int sWorkerJob; /* 0: Gpu2D_Work(0), 1: Gpu2D_EndEngine(1) */

static void WorkerMain(void *arg)
{
    (void)arg;
    for (;;) {
        LightEvent_Wait(&sWorkStart);
        if (sWorkerQuit) {
            break;
        }
        if (sWorkerJob == 0) {
            Gpu2D_Work(0);
        } else {
            Gpu2D_EndEngine(1);
        }
        LightEvent_Signal(&sWorkDone);
    }
}

static void StartWorker(void)
{
    bool isNew3DS = false;
    APT_CheckNew3DS(&isNew3DS);
    if (!isNew3DS) {
        return;
    }
    LightEvent_Init(&sWorkStart, RESET_ONESHOT);
    LightEvent_Init(&sWorkDone, RESET_ONESHOT);
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    sWorker = threadCreate(WorkerMain, NULL, 64 * 1024, prio, 2, false);
    gPortPerf.flags |= 2 | (sWorker != NULL ? 1 : 0);
}

/* The worker starts on its half while the main thread draws the 3D frame,
 * then does its own half (FinishScreens). */
static void StartScreens(void)
{
    Gpu2D_Begin();
    if (sWorker != NULL) {
        sWorkerJob = 0;
        LightEvent_Signal(&sWorkStart);
    }
}

static void FinishScreens(void)
{
    if (sWorker == NULL) {
        Gpu2D_Work(0);
        Gpu2D_Work(1);
        Gpu2D_EndEngine(0);
        Gpu2D_EndEngine(1);
    } else {
        Gpu2D_Work(1);
        LightEvent_Wait(&sWorkDone);
        /* Both engines finished at once only when both have new layers:
         * otherwise handing one to the worker costs more than it saves. */
        if (Gpu2D_EngineHasNewLayers(0) && Gpu2D_EngineHasNewLayers(1)) {
            sWorkerJob = 1;
            LightEvent_Signal(&sWorkStart);
            Gpu2D_EndEngine(0);
            LightEvent_Wait(&sWorkDone);
        } else {
            Gpu2D_EndEngine(0);
            Gpu2D_EndEngine(1);
        }
    }
    Gpu2D_End();
}

/* Sound -------------------------------------------------------------------- */

/* NDSP plays a ring of buffers; the audio thread refills each as it finishes
 * (NDSP calls back every DSP frame, about 5 ms): AUDIO_CHUNK frames each,
 * about 31 ms queued in all. */
#define AUDIO_BUFS 4
#define AUDIO_CHUNK 256
#define AUDIO_BUF_FRAMES 1024 /* room for a whole frame's sound (test runs) */

static ndspWaveBuf sWaveBufs[AUDIO_BUFS];
static int sNextWaveBuf;
static bool sAudioOn, sAudioThreaded;
static Thread sAudioThread;
static LightEvent sAudioEvent;
static volatile bool sAudioQuit;

static void AudioWake(void)
{
    LightEvent_Signal(&sAudioEvent);
}

static void AudioIdle(void)
{
    svcSleepThread(100000); /* 0.1 ms */
}

static void NdspFrame(void *arg)
{
    (void)arg;
    LightEvent_Signal(&sAudioEvent);
}

/* The next buffer in the ring, if NDSP is done with it. */
static ndspWaveBuf *FreeWaveBuf(void)
{
    ndspWaveBuf *wb = &sWaveBufs[sNextWaveBuf];
    return wb->status == NDSP_WBUF_FREE || wb->status == NDSP_WBUF_DONE ? wb : NULL;
}

static void QueueWaveBuf(ndspWaveBuf *wb, int frames)
{
    wb->nsamples = frames;
    DSP_FlushDataCache(wb->data_pcm16, frames * 4);
    ndspChnWaveBufAdd(0, wb);
    sNextWaveBuf = (sNextWaveBuf + 1) % AUDIO_BUFS;
}

static void AudioMain(void *arg)
{
    (void)arg;
    while (!sAudioQuit) {
        LightEvent_Wait(&sAudioEvent);
        u64 start = svcGetSystemTick();
        NtrSound_Service();
        int free = 0;
        for (int i = 0; i < AUDIO_BUFS; i++) {
            free += sWaveBufs[i].status == NDSP_WBUF_FREE || sWaveBufs[i].status == NDSP_WBUF_DONE;
        }
        if (free == AUDIO_BUFS && gPortPerf.soundFrames != 0) {
            gPortPerf.soundStarved++;
        }
        for (ndspWaveBuf *wb; (wb = FreeWaveBuf()) != NULL;) {
            NtrSound_Render(wb->data_pcm16, AUDIO_CHUNK);
            QueueWaveBuf(wb, AUDIO_CHUNK);
            gPortPerf.soundFrames += AUDIO_CHUNK;
        }
        gPortPerf.soundUs += (u32)((svcGetSystemTick() - start) * 1000000 / SYSCLOCK_ARM11);
    }
}

/* Test runs drive the driver from the game's thread, a frame's share of the
 * output each frame (frame n ends at n / 60 s), so they repeat exactly; so
 * does a console without NDSP, silently. */
static void RenderFrameSound(void)
{
    static u32 frames;
    static u64 done;
    static s16 scratch[2 * AUDIO_BUF_FRAMES];
    u64 end = (u64)++frames * 65457 / 120; /* NTR_SOUND_RATE / 60 per frame */
    int n = (int)(end - done);
    done = end;
    ndspWaveBuf *wb = sAudioOn ? FreeWaveBuf() : NULL;
    NtrSound_Render(wb != NULL ? wb->data_pcm16 : scratch, n);
    if (wb != NULL) {
        QueueWaveBuf(wb, n);
    }
    gPortPerf.soundFrames += n;
}

static void StartSound(bool threaded)
{
    sAudioOn = R_SUCCEEDED(ndspInit());
    if (sAudioOn) {
        ndspSetOutputMode(NDSP_OUTPUT_STEREO);
        ndspChnReset(0);
        ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
        ndspChnSetRate(0, (float)NTR_SOUND_RATE);
        ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
        float mix[12] = { 1.0f, 1.0f };
        ndspChnSetMix(0, mix);
        s16 *mem = linearAlloc(AUDIO_BUFS * AUDIO_BUF_FRAMES * 4);
        if (mem == NULL) {
            ndspExit();
            sAudioOn = false;
        } else {
            memset(mem, 0, AUDIO_BUFS * AUDIO_BUF_FRAMES * 4);
            memset(sWaveBufs, 0, sizeof(sWaveBufs));
            for (int i = 0; i < AUDIO_BUFS; i++) {
                sWaveBufs[i].data_pcm16 = mem + i * AUDIO_BUF_FRAMES * 2;
            }
        }
    }
    /* The thread waits for its first wake-up, after the driver starts. */
    if (threaded && sAudioOn) {
        LightEvent_Init(&sAudioEvent, RESET_ONESHOT);
        /* Above the game's priority, on the New 3DS's spare core (with the
         * 2D worker) or else beside the game. */
        bool isNew3DS = false;
        APT_CheckNew3DS(&isNew3DS);
        s32 prio = 0x30;
        svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
        sAudioThread = threadCreate(AudioMain, NULL, 32 * 1024, prio - 1, isNew3DS ? 2 : 0, false);
        if (sAudioThread == NULL) {
            sAudioThread = threadCreate(AudioMain, NULL, 32 * 1024, prio - 1, 0, false);
        }
    }
    sAudioThreaded = sAudioThread != NULL;
    gPortPerf.soundMode = sAudioThreaded ? 2 : sAudioOn ? 1 : 0;
    NtrSound_Start(sAudioThreaded, AudioWake, AudioIdle);
    if (sAudioThreaded) {
        ndspSetCallback(NdspFrame, NULL);
        AudioWake();
    }
}

static void StopSound(void)
{
    if (sAudioThread != NULL) {
        sAudioQuit = true;
        AudioWake();
        threadJoin(sAudioThread, U64_MAX);
        threadFree(sAudioThread);
        sAudioThread = NULL;
    }
    if (sAudioOn) {
        ndspExit();
        sAudioOn = false;
    }
}

u64 NtrHost_Ticks(void)
{
    return svcGetSystemTick();
}

u64 NtrHost_TicksPerSecond(void)
{
    return SYSCLOCK_ARM11;
}

/* The CPU clock, measured with a loop of known cycle count (one dependent
 * add per iteration) against the system tick, which is fixed at 268 MHz. */
static void __attribute__((noinline)) MeasureCpuClock(void)
{
    const u32 iterations = 4000000;
    u64 t0 = svcGetSystemTick();
    u32 x = 0;
    for (u32 i = 0; i < iterations; i++) {
        __asm__ volatile("add %0, %0, #1" : "+r"(x));
    }
    u64 ticks = svcGetSystemTick() - t0;
    /* The loop is about 3 instructions per iteration (add, add, cmp+branch). */
    gPortPerf.cpuMHz = ticks ? (u32)((u64)iterations * 3 * (SYSCLOCK_ARM11 / 1000000) / ticks) : 0;
}

static u32 ToUs(u64 ticks, u32 frames)
{
    return (u32)(ticks * 1000000u / SYSCLOCK_ARM11 / frames);
}

/* 3DS buttons to DS PAD bits. */
static u32 ReadDSKeys(u32 held)
{
    static const struct {
        u32 ctr, ds;
    } map[] = {
        { KEY_A, PAD_BUTTON_A }, { KEY_B, PAD_BUTTON_B }, { KEY_SELECT, PAD_BUTTON_SELECT }, { KEY_START, PAD_BUTTON_START },
        { KEY_RIGHT, PAD_KEY_RIGHT }, { KEY_LEFT, PAD_KEY_LEFT }, { KEY_UP, PAD_KEY_UP }, { KEY_DOWN, PAD_KEY_DOWN },
        { KEY_R, PAD_BUTTON_R }, { KEY_L, PAD_BUTTON_L }, { KEY_X, PAD_BUTTON_X }, { KEY_Y, PAD_BUTTON_Y },
    };
    u32 ds = 0;
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (held & map[i].ctr) {
            ds |= map[i].ds;
        }
    }
    return ds;
}

static void InitScreen(Screen *s, gfxScreen_t screen, int width)
{
    s->w = width;
    s->h = 240;
    /* Screen framebuffers are rotated: 240 wide, `width` high. */
    s->target = C3D_RenderTargetCreate(240, width, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    C3D_RenderTargetSetOutput(s->target, screen, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
}

/* Frame time readout, left of the top screen: a bar (white line = 1/60 s) and
 * the breakdown in milliseconds, averaged over 60 frames. */
static C2D_TextBuf sTextBuf;

/* Shown for the first seconds when NDSP could not start. */
static void DrawNoSoundNotice(void)
{
    C2D_Text text;
    C2D_TextBufClear(sTextBuf);
    C2D_TextParse(&text, sTextBuf, "No sound: the DSP firmware is missing.\nRun DSP1 once to dump it to\nsdmc:/3ds/dspfirm.cdc.");
    C2D_DrawRectSolid(0, 0, 0.6f, 320, 44, C2D_Color32(0, 0, 0, 200));
    C2D_DrawText(&text, C2D_WithColor, 6, 4, 0.7f, 0.45f, 0.45f, C2D_Color32(255, 255, 255, 255));
}

static void DrawPerfOverlay(void)
{
    float h = sFrameMs / (1000.0f / 60.0f) * 120.0f;
    if (h > 240.0f) {
        h = 240.0f;
    }
    u32 color = sFrameMs <= 1000.0f / 60.0f + 0.5f ? C2D_Color32(0, 200, 0, 255) : C2D_Color32(220, 0, 0, 255);
    C2D_DrawRectSolid(4, 240 - h, 0.5f, 8, h, color);
    C2D_DrawRectSolid(0, 120, 0.6f, 16, 1, C2D_Color32(255, 255, 255, 255));

    char str[160];
    snprintf(str, sizeof(str), "frame\n%.1f\nmax\n%.1f\ngame\n%.1f\n3D\n%.1f\n2D\n%.1f\nwait\n%.1f\nGPU\n%.1f\nms\n%lu\nMHz",
        gPortPerf.frameUs / 1000.0f, gPortPerf.maxFrameUs / 1000.0f, gPortPerf.gameUs / 1000.0f,
        (gPortPerf.rasterUs + gPortPerf.gpu3dUs) / 1000.0f, gPortPerf.renderUs / 1000.0f,
        (gPortPerf.presentUs + gPortPerf.waitUs) / 1000.0f, C3D_GetDrawingTime(), (unsigned long)gPortPerf.cpuMHz);
    C2D_Text text;
    C2D_TextBufClear(sTextBuf);
    C2D_TextParse(&text, sTextBuf, str);
    /* On a dark box: the overlay is drawn over the game picture now. */
    C2D_DrawRectSolid(14, 4, 0.6f, 50, 232, C2D_Color32(0, 0, 0, 160));
    C2D_DrawText(&text, C2D_WithColor, 18, 8, 0.7f, 0.42f, 0.42f, C2D_Color32(255, 255, 255, 255));
}

/* The save file is written by a thread behind the game (one write for all
 * the flash requests of a save, rather than a file write for each, which
 * held the game up for a third of a second). */
static LightLock sSaveLock;
static LightEvent sSaveEvent;
static Thread sSaveThread;
static volatile bool sSaveQuit;

static void SaveLock(void)
{
    LightLock_Lock(&sSaveLock);
}

static void SaveUnlock(void)
{
    LightLock_Unlock(&sSaveLock);
}

static void SaveWake(void)
{
    LightEvent_Signal(&sSaveEvent);
}

static void SaveMain(void *arg)
{
    (void)arg;
    while (!sSaveQuit) {
        LightEvent_Wait(&sSaveEvent);
        while (Ntr_BackupWriteChanges()) {
        }
    }
}

static void StartSaveWriter(bool isNew3DS)
{
    if (gNtrDebugSkip & NTR_SKIP_SAVE_WRITER) {
        return;
    }
    LightLock_Init(&sSaveLock);
    LightEvent_Init(&sSaveEvent, RESET_ONESHOT);
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    sSaveThread = threadCreate(SaveMain, NULL, 16 * 1024, prio + 1, isNew3DS ? 2 : 0, false);
    if (sSaveThread != NULL) {
        Ntr_SetBackupWriter(SaveWake, SaveLock, SaveUnlock);
    }
}

static void StopSaveWriter(void)
{
    if (sSaveThread != NULL) {
        sSaveQuit = true;
        SaveWake();
        threadJoin(sSaveThread, U64_MAX);
        threadFree(sSaveThread);
        sSaveThread = NULL;
    }
    while (Ntr_BackupWriteChanges()) { /* whatever is left */
    }
}

static void Quit(void)
{
    StopSaveWriter();
    StopSound();
    if (sWorker != NULL) {
        sWorkerQuit = true;
        LightEvent_Signal(&sWorkStart);
        threadJoin(sWorker, U64_MAX);
        threadFree(sWorker);
    }
    C2D_Fini();
    C3D_Fini();
    gfxExit();
    /* romfs stays mounted: exit() closes the game's open files, which need it
     * (unmounting first made closing the app crash). */
    exit(0);
}

/* Tests: with sdmc:/pokeplat_script.txt present, the game plays that script
 * (frontend/input_script.c) with a deterministic clock and saves the screens
 * it asks for as sdmc:/pokeplat_dumps/frameNNNN_{top,bottom}.raw (RGBA8;
 * tools/raw_to_png.py converts them), then quits. It starts from the save
 * sdmc:/pokeplat_test_save.sav when that exists. The desktop harness plays
 * the same scripts, so the two can be compared. */
static BOOL sTestScript;
static int sVBlanks;

/* Reads the screen's framebuffer back. Call after C3D_FrameEnd: inside a
 * frame citro3d only queues the transfer, and the draws run from FrameEnd. */
static void DumpScreen(const Screen *s, const char *name)
{
    static u32 *linear;
    static u32 row[400];
    if (linear == NULL) {
        linear = linearAlloc(240 * 400 * 4);
    }
    C3D_SyncDisplayTransfer((u32 *)s->target->frameBuf.colorBuf, GX_BUFFER_DIM(240, s->w), linear, GX_BUFFER_DIM(240, s->w),
        GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8)
            | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    GSPGPU_InvalidateDataCache(linear, 240 * 400 * 4);

    char path[96];
    snprintf(path, sizeof(path), "sdmc:/pokeplat_dumps/frame%04d_%s.raw", sVBlanks, name);
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return;
    }
    u16 header[4] = { 0x3838, 0x3838, (u16)s->w, (u16)s->h }; /* "8888" */
    fwrite(header, sizeof(header), 1, f);
    /* The framebuffer is the screen turned: pixel (x, y) at x * 240 + 239 - y. */
    for (int y = 0; y < s->h; y++) {
        for (int x = 0; x < s->w; x++) {
            row[x] = linear[x * 240 + (239 - y)];
        }
        fwrite(row, 4, (size_t)s->w, f);
    }
    fclose(f);
}

static void LoadTestScript(void)
{
    FILE *f = fopen("sdmc:/pokeplat_script.txt", "rb");
    if (f == NULL) {
        return;
    }
    static char text[64 * 1024];
    size_t n = Ntr_ReadPieces(text, sizeof(text) - 1, f);
    text[n] = 0;
    fclose(f);
    InputScript_Parse(text);
    mkdir("sdmc:/pokeplat_dumps", 0777);
    /* Every run starts from sdmc:/pokeplat_test_save.sav if present, else
     * without a save. */
    remove("sdmc:/pokeplat_dumps/test.sav");
    FILE *in = fopen("sdmc:/pokeplat_test_save.sav", "rb");
    if (in != NULL) {
        FILE *out = fopen("sdmc:/pokeplat_dumps/test.sav", "wb");
        static u8 buf[NTR_FILE_PIECE];
        size_t n;
        while (out != NULL && (n = fread(buf, 1, sizeof(buf), in)) > 0) {
            fwrite(buf, 1, n, out);
        }
        if (out != NULL) {
            fclose(out);
        }
        fclose(in);
    }
    Ntr_SetDeterministicTime(TRUE);
    sTestScript = TRUE;
}

/* Frames are paced to 60 Hz on average rather than each to its own VBlank:
 * the overworld runs its logic on every other frame, so one frame can take
 * longer than a VBlank period as long as the next is short. A frame ahead of
 * the schedule waits for VBlank; one behind it goes straight on. */
#define FRAME_TICKS ((u64)(SYSCLOCK_ARM11 / 59.8261)) /* the 3DS screens' refresh */
static u64 sFrameDue;

static void BeginFrame(void)
{
    u64 now = svcGetSystemTick();
    if (sFrameDue == 0 || now > sFrameDue + 2 * FRAME_TICKS) {
        sFrameDue = now; /* far behind (or the first frame): start over */
    }
    if (now < sFrameDue) {
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        sFrameDue = svcGetSystemTick() + FRAME_TICKS;
    } else {
        C3D_FrameBegin(0);
        sFrameDue += FRAME_TICKS;
    }
}

void NtrHost_VBlank(void)
{
    sVBlanks++;
    u64 gameEnd = svcGetSystemTick();
    if (!sAudioThreaded) {
        RenderFrameSound();
    }

    BeginFrame();
    Gpu3D_SetSupersample(Port_GetSetting(PORT_SETTING_3D_EDGES) != 0);
    u64 waitEnd = svcGetSystemTick();
    Gpu2D_BeginFrame();
    BOOL draw2D = !(gNtrDebugSkip & NTR_SKIP_2D);
    if (draw2D) {
        StartScreens();
    }
    Gpu3D_Render();
    u64 gpuEnd = svcGetSystemTick();
    sAccGpu3D += gpuEnd - waitEnd;
    sAccWait += waitEnd - gameEnd;
    if (draw2D) {
        FinishScreens();
    }
    u64 composeEnd = svcGetSystemTick();

    u64 now = svcGetSystemTick();
    if (sFrameStart != 0) {
        sFrameMs = (float)(now - sFrameStart) * 1000.0f / SYSCLOCK_ARM11;
        sAccGame += gameEnd - sFrameStart;
        sAccRender += now - gpuEnd;
        sAccFrame += now - sFrameStart;
        sAccRaster += gNtrRasterTicks - sLastRaster;
        sAccFrames++;
        sMaxGame = gameEnd - sFrameStart > sMaxGame ? gameEnd - sFrameStart : sMaxGame;
        sMaxFrame = now - sFrameStart > sMaxFrame ? now - sFrameStart : sMaxFrame;

        if (now - sFrameStart > 2 * FRAME_TICKS) {
            gPortPerf.slowFrames++;
        }

        sFrameCount++;
        for (int b = 0; b < BENCH_WINDOWS; b++) {
            if (sFrameCount >= sBenchStart[b] && sFrameCount < sBenchStart[b] + BENCH_FRAMES) {
                sBenchAcc[0] += now - sFrameStart;
                sBenchAcc[1] += gameEnd - sFrameStart;
                sBenchAcc[2] += now - gpuEnd;
                sBenchAcc[3] += gpuEnd - waitEnd;
                for (int e = 0; e < 2; e++) {
                    for (int i = 0; i < 3; i++) {
                        sBenchAcc[4 + e * 3 + i] += gNtrRenderTicks[e][i] - sBenchStage[e][i];
                    }
                }
                u64 game = gameEnd - sFrameStart, work = now - sFrameStart - (waitEnd - gameEnd);
                if (b == 2) {
                    gPortBenchGame[sFrameCount - sBenchStart[b]] = ToUs(game, 1);
                }
                sBenchAcc[10] = game > sBenchAcc[10] ? game : sBenchAcc[10];
                sBenchAcc[11] = work > sBenchAcc[11] ? work : sBenchAcc[11];
                for (int k = 0; k < 4; k++) {
                    sBenchAcc[12 + k] += gNtrExtTicks[k] - sBenchExt[k];
                }
                if (sFrameCount == sBenchStart[b] + BENCH_FRAMES - 1) {
                    for (int k = 0; k < BENCH_VALUES; k++) {
                        gPortBench[b][k] = ToUs(sBenchAcc[k], k < 10 || k >= 12 ? BENCH_FRAMES : 1);
                        sBenchAcc[k] = 0;
                    }
                }
            }
        }
    }
    sLastRaster = gNtrRasterTicks;
    memcpy(sBenchStage, gNtrRenderTicks, sizeof(sBenchStage));
    memcpy(sBenchExt, gNtrExtTicks, sizeof(sBenchExt));
    gPortPerf.flags = (gPortPerf.flags & 3) | (Gpu2D_UsedSoftware(0) ? 4 : 0) | (Gpu2D_UsedSoftware(1) ? 8 : 0);

    BOOL mainOnTop = Ntr_MainEngineOnTop();
    u64 drawStart = svcGetSystemTick();
    if (draw2D) {
        Gpu2D_Draw(mainOnTop ? 0 : 1, sTop.target, sTop.w, sTop.h);
    }
#ifdef PORT_PERF_OVERLAY_ALWAYS
    if (true) {
#else
    if ((hidKeysHeld() & (KEY_SELECT | KEY_R)) == (KEY_SELECT | KEY_R)) {
#endif
        C2D_Prepare();
        C2D_SceneBegin(sTop.target);
        DrawPerfOverlay();
        C2D_Flush();
    }
    if (draw2D) {
        Gpu2D_Draw(mainOnTop ? 1 : 0, sBottom.target, sBottom.w, sBottom.h);
    }
    if (!sAudioOn && !sTestScript && sVBlanks < 15 * 60) {
        C2D_Prepare();
        C2D_SceneBegin(sBottom.target);
        DrawNoSoundNotice();
        C2D_Flush();
    }
    u64 drawEnd = svcGetSystemTick();
    C3D_FrameEnd(0);
    u64 presentEnd = svcGetSystemTick();
    sAccPresent += svcGetSystemTick() - now;
    if (sTestScript && InputScript_Dump(sVBlanks)) {
        DumpScreen(&sTop, "top");
        DumpScreen(&sBottom, "bottom");
    }
    if (sTestScript && InputScript_Frames() > 0 && sVBlanks >= InputScript_Frames()) {
        Quit();
    }

    if (sAccFrames >= 60) {
        gPortPerf.frames += sAccFrames;
        gPortPerf.frameUs = ToUs(sAccFrame, sAccFrames);
        gPortPerf.gameUs = ToUs(sAccGame, sAccFrames);
        gPortPerf.rasterUs = ToUs(sAccRaster, sAccFrames);
        gPortPerf.renderUs = ToUs(sAccRender, sAccFrames);
        gPortPerf.presentUs = ToUs(sAccPresent, sAccFrames);
        gPortPerf.gpu3dUs = ToUs(sAccGpu3D, sAccFrames);
        gPortPerf.waitUs = ToUs(sAccWait, sAccFrames);
        gPortPerf.maxGameUs = ToUs(sMaxGame, 1);
        extern u32 __ctru_heap_size;
        struct mallinfo mi = mallinfo();
        gPortPerf.heapKB = __ctru_heap_size / 1024;
        gPortPerf.heapUsedKB = (u32)mi.uordblks / 1024;
        gPortPerf.linearFreeKB = linearSpaceFree() / 1024;
        gPortPerf.fileCacheKB = sCacheBytes / 1024;
        gPortPerf.prefetchedKB = gNtrFsPrefetched / 1024;
        extern u32 gPortHeapFailures;
        gPortPerf.heapFailures = gPortHeapFailures;
        gPortPerf.maxFrameUs = ToUs(sMaxFrame, 1);
        sMaxGame = sMaxFrame = 0;
        /* Stage times of both engines (summed, so more than the wall time
         * when the engines render in parallel). */
        u64 stage[3];
        for (int i = 0; i < 3; i++) {
            stage[i] = gNtrRenderTicks[0][i] + gNtrRenderTicks[1][i];
        }
        gPortPerf.bgUs = ToUs(stage[0] - sLastStage[0], sAccFrames);
        gPortPerf.objUs = ToUs(stage[1] - sLastStage[1], sAccFrames);
        gPortPerf.composeUs = ToUs(stage[2] - sLastStage[2], sAccFrames);
        memcpy(sLastStage, stage, sizeof(sLastStage));
        sAccGame = sAccRaster = sAccRender = sAccPresent = sAccFrame = sAccGpu3D = sAccWait = 0;
        sAccFrames = 0;
    }

    if (!aptMainLoop()) {
        Quit();
    }
    u64 end = svcGetSystemTick();
    if (sFrameStart != 0) {
        u32 i = gPortFrameLog.count % FRAME_LOG;
        gPortFrameLog.frame[i] = To10Us(end - sFrameStart);
        gPortFrameLog.busy[i] = To10Us(end - sFrameStart - (waitEnd - gameEnd));
        gPortFrameLog.game[i] = To10Us(gameEnd - sFrameStart);
        gPortFrameLog.render[i] = To10Us(end - gpuEnd);
        gPortFrameLog.flush[i] = To10Us(gGpuFlushTicks - sLastFlushTicks);
        gPortFrameLog.geo[i] = To10Us(gNtrGeTicks - sLastGeTicks);
        gPortFrameLog.raster[i] = To10Us(gNtrRasterTicks - sLastRasterTicks);
        gPortFrameLog.fs[i] = To10Us(gNtrFsTicks - sLastFsTicks);
        gPortFrameLog.lz[i] = To10Us(gNtrLzTicks - sLastLzTicks + gNtrSoundWaitTicks - sLastSoundWaitTicks);
        gPortFrameLog.compose[i] = To10Us(composeEnd - gpuEnd);
        gPortFrameLog.draw2d[i] = To10Us(drawEnd - drawStart);
        gPortFrameLog.present[i] = To10Us(presentEnd - drawEnd);
        {
            static u64 last[4];
            extern u64 gNtrExtTicks[4];
            u64 now4[4] = { gNtrRenderTicks[0][0] + gNtrRenderTicks[1][0], gNtrRenderTicks[0][1] + gNtrRenderTicks[1][1],
                gNtrRenderTicks[0][2] + gNtrRenderTicks[1][2], gNtrExtTicks[0] + gNtrExtTicks[1] + gNtrExtTicks[2] + gNtrExtTicks[3] };
            for (int k = 0; k < 4; k++) {
                gPortFrameLog.stage[k][i] = To10Us(now4[k] - last[k]);
                last[k] = now4[k];
            }
            extern u32 gGpu3DTexDecodes, gGpu3DTexTexels, gGpu3DTexHashes;
            static u32 lastTex[3];
            u32 nowTex[3] = { gGpu3DTexDecodes, gGpu3DTexTexels / 1000, gGpu3DTexHashes };
            for (int k = 0; k < 3; k++) {
                gPortFrameLog.tex[k][i] = (u16)(nowTex[k] - lastTex[k]);
                lastTex[k] = nowTex[k];
            }
        }
        gPortFrameLog.count++;
    }
    sLastFlushTicks = gGpuFlushTicks;
    sLastGeTicks = gNtrGeTicks;
    sLastRasterTicks = gNtrRasterTicks;
    sLastFsTicks = gNtrFsTicks;
    sLastLzTicks = gNtrLzTicks;
    sLastSoundWaitTicks = gNtrSoundWaitTicks;
    sFrameStart = end;

    hidScanInput();
    u32 held = hidKeysHeld();
    touchPosition touch;
    hidTouchRead(&touch);
    /* The DS touch screen is the middle 256x192 of the bottom screen. */
    int ox = (sBottom.w - DS_W) / 2, oy = (sBottom.h - DS_H) / 2;
    BOOL touching = (held & KEY_TOUCH) && touch.px >= ox && touch.px < ox + DS_W && touch.py >= oy && touch.py < oy + DS_H;
    u16 keys = (u16)ReadDSKeys(held);
    u16 tx = (u16)(touch.px - ox), ty = (u16)(touch.py - oy);
    if (sTestScript) {
        InputScript_Input(sVBlanks + 1, &keys, &touching, &tx, &ty);
    }
    Ntr_SetInput(keys, touching, tx, ty);
}

void NtrHost_Reset(u32 parameter)
{
    (void)parameter;
    Quit();
}

/* ROM files ------------------------------------------------------------------ */

#include "prefetch_list.h"

/* A thread reads ROM files into the file cache ahead of the game, in the
 * time the game leaves free (at a lower priority; on the New 3DS on the
 * spare core), so that loads later find their data in memory. The cache gets
 * most of the free memory, less what the game may still need. */
static LightLock sFsLock;

static void FsLock(void)
{
    LightLock_Lock(&sFsLock);
}

static void FsUnlock(void)
{
    LightLock_Unlock(&sFsLock);
}

static void PrefetchMain(void *arg)
{
    (void)arg;
    NtrFS_Prefetch(sPrefetchList, (int)(sizeof(sPrefetchList) / sizeof(sPrefetchList[0])), sCacheBytes - (4u << 20));
}

static void StartFiles(bool isNew3DS)
{
    extern u32 __ctru_heap_size;
    struct mallinfo mi = mallinfo();
    s64 spare = (s64)__ctru_heap_size - (s64)mi.uordblks - (24 << 20); /* the game's allocations and ours */
    s64 cap = isNew3DS ? 64 << 20 : 16 << 20;
    sCacheBytes = (u32)(spare < (8 << 20) ? 8 << 20 : spare > cap ? cap : spare);
    NtrFS_SetCacheSize(sCacheBytes);
    if (gNtrDebugSkip & NTR_SKIP_PREFETCH) {
        return;
    }
    LightLock_Init(&sFsLock);
    NtrFS_SetLock(FsLock, FsUnlock);
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    if (threadCreate(PrefetchMain, NULL, 16 * 1024, prio + 2, isNew3DS ? 2 : 0, true) == NULL) {
        NtrFS_SetLock(NULL, NULL);
    }
}

/* Times reads of ROM files the way the game's reads go (unbuffered stdio on
 * romfs), for the fsbench test script command: microseconds per read in
 * gPortFsBench, which tools read from the emulator. */
u32 gPortFsBench[64]; /* [0..41] the reads below, [62] a 4KB read through FS:USER directly */

static void RunFsBench(void)
{
    /* Three reads of each size at different places, then a few more. */
    static const u32 sizes[] = { 4096, 6144, 8192, 10240, 12288, 16384, 16388, 20480, 24576, 32768, 49152, 65536, 131072, 524288 };
    struct { u32 size, offset; int sleepMs; } reads[48];
    u32 numReads = 0;
    for (u32 k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
        for (u32 j = 0; j < 3; j++) {
            reads[numReads].size = sizes[k];
            reads[numReads].offset = (k * 3 + j) * 0x40000 + 0x1234 * j;
            reads[numReads++].sleepMs = 0;
        }
    }
    FILE *f = fopen("romfs:/fielddata/land_data/land_data.narc", "rb");
    void *buf = malloc(1 << 20);
    if (f == NULL || buf == NULL) {
        return;
    }
    setvbuf(f, NULL, _IONBF, 0);
    for (u32 i = 0; i < numReads; i++) {
        if (reads[i].sleepMs) {
            svcSleepThread(reads[i].sleepMs * 1000000LL);
        }
        u64 t0 = svcGetSystemTick();
        fseek(f, (long)reads[i].offset, SEEK_SET);
        fread(buf, 1, reads[i].size, f);
        gPortFsBench[i] = (u32)((svcGetSystemTick() - t0) * 1000000 / SYSCLOCK_ARM11);
    }
    /* A file opened for the first time. */
    u64 t0 = svcGetSystemTick();
    FILE *g = fopen("romfs:/itemtool/itemdata/pl_item_data.narc", "rb");
    gPortFsBench[60] = (u32)((svcGetSystemTick() - t0) * 1000000 / SYSCLOCK_ARM11);
    if (g != NULL) {
        setvbuf(g, NULL, _IONBF, 0);
        t0 = svcGetSystemTick();
        fread(buf, 1, 19676, g);
        gPortFsBench[61] = (u32)((svcGetSystemTick() - t0) * 1000000 / SYSCLOCK_ARM11);
        fclose(g);
    }
    /* Big reads against 8KB reads of the same data, several times: does a big
     * read (which Azahar completes asynchronously) ever return other bytes? */
    {
        u8 *big = malloc(512 * 1024), *small = malloc(512 * 1024);
        u32 mismatches = 0, mismatchBytes = 0;
        for (int round = 0; big != NULL && small != NULL && round < 100; round++) {
            long at = (long)(round * 1000003L) % (15L << 20);
            fseek(f, at, SEEK_SET);
            fread(big, 1, 512 * 1024, f);
            for (u32 o = 0; o < 512 * 1024; o += 8192) {
                fseek(f, at + (long)o, SEEK_SET);
                fread(small + o, 1, 8192, f);
            }
            u32 diff = 0;
            for (u32 o = 0; o < 512 * 1024; o++) {
                diff += big[o] != small[o];
            }
            mismatches += diff != 0;
            mismatchBytes += diff;
        }
        gPortFsBench[54] = mismatches;
        gPortFsBench[55] = mismatchBytes;
        free(big);
        free(small);
    }
    gPortFsBench[63] = 0x48434e42; /* done */
    free(buf);
    fclose(f);
}

/* Every file of the extracted ROM and its size (tools/gen_romfs_list.py). */
#include "romfs_files.h"

/* Mounts romfs and checks that it finds every file of the ROM at its size:
 * in the emulator a few files were sometimes missing after the mount, and
 * the game crashed when it came to them. Mounts again while any is missing,
 * up to eight times. gPortRomfs, for tools: the mounts it took, and how many
 * files the last bad mount was missing. */
u32 gPortRomfs[2];

static Result MountRomfs(void)
{
    Result rc = romfsInit();
    for (u32 mounts = 1; R_SUCCEEDED(rc); mounts++) {
        gPortRomfs[0] = mounts;
        u32 missing = 0;
        for (u32 k = 0; k < sizeof(sRomfsFiles) / sizeof(sRomfsFiles[0]); k++) {
            struct stat st;
            missing += stat(sRomfsFiles[k].path, &st) != 0 || (u32)st.st_size != sRomfsFiles[k].size;
        }
        if (missing == 0 || mounts == 8) {
            break;
        }
        gPortRomfs[1] = missing;
        romfsExit();
        rc = romfsInit();
    }
    return rc;
}

int main(void)
{
#ifndef PORT_NO_SPEEDUP
    osSetSpeedupEnable(true);
#endif
    gfxInitDefault();

    Result rc = MountRomfs();
    if (R_FAILED(rc)) {
        consoleInit(GFX_TOP, NULL);
        printf("romfsInit failed: %08lx\n", rc);
        printf("Was the ROM extracted into romfs/ before building?\n\nPress START to exit.");
        while (aptMainLoop()) {
            hidScanInput();
            if (hidKeysDown() & KEY_START) {
                break;
            }
            gspWaitForVBlank();
        }
        gfxExit();
        return 0;
    }

    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();
    InitScreen(&sTop, GFX_TOP, 400);
    InitScreen(&sBottom, GFX_BOTTOM, 320);
    Ntr_SetScreenSizes(sTop.w, sTop.h, sBottom.w, sBottom.h);
    Ntr_SetOutputRGBA5551(TRUE);
    StartWorker();
    MeasureCpuClock();
    Gpu3D_Init();
    Gpu2D_Init();
    sTextBuf = C2D_TextBufNew(256);

    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/pokeplat", 0777);
    LoadTestScript();
    Ntr_SetBackupFile(sTestScript ? "sdmc:/pokeplat_dumps/test.sav" : "sdmc:/3ds/pokeplat/pokeplat.sav");
    if (!sTestScript) { /* test runs keep the defaults, to match the harness */
        Port_SetSettingsFile("sdmc:/3ds/pokeplat/settings.txt");
    }
    NtrFS_SetRoot("romfs:");
    if (sTestScript && InputScript_FsBench()) {
        RunFsBench();
    }
    bool isNew3DS = false;
    APT_CheckNew3DS(&isNew3DS);
    StartFiles(isNew3DS);
    StartSaveWriter(isNew3DS);
    StartSound(!sTestScript || InputScript_RealtimeAudio());

    NitroMain(); /* never returns */
    return 0;
}
