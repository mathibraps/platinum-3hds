/*
 * NitroSDK replacement layer.
 *
 * The decompiled game, NitroSystem and the SDK's own portable code are built
 * unchanged on top of this header. It provides:
 *
 *   - the SDK's constants, types, prototypes and inline helpers, extracted
 *     from the NitroSDK headers at build time (tools/gen_sdk_defs.py:
 *     ntr_sdk_defs.h, ntr_sdk_inlines.h);
 *   - the DS memory map, emulated (ntr_memmap.h): registers, VRAM, palette
 *     RAM and OAM are host memory that the renderer reads like the hardware;
 *   - declarations for the port's own platform code.
 *
 * Every SDK header path resolves here through generated forwarding headers.
 */
#ifndef NTR_SHIM_H
#define NTR_SHIM_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ types */

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef volatile u8 vu8;
typedef volatile u16 vu16;
typedef volatile u32 vu32;
typedef volatile u64 vu64;
typedef volatile s8 vs8;
typedef volatile s16 vs16;
typedef volatile s32 vs32;
typedef volatile s64 vs64;
typedef float f32;
typedef double f64;
typedef int BOOL;

typedef u8 REGType8;
typedef u16 REGType16;
typedef u32 REGType32;
typedef u64 REGType64;
typedef volatile u8 REGType8v;
typedef volatile u16 REGType16v;
typedef volatile u32 REGType32v;
typedef volatile u64 REGType64v;

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

#define ATTRIBUTE_ALIGN(n) __attribute__((aligned(n)))

/* SDK debug checks are compiled out, as in the SDK's final-ROM build. */
#define SDK_ASSERT(...) ((void)0)
#define SDK_ASSERTMSG(...) ((void)0)
#define SDK_NULL_ASSERT(...) ((void)0)
#define SDK_MINMAX_ASSERT(...) ((void)0)
#define SDK_ALIGN4_ASSERT(...) ((void)0)
#define SDK_ALIGN2_ASSERT(...) ((void)0)
#define SDK_ALIGN32_ASSERT(...) ((void)0)
#define SDK_TASSERTMSG(...) ((void)0)
#define SDK_WARNING(...) ((void)0)
#define SDK_WARNING_MSG(...) ((void)0)
/* Fatal SDK errors stop the game, as in the SDK's final-ROM build. */
void OS_Terminate(void);
#define SDK_FATAL_ERROR(...) OS_Terminate()
#define SDK_TFATAL_ERROR(...) OS_Terminate()
#define SDK_INTERNAL_ERROR(...) OS_Terminate()
#define SDK_COMPILER_ASSERT(expr) _Static_assert(expr, #expr)
#define SDK_INLINE static inline
#define SDK_WEAK_SYMBOL __attribute__((weak))
#define SDK_DECL_INLINE static inline
#define OS_Printf(...) ((void)0)
#define OS_TPrintf(...) ((void)0)
#define __alloca(size) __builtin_alloca(size)

/* NitroSystem marks header functions "inline" with Metrowerks' meaning
 * (internal linkage); C99 needs "static inline" for the same effect. */
#define NNS_INLINE_H_
#define NNS_INLINE static inline

/* --------------------------------------------------------- file system */

/* The port reads the ROM's file system from extracted files (ntr_sys.c), so
 * it uses its own file type instead of the SDK's. */
typedef enum {
    FS_SEEK_SET = 0,
    FS_SEEK_CUR = 1,
    FS_SEEK_END = 2
} FSSeekFileMode;

typedef struct FSArchive {
    u32 opaque[24];
} FSArchive;

typedef struct FSFile {
    void *handle; /* the port's open file (ntr_sys.c) */
    u32 size;
    u32 pos;
    void *block; /* the cache block last read from */
} FSFile;

/* ROM files are read through a cache of recently used blocks in memory (the
 * game reads small pieces of its archives all the time, each a request to
 * the SD card on the 3DS). Sets its size in bytes (default 16MB). */
void NtrFS_SetCacheSize(u32 bytes);
/* For a frontend that prefetches from another thread: locks around the
 * cache, set before the game starts. */
void NtrFS_SetLock(void (*lock)(void), void (*unlock)(void));
/* Reads ROM files (paths as the game opens them) into the cache ahead of the
 * game, each whole, in order, a piece at a time like the game's reads, until
 * `budget` bytes of the cache hold data; it never pushes out data the game
 * read. Runs on the prefetching thread, returns when done. */
void NtrFS_Prefetch(const char *const *paths, int count, u32 budget);
extern u32 gNtrFsPrefetched; /* bytes it read so far */
/* Reads from the host file system so far, and bytes read. */
extern u32 gNtrFsHostReads, gNtrFsHostBytes;

/* A file by number (FS_ConvertPathToFileID), for FS_OpenFileFast. */
typedef struct FSFileID {
    struct FSArchive *arc;
    u32 file_id;
} FSFileID;

BOOL FS_ConvertPathToFileID(FSFileID *id, const char *path);
BOOL FS_OpenFileFast(FSFile *file, FSFileID id);
static inline BOOL FS_CancelFile(FSFile *file) { (void)file; return TRUE; }

/* nitro/version.h */
#define SDK_VERSION_MAJOR 4
#define SDK_VERSION_MINOR 2
#define SDK_VERSION_RELSTEP 30001
#define SDK_VERSION_NUMBER(major, minor, relstep) (((major) << 24) | ((minor) << 16) | ((relstep) << 0))
#define SDK_CURRENT_VERSION_NUMBER SDK_VERSION_NUMBER(SDK_VERSION_MAJOR, SDK_VERSION_MINOR, SDK_VERSION_RELSTEP)

void FS_InitFile(FSFile *file);
BOOL FS_OpenFile(FSFile *file, const char *path);
BOOL FS_CloseFile(FSFile *file);
BOOL FS_SeekFile(FSFile *file, s32 offset, FSSeekFileMode origin);
s32 FS_ReadFile(FSFile *file, void *dest, s32 size);
u32 FS_GetLength(FSFile *file);
u32 FS_GetPosition(FSFile *file);

/* The file system needs no setup or table (FS_Init, FS_LoadTable). */
void FS_Init(u32 default_dma_no);
static inline u32 FS_GetTableSize(void) { return 0; }
static inline BOOL FS_LoadTable(void *p_mem, u32 size) { (void)p_mem; (void)size; return FALSE; }
static inline BOOL FS_IsAvailable(void) { return TRUE; }

/* Holds freed game memory back, filled with 0xdd, up to this many bytes
 * (game/port_heap.c; for finding reads of freed memory). */
void PortHeap_SetQuarantine(u32 bytes);

/* Directory holding the extracted ROM file system (set once at boot). */
void NtrFS_SetRoot(const char *root);

/* fread and fwrite in pieces of at most NTR_FILE_PIECE bytes. Azahar
 * finishes reads that small at once; bigger ones it finishes later, and the
 * port saw files missing after romfs read its tables that way, so all the
 * port's file reads and writes go in pieces. */
#define NTR_FILE_PIECE 0x2000
size_t Ntr_ReadPieces(void *dst, size_t len, FILE *f);
size_t Ntr_WritePieces(const void *src, size_t len, FILE *f);

/* Everything is linked statically, so overlays have no identity. */
typedef u32 FSOverlayID;
#define FS_EXTERN_OVERLAY(name) extern int PortOverlayUnused_##name
#define FS_OVERLAY_ID(name) ((FSOverlayID)0)

/* ------------------------------------------------------------ SDK proper */

#include "ntr_memmap.h"

/* Constants, types and prototypes generated from the NitroSDK headers. */
#include "ntr_sdk_defs.h"

/* ---------------------------------------------------------- platform */

/* Per-engine VRAM views, as mapped by the current bank configuration
 * (engine 0 = main/"A", engine 1 = sub/"B"). Registers, palettes and OAM
 * live in the emulated memory map. */
typedef struct NtrEngine {
    u32 io; /* I/O register block offset: 0x0000 main, 0x1000 sub */
    u8 *bgVram;
    u32 bgVramSize;
    u8 *objVram;
    u32 objVramSize;
    u8 *bgExtPltt; /* 4 slots x 8KB, NULL when unmapped */
    u8 *objExtPltt; /* 8KB, NULL when unmapped */
    u16 *bgPltt; /* palette RAM */
    u16 *objPltt;
    u8 *oam;
} NtrEngine;

extern NtrEngine gNtrEngines[2];

/* Texture image and palette memory as the 3D engine sees it (bank-mapped). */
extern u8 *gNtrTexVram; /* 512KB: texture slots 0-3 */
extern u8 *gNtrTexPltt; /* 96KB: texture palette slots 0-5 */
/* When texture memory was last written, per 16KB page: a stamp from
 * gNtrTexWrites (texture uploads and banks mapped as texture slots), so the
 * 3DS renderer re-checks a cached texture only after its pages changed. */
#define NTR_TEX_PAGE_SHIFT 14
extern u32 gNtrTexWrites;
extern u32 gNtrTexVramPage[0x80000 >> NTR_TEX_PAGE_SHIFT];
extern u32 gNtrTexPlttPage[0x18000 >> NTR_TEX_PAGE_SHIFT];
void Ntr_TexMemoryWritten(BOOL palette, u32 addr, u32 size);

/* Reads an emulated register by I/O offset (DS address - 0x04000000). */
u16 Ntr_ReadIoReg16(u32 offset);

/* Renders both 2D engines (with the 3D layer) to RGBA8888 256x192 images;
 * top/bottom follow the display swap (POWCNT bit 15). */
void Ntr_RenderScreens(u32 *top, u32 *bottom);

/* The same per engine (0 = main, 1 = sub). The two engines may be rendered at
 * the same time on different threads. */
void Ntr_RenderEngine(int engine, void *out, int stride); /* stride in pixels, 0 = the width */

/* Render 16-bit RGBA5551 pixels (u16) instead of RGBA8888 (u32). */
void Ntr_SetOutputRGBA5551(BOOL enable);
BOOL Ntr_MainEngineOnTop(void);

/* Output screens -------------------------------------------------------------
 *
 * Widescreen: each DS screen can be rendered larger than 256x192 (on the 3DS:
 * 400x240 top, 320x240 bottom). The DS picture keeps its pixels, centred; 3D
 * scenes drawn with the full-screen viewport show more of the world around
 * it (the geometry engine widens clip space), 2D layers stay in the centre.
 * The default is the DS size on both screens. */
#define NTR_MAX_SCREEN_W 400
#define NTR_MAX_SCREEN_H 240

void Ntr_SetScreenSizes(int topW, int topH, int bottomW, int bottomH);

/* Widescreen layers that scroll go on beside the DS picture (ntr_render.c),
 * which is followed frame by frame as the engines render. A front end that
 * does not render every frame calls this every frame instead. */
void Ntr_TrackFrame(void);

/* Output size of the screen a 2D engine (0 = main, 1 = sub) is shown on. */
void Ntr_EngineScreenSize(int engine, int *w, int *h);

/* The rendered 3D frame (BG0 of the main engine when enabled), at the main
 * engine's output size: RGB555 color with bit 15 set where there is no pixel
 * (alpha 0), the 5-bit alpha, and per row whether any pixel is translucent
 * (alpha 1-30). */
extern u16 gNtr3DColor[NTR_MAX_SCREEN_H][NTR_MAX_SCREEN_W];
extern u8 gNtr3DAlpha[NTR_MAX_SCREEN_H][NTR_MAX_SCREEN_W];
extern u8 gNtr3DRowTranslucent[NTR_MAX_SCREEN_H];

/* GPU composition ------------------------------------------------------------
 *
 * Instead of composing a 2D engine itself (Ntr_RenderEngine), the renderer can
 * decode each layer into an image and describe how a GPU combines them: a list
 * of passes, each drawing one source over some rectangles of the output with
 * an alpha test, a stencil test or write, and a blend. The stencil holds the
 * top layer of every pixel (one bit each: BG0-3, OBJ, backdrop), which is how
 * the DS blending rules find the layer below. Colors are RGBA8 0xRRGGBBAA.
 * With the OBJ window, the window's shape is an image too (objWin), and
 * passes can be limited to the pixels inside or outside it. */
#define NTR_LAYER_W NTR_MAX_SCREEN_W
#define NTR_LAYER_H NTR_MAX_SCREEN_H
#define NTR_COMPOSE_MAX_PASSES 256
#define NTR_COMPOSE_MAX_RECTS 1024

/* Alpha of OBJ image pixels: priority and semi-transparency. */
#define NTR_OBJ_ALPHA(prio, semi) (0x10 + (prio) * 0x40 + ((semi) ? 0x20 : 0))

enum { NTR_SRC_COLOR, NTR_SRC_BG0, NTR_SRC_BG1, NTR_SRC_BG2, NTR_SRC_BG3, NTR_SRC_OBJ, NTR_SRC_3D };
enum { NTR_TEST_ALWAYS, NTR_TEST_EQUAL, NTR_TEST_NOTEQUAL, NTR_TEST_LESS, NTR_TEST_GREATER };
enum {
    NTR_BLEND_NONE, /* the source replaces the pixel */
    NTR_BLEND_EVA_EVB, /* source * eva/16 + pixel * evb/16, saturated */
    NTR_BLEND_SRC_ALPHA, /* source * a + pixel * (1 - a) */
};

typedef struct NtrRect {
    s16 x0, y0, x1, y1; /* output pixels, x1 and y1 exclusive */
} NtrRect;

typedef struct NtrPass {
    u8 source; /* NTR_SRC_* */
    u8 alphaTest, alphaRef; /* source alpha <test> ref (8-bit) */
    /* (stencilRef & stencilMask) <test> (stencil & stencilMask); a pass that
     * writes the stencil replaces the stencilWrite bits with stencilRef. */
    u8 stencilTest, stencilRef, stencilMask, stencilWrite;
    u8 colorWrite;
    u8 blend, eva, evb;
    /* What the blend mixes with: the pixel (NTR_SRC_COLOR), or a layer's own
     * image, whose raw color the DS uses where the pixel is blended already. */
    u8 below;
    u32 color; /* NTR_SRC_COLOR */
    u8 objWindow; /* 0: everywhere, 1: only inside the OBJ window, 2: only outside it */
    u16 firstRect, numRects;
} NtrPass;

typedef struct NtrComposition {
    int outW, outH;
    int ox, oy; /* where the DS picture is */
    int ofs3D; /* the 3D image's texel x for output x is x + ofs3D */
    u8 changedBg; /* BG images decoded anew this frame (the others are as before) */
    s16 objRow0, objRow1; /* OBJ image rows written this frame (sprites, or cleared) */
    BOOL objWindow; /* passes use the OBJ window image (written whole this frame) */
    int numPasses, numRects;
    BOOL overflow;
    NtrPass passes[NTR_COMPOSE_MAX_PASSES];
    NtrRect rects[NTR_COMPOSE_MAX_RECTS];
} NtrComposition;

/* Where Ntr_ComposeEngine decodes the layers: BG images in RGBA5551 (alpha 0 =
 * transparent), the OBJ image in RGBA8 with NTR_OBJ_ALPHA (0 = none), each
 * covering the output (texel x, y = output pixel x, y; up to NTR_LAYER_W x
 * NTR_LAYER_H) with rows `stride` pixels apart: BG layers and sprites go on
 * beside the DS picture. The 3D image is the frontend's (the GPU 3D frame,
 * RGBA8 at the main engine's output size). The buffers must stay the same
 * from frame to frame: rows of the OBJ image are only cleared when they had
 * sprites before. */
typedef struct NtrLayerImages {
    u16 *bg[4];
    u32 *obj;
    int stride;
    u16 *objWin; /* the OBJ window's shape: RGBA5551, 0xffff inside, 0 outside */
} NtrLayerImages;

/* Decodes the layers of an engine (0 = main, 1 = sub) for the frame and
 * describes their composition. Returns FALSE when the frame uses something the
 * passes do not cover (display modes other than graphics, or color effects
 * that need the raw color of a blended pixel); then render it with
 * Ntr_RenderEngine. */
BOOL Ntr_ComposeEngine(int engine, const NtrLayerImages *images, NtrComposition *out);

/* The same in steps, to spread the decoding over threads: Ntr_ComposeBegin
 * (FALSE: use Ntr_RenderEngine), then Ntr_ComposeLines for each part of the
 * lines (parts in any threads, at most NTR_COMPOSE_MAX_PARTS), then
 * Ntr_ComposeEnd. */
#define NTR_COMPOSE_MAX_PARTS 2
BOOL Ntr_ComposeBegin(int engine, NtrComposition *out);
void Ntr_ComposeLines(int engine, const NtrLayerImages *images, int part, int numParts);
BOOL Ntr_ComposeEnd(int engine, const NtrLayerImages *images, NtrComposition *out);
extern const char *gNtrComposeFallback; /* why, for diagnostics */

/* Front end hooks, called from the game's thread ---------------------------- */

/* End of a frame (the game waits for VBlank): present the screens rendered by
 * Ntr_RenderScreens, pace to 60 Hz and pass the input on with Ntr_SetInput. */
void NtrHost_VBlank(void);

/* A monotonic clock for profiling, and its rate. */
u64 NtrHost_Ticks(void);
u64 NtrHost_TicksPerSecond(void);

/* Time spent in the 3D rasterizer (NtrHost_Ticks units), for profiling;
 * the time in the geometry engine's command blocks. */
extern u64 gNtrRasterTicks;
/* Work left out, for measuring what each part costs (test scripts' skip
 * command): NTR_SKIP_MIX mixes silence, NTR_SKIP_GEOMETRY drops geometry
 * command blocks, NTR_SKIP_2D leaves the screens' 2D layers out,
 * NTR_SKIP_PREFETCH reads no ROM files ahead of the game,
 * NTR_SKIP_SAVE_WRITER writes the save file through on each request. */
enum { NTR_SKIP_MIX = 1, NTR_SKIP_GEOMETRY = 2, NTR_SKIP_2D = 4, NTR_SKIP_PREFETCH = 8, NTR_SKIP_SAVE_WRITER = 16 };
extern u32 gNtrDebugSkip;
extern u64 gNtrGeTicks;
extern u64 gNtrFsTicks, gNtrLzTicks; /* file reads, LZ77 decompression */
extern u64 gNtrSoundWaitTicks; /* waiting for the sound driver */
/* Time in the 2D renderer's stages: BG lines, OBJ lines, composition. */
extern u64 gNtrRenderTicks[2][3];

/* The game asked for a reset or power off (parameter: OS reset parameter). */
void NtrHost_Reset(u32 parameter);

/* Current key state (PAD_* bits) and touch point (bottom screen pixels). */
void Ntr_SetInput(u16 keys, BOOL touching, u16 touchX, u16 touchY);
void Ntr_SetTouch(BOOL touching, u16 x, u16 y);

/* Save data (the 512KB flash backup) lives in this host file. */
void Ntr_SetBackupFile(const char *path);
/* Writes the save file from another thread (the frontend's): wake is called
 * after requests that changed save memory; the thread then calls
 * Ntr_BackupWriteChanges until it returns FALSE (nothing left), as must the
 * frontend before it quits. The locks guard the save memory. */
void Ntr_SetBackupWriter(void (*wake)(void), void (*lock)(void), void (*unlock)(void));
BOOL Ntr_BackupWriteChanges(void);

/* Tests: time that depends only on the frame count (RTC, ticks, entropy),
 * so runs repeat exactly on every platform. */
void Ntr_SetDeterministicTime(BOOL enable);
BOOL Ntr_DeterministicTime(void);
u32 Ntr_FramesEnded(void);

/* Runs the VBlank interrupt handler now, without ending the frame (for
 * code that relies on VBlank arriving during long frames). */
void Ntr_RaiseVBlankIrq(void);

/* Applies armed HBlank DMA transfers (once per frame). */
void Ntr_EndFrameDma(void);

/* Sound (ntr_snd.c, ntr_spu.c): the DS sound driver and hardware. The front
 * end starts the driver before the game, then asks for its output as it
 * plays it: NTR_SOUND_RATE stereo frames a second, interleaved 16-bit left
 * and right.
 * Threaded (threaded = TRUE): only the front end's audio thread calls
 * NtrSound_Render and NtrSound_Service; `wake`, called from the game's
 * thread, must make it call NtrSound_Service soon (the game sent the driver
 * a message), and `idle` pauses the game's thread briefly while the game
 * waits for the driver.
 * Otherwise the game's thread calls NtrSound_Render (once a frame), and the
 * driver takes the game's messages as they are sent; wake and idle are not
 * used. */
#define NTR_SOUND_RATE 32728.5
void NtrSound_Start(BOOL threaded, void (*wake)(void), void (*idle)(void));
void NtrSound_Render(s16 *out, int frames);
void NtrSound_Service(void);
/* Game side: delivers the driver's messages (each frame), and waits for it. */
void Ntr_SoundPoll(void);
void Ntr_SoundWait(void);
/* The ARM7's sound registers, and the hardware behind them (driver side). */
#define NTR_SND7_REGS_SIZE 0x520
extern u8 gNtrSnd7Regs[NTR_SND7_REGS_SIZE];
void Ntr_SpuReset(void);
void Ntr_SpuStartChannel(int ch);
void Ntr_SpuStartCapture(int mask);
void Ntr_SpuMix(s16 *out, int frames);

/* The game's entry point (main.c). */
void NitroMain(void);

/* The SDK implements this with the ARM clz instruction in inline assembly. */
static inline u32 MATH_CountLeadingZerosInline(u32 x) { return x ? (u32)__builtin_clz(x) : 32; }

/* SDK inline helpers (generated); they use the emulated registers above. */
#include "ntr_sdk_inlines.h"

#ifdef __cplusplus
}
#endif

#endif /* NTR_SHIM_H */
