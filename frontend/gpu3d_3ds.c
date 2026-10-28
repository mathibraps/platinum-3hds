/*
 * The DS 3D engine's pixel pipeline on the 3DS GPU.
 *
 * The port's geometry engine (ntr_g3.c) transforms, lights and clips the
 * game's polygons as the DS does; this file takes the finished polygon list
 * at SWAP_BUFFERS instead of the software rasterizer and draws it with
 * citro3d into a texture, which GPU composition (gpu2d_3ds.c) uses as BG0.
 * Frames composed on the CPU read it back into gNtr3DColor/gNtr3DAlpha.
 *
 * Mapping of the DS rules:
 *   - textures: every DS format is decoded to RGBA8 and cached; an entry is
 *     checked against a hash of its texel and palette bytes each frame, so
 *     any way the game changes VRAM is seen;
 *   - modulate, decal and untextured polygons map to the texture combiner;
 *     toon and highlight use the toon color per vertex (an approximation);
 *   - alpha test, translucent blending (color blended, alpha = max), opaque
 *     polygons before translucent ones (sorted by Y unless manual sort),
 *     depth write for translucent polygons only when asked, and the
 *     translucent polygon ID rule via the stencil buffer;
 *   - over a cleared pixel with alpha 0 a translucent polygon is not blended
 *     but keeps its own color and alpha: the stencil marks cleared pixels
 *     (0x80), opaque ones (0) and translucent ones (0x40 | polygon ID), and
 *     translucent polygons are drawn twice, unblended over cleared pixels,
 *     then blended over the rest;
 *   - Z buffering, and W buffering with the GPU's own W-buffer mode;
 * Not done: edge marking, fog, anti-aliasing, shadow and wireframe polygons
 * (the software rasterizer skips those too).
 *
 * The frame is drawn into a 512x256 texture, the image in its top rows (the
 * texture's first rows are its top, t = 1).
 *
 * Supersampling (the 3D EDGES option): the frame is drawn at twice the size
 * into a 1024x512 texture with linear filtering. Composition samples it at
 * the same texture coordinates, which fall on the corner between the four
 * texels of each output pixel, so the GPU averages them: smooth polygon edges
 * and textures, while the 2D layers keep their pixels. Texture pixel art that
 * does not line up with the screen's pixels (most of the overworld) comes out
 * softer, which is why the option is off by default. (Snapping vertices to
 * whole pixels, as the DS does, keeps that art sharp but opens seams between
 * map blocks, whose shared edges differ slightly in floating point.)
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>
#include <citro3d.h>

#include "gpu2d_3ds.h"
#include "gpu3d_3ds.h"
#include "ntr_g3_internal.h"

extern const unsigned char gGpu3DShader[];
extern const unsigned gGpu3DShader_size;

/* The output is the main engine's screen size (widescreen on the 3DS). */
static int sOutW = 256, sOutH = 192;
/* The target's size at 1x; at scale 2 it is twice that, with the same
 * layout, so vertex positions (relative to the target) do not change. */
#define TARGET_W 512
#define TARGET_H 256
static C3D_Tex sTargetTex;
static int sScale, sWantScale = 1;

#define STENCIL_CLEARED 0x80
#define STENCIL_TRANSLUCENT 0x40

#define MAX_VERTS (NTR_G3_MAX_POLYS * 12)
#define MAX_BATCHES NTR_G3_MAX_POLYS
#define TEX_CACHE_SIZE 512
#define TEX_KEEP_FRAMES 120

/* Read back at 1x: a supersampled frame is averaged down by the transfer. */
#define TRANSFER_FLAGS(scaling)                                                                     \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0)                  \
        | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) \
        | GX_TRANSFER_SCALING(scaling))

typedef struct GpuVertex {
    float pos[4];
    float color[4];
    float tc[2];
} GpuVertex;

enum { TEV_COLOR, TEV_MODULATE, TEV_DECAL };

typedef struct TexEntry {
    bool used;
    u32 image, pltt; /* key: TEXIMAGE_PARAM without the wrap bits, PLTT_BASE */
    u32 hash;
    u32 lastFrame;
    u32 checked; /* gNtrTexWrites when the hash was last checked */
    C3D_Tex tex;
    /* Big textures that changed once (an animated sprite sheet, say) keep a
     * copy of the bytes they are decoded from, so a change decodes only the
     * rows of tiles that differ (UpdateFromShadow). */
    u8 *shadow; /* texel bytes, then palette bytes */
    u32 shadowTex, shadowPal;
} TexEntry;

/* Polygons with the same GPU state are drawn together. */
typedef struct Batch {
    int first, count;
    TexEntry *tex;
    u8 tev;
    u8 wrapS, wrapT;
    bool blend;
    bool translucent;
    bool depthEqual;
    bool depthWrite;
    u8 polyID;
} Batch;

static shaderProgram_s sProgram;
static DVLB_s *sShader;
static C3D_RenderTarget *sTarget;
static bool sDrawn; /* the target holds a frame */
static u32 *sReadback;
static GpuVertex *sVerts;
static int sNumVerts;
static Batch sBatches[MAX_BATCHES];
static int sNumBatches;
static bool sPending;
static u32 sClearColor, sClearDepth;
static bool sWBuffer;
static int sAlphaRef;
static TexEntry sTexCache[TEX_CACHE_SIZE];
static u32 sFrame;

/* Textures ------------------------------------------------------------------ */

u32 gGpu3DTexDecodes, gGpu3DTexTexels, gGpu3DTexHashes; /* textures (or rows of tiles) decoded, texels, hash checks so far */

static u32 HashBytes(u32 h, const u8 *base, u32 size, u32 addr, u32 len)
{
    if (addr >= size) {
        return h;
    }
    if (len > size - addr) {
        len = size - addr;
    }
    const u8 *p = base + addr;
    u32 i = 0;
    for (; i + 4 <= len; i += 4) {
        u32 w;
        memcpy(&w, p + i, 4);
        h = (h ^ w) * 0x01000193u;
    }
    for (; i < len; i++) {
        h = (h ^ p[i]) * 0x01000193u;
    }
    return h;
}

/* The latest write to the texture memory pages in [addr, addr + len). */
static u32 PagesStamp(const u32 *pages, u32 count, u32 addr, u32 len)
{
    u32 stamp = 0;
    for (u32 p = addr >> NTR_TEX_PAGE_SHIFT; p <= (addr + (len ? len : 1) - 1) >> NTR_TEX_PAGE_SHIFT && p < count; p++) {
        stamp = pages[p] > stamp ? pages[p] : stamp;
    }
    return stamp;
}

/* The latest write to the memory a texture reads (as TextureHash; for 4x4
 * compressed textures, all palette memory above the base). */
static u32 TextureStamp(u32 image, u32 pltt)
{
    int fmt = (image >> 26) & 7;
    u32 n = (8u << ((image >> 20) & 7)) * (8u << ((image >> 23) & 7));
    u32 addr = (image & 0xffff) << 3;
    u32 palBase = pltt << 4, palLen = 0, texLen = 0;
    const u32 nv = 0x80000 >> NTR_TEX_PAGE_SHIFT, np = 0x18000 >> NTR_TEX_PAGE_SHIFT;
    u32 stamp = 0;
    switch (fmt) {
    case GX_TEXFMT_A3I5:
        texLen = n, palLen = 32 * 2;
        break;
    case GX_TEXFMT_A5I3:
        texLen = n, palLen = 8 * 2;
        break;
    case GX_TEXFMT_PLTT4:
        texLen = n / 4, palBase = pltt << 3, palLen = 4 * 2;
        break;
    case GX_TEXFMT_PLTT16:
        texLen = n / 2, palLen = 16 * 2;
        break;
    case GX_TEXFMT_PLTT256:
        texLen = n, palLen = 256 * 2;
        break;
    case GX_TEXFMT_DIRECT:
        texLen = n * 2;
        break;
    case GX_TEXFMT_COMP4x4: {
        texLen = n / 4;
        u32 idxAddr = 0x20000 + ((addr & 0x1ffff) >> 1) + ((addr & 0x40000) >> 2);
        stamp = PagesStamp(gNtrTexVramPage, nv, idxAddr, n / 8);
        palLen = palBase < 0x18000 ? 0x18000 - palBase : 0;
        break;
    }
    default:
        break;
    }
    u32 t = PagesStamp(gNtrTexVramPage, nv, addr, texLen);
    stamp = t > stamp ? t : stamp;
    if (palLen) {
        u32 p = PagesStamp(gNtrTexPlttPage, np, palBase, palLen);
        stamp = p > stamp ? p : stamp;
    }
    return stamp;
}

/* A hash of the VRAM bytes a texture reads (texels, 4x4 block data, palette). */
static u32 TextureHash(u32 image, u32 pltt)
{
    int fmt = (image >> 26) & 7;
    u32 w = 8u << ((image >> 20) & 7), h = 8u << ((image >> 23) & 7);
    u32 n = w * h;
    u32 addr = (image & 0xffff) << 3;
    u32 palBase = pltt << 4, palLen = 0, texLen = 0;
    u32 hash = 0x811c9dc5u ^ image ^ (pltt << 16);

    switch (fmt) {
    case GX_TEXFMT_A3I5:
        texLen = n, palLen = 32 * 2;
        break;
    case GX_TEXFMT_A5I3:
        texLen = n, palLen = 8 * 2;
        break;
    case GX_TEXFMT_PLTT4:
        texLen = n / 4, palBase = pltt << 3, palLen = 4 * 2;
        break;
    case GX_TEXFMT_PLTT16:
        texLen = n / 2, palLen = 16 * 2;
        break;
    case GX_TEXFMT_PLTT256:
        texLen = n, palLen = 256 * 2;
        break;
    case GX_TEXFMT_DIRECT:
        texLen = n * 2;
        break;
    case GX_TEXFMT_COMP4x4: {
        texLen = n / 4;
        u32 idxAddr = 0x20000 + ((addr & 0x1ffff) >> 1) + ((addr & 0x40000) >> 2);
        hash = HashBytes(hash, gNtrTexVram, 0x80000, idxAddr, n / 8);
        /* Blocks pick palettes anywhere above the base; hash what they can reach. */
        u32 maxPal = 0;
        for (u32 i = 0; i < n / 16; i++) {
            u32 a = idxAddr + i * 2;
            if (a + 1 < 0x80000) {
                u32 info = gNtrTexVram[a] | gNtrTexVram[a + 1] << 8;
                u32 ofs = ((info & 0x3fff) << 2) + 8;
                if (ofs > maxPal) {
                    maxPal = ofs;
                }
            }
        }
        palLen = maxPal;
        break;
    }
    default:
        break;
    }
    hash = HashBytes(hash, gNtrTexVram, 0x80000, addr, texLen);
    return HashBytes(hash, gNtrTexPltt, 0x18000, palBase, palLen);
}

static void DecodeTexture(TexEntry *e, u32 image, u32 pltt)
{
    gGpu3DTexDecodes++;
    gGpu3DTexTexels += (8u << ((image >> 20) & 7)) * (8u << ((image >> 23) & 7));
    NtrTex_DecodeTiled(image, pltt, (u32 *)e->tex.data);
    C3D_TexFlush(&e->tex);
}

/* Where a texture's texels and palette are, for formats whose texel rows
 * follow one another (not 4x4 compressed); FALSE if it has no such layout or
 * wraps around the end of texture memory. */
static bool RowLayout(u32 image, u32 pltt, u32 *texAddr, u32 *texLen, u32 *palAddr, u32 *palLen)
{
    int fmt = (image >> 26) & 7;
    u32 n = (8u << ((image >> 20) & 7)) * (8u << ((image >> 23) & 7));
    *texAddr = (image & 0xffff) << 3;
    *palAddr = pltt << 4;
    switch (fmt) {
    case GX_TEXFMT_A3I5:
        *texLen = n, *palLen = 32 * 2;
        break;
    case GX_TEXFMT_A5I3:
        *texLen = n, *palLen = 8 * 2;
        break;
    case GX_TEXFMT_PLTT4:
        *texLen = n / 4, *palAddr = pltt << 3, *palLen = 4 * 2;
        break;
    case GX_TEXFMT_PLTT16:
        *texLen = n / 2, *palLen = 16 * 2;
        break;
    case GX_TEXFMT_PLTT256:
        *texLen = n, *palLen = 256 * 2;
        break;
    case GX_TEXFMT_DIRECT:
        *texLen = n * 2, *palLen = 0;
        break;
    default:
        return false;
    }
    return *texAddr + *texLen <= 0x80000 && *palAddr + *palLen <= 0x18000;
}

#define SHADOW_MIN_TEXELS (128 * 128)

/* A change to a texture with a copy of its bytes: the rows of tiles whose
 * bytes differ are decoded again (all of it if the palette changed). */
static void UpdateFromShadow(TexEntry *e, u32 image, u32 pltt)
{
    u32 texAddr, texLen, palAddr, palLen;
    RowLayout(image, pltt, &texAddr, &texLen, &palAddr, &palLen);
    const u8 *tex = gNtrTexVram + texAddr, *pal = gNtrTexPltt + palAddr;
    if (palLen != 0 && memcmp(e->shadow + texLen, pal, palLen) != 0) {
        memcpy(e->shadow, tex, texLen);
        memcpy(e->shadow + texLen, pal, palLen);
        DecodeTexture(e, image, pltt);
        return;
    }
    u32 h = 8u << ((image >> 23) & 7);
    u32 band = texLen / h * 8; /* bytes of eight rows */
    bool any = false;
    for (u32 y = 0; y < h; y += 8) {
        u32 at = y / 8 * band;
        if (memcmp(e->shadow + at, tex + at, band) != 0) {
            memcpy(e->shadow + at, tex + at, band);
            gGpu3DTexDecodes++;
            gGpu3DTexTexels += (8u << ((image >> 20) & 7)) * 8;
            NtrTex_DecodeTiledRows(image, pltt, (u32 *)e->tex.data, y, y + 8);
            any = true;
        }
    }
    if (any) {
        C3D_TexFlush(&e->tex);
    }
}

/* Gives a big texture that just changed a copy of its bytes, for next time. */
static void MakeShadow(TexEntry *e, u32 image, u32 pltt)
{
    u32 texAddr, texLen, palAddr, palLen;
    u32 n = (8u << ((image >> 20) & 7)) * (8u << ((image >> 23) & 7));
    if (n < SHADOW_MIN_TEXELS || !RowLayout(image, pltt, &texAddr, &texLen, &palAddr, &palLen)) {
        return;
    }
    e->shadow = malloc(texLen + palLen);
    if (e->shadow != NULL) {
        memcpy(e->shadow, gNtrTexVram + texAddr, texLen);
        memcpy(e->shadow + texLen, gNtrTexPltt + palAddr, palLen);
    }
}

static TexEntry *GetTexture(u32 image, u32 pltt)
{
    u32 key = image & ~0x000f0000u; /* the wrap bits are sampler state */
    u32 slot = (key * 2654435761u ^ pltt * 40503u) % TEX_CACHE_SIZE;
    TexEntry *free = NULL;
    u32 hash;

    for (u32 i = 0; i < TEX_CACHE_SIZE; i++) {
        TexEntry *e = &sTexCache[(slot + i) % TEX_CACHE_SIZE];
        if (e->used && e->image == key && e->pltt == pltt) {
            /* Checked against VRAM at most once per frame (VRAM does not
             * change while a frame's polygons are turned into draws), and
             * only when memory it reads was written since the last check. */
            if (e->lastFrame == sFrame) {
                return e;
            }
            e->lastFrame = sFrame;
            if (TextureStamp(key, pltt) <= e->checked) {
                return e;
            }
            e->checked = gNtrTexWrites;
            if (e->shadow != NULL) {
                UpdateFromShadow(e, key, pltt);
                return e;
            }
            gGpu3DTexHashes++;
            hash = TextureHash(key, pltt);
            if (e->hash != hash) {
                DecodeTexture(e, key, pltt);
                MakeShadow(e, key, pltt);
                e->hash = hash;
            }
            return e;
        }
        if (!e->used && free == NULL) {
            free = e;
        }
    }
    if (free == NULL) {
        return NULL;
    }
    hash = TextureHash(key, pltt);
    u32 w = 8u << ((key >> 20) & 7), h = 8u << ((key >> 23) & 7);
    if (!C3D_TexInit(&free->tex, (u16)w, (u16)h, GPU_RGBA8)) {
        return NULL;
    }
    C3D_TexSetFilter(&free->tex, GPU_NEAREST, GPU_NEAREST);
    free->used = true;
    free->shadow = NULL;
    free->image = key;
    free->pltt = pltt;
    free->hash = hash;
    free->lastFrame = sFrame;
    free->checked = gNtrTexWrites;
    DecodeTexture(free, key, pltt);
    return free;
}

static void EvictTextures(void)
{
    for (int i = 0; i < TEX_CACHE_SIZE; i++) {
        TexEntry *e = &sTexCache[i];
        if (e->used && sFrame - e->lastFrame > TEX_KEEP_FRAMES) {
            C3D_TexDelete(&e->tex);
            free(e->shadow);
            e->shadow = NULL;
            e->used = false;
        }
    }
}

/* Polygons ------------------------------------------------------------------ */

static bool IsTranslucent(const NtrG3Polygon *p)
{
    int alpha = (p->polyAttr >> 16) & 31;
    int fmt = (p->texImage >> 26) & 7;
    bool tex = (NTR_IO16(0x060) & 1) && fmt != GX_TEXFMT_NONE;
    return (alpha > 0 && alpha < 31) || (tex && (fmt == GX_TEXFMT_A3I5 || fmt == GX_TEXFMT_A5I3));
}

static float PolyTopY(const NtrG3PolyList *list, const NtrG3Polygon *p)
{
    float best = 1e9f;
    for (int i = 0; i < p->numVerts; i++) {
        const NtrG3Vertex *v = &list->verts[p->firstVert + i];
        float y = -(v->c[1] / (v->w > 1e-6f ? v->w : 1e-6f));
        if (y < best) {
            best = y;
        }
    }
    return best;
}

/* Translucent polygons sort by their top (worked out once each), then by
 * their order in the list. */
typedef struct SortKey {
    float top;
    int index;
} SortKey;

static int CompareTopY(const void *a, const void *b)
{
    const SortKey *ka = a, *kb = b;
    if (ka->top != kb->top) {
        return ka->top < kb->top ? -1 : 1;
    }
    return ka->index - kb->index;
}

static GPU_TEXTURE_WRAP_PARAM Wrap(bool repeat, bool flip)
{
    return !repeat ? GPU_CLAMP_TO_EDGE : (flip ? GPU_MIRRORED_REPEAT : GPU_REPEAT);
}

static bool SameState(const Batch *a, const Batch *b)
{
#ifdef GPU3D_DEBUG_REVERSE
    return false; /* one draw per polygon, so the reversed order shows */
#endif
    return a->tex == b->tex && a->tev == b->tev && a->wrapS == b->wrapS && a->wrapT == b->wrapT && a->blend == b->blend
        && a->translucent == b->translucent && a->depthEqual == b->depthEqual && a->depthWrite == b->depthWrite
        && (!a->translucent || a->polyID == b->polyID);
}

static void AddPolygon(const NtrG3PolyList *list, const NtrG3Polygon *p, bool wBuffer)
{
    u32 attr = p->polyAttr;
    int mode = (attr >> 4) & 3;
    int alpha = (attr >> 16) & 31;
    if (mode == GX_POLYGONMODE_SHADOW || alpha == 0 || p->numVerts < 3) {
        return;
    }
    int triVerts = (p->numVerts - 2) * 3;
    if (sNumVerts + triVerts > MAX_VERTS || sNumBatches >= MAX_BATCHES) {
        return;
    }

    u16 disp3d = NTR_IO16(0x060);
    int fmt = (p->texImage >> 26) & 7;
    bool textured = (disp3d & 1) && fmt != GX_TEXFMT_NONE;

    Batch b = { 0 };
    b.tex = textured ? GetTexture(p->texImage, p->texPltt) : NULL;
    b.tev = b.tex == NULL ? TEV_COLOR : (mode == GX_POLYGONMODE_DECAL ? TEV_DECAL : TEV_MODULATE);
    b.wrapS = (u8)Wrap((p->texImage >> 16) & 1, (p->texImage >> 18) & 1);
    b.wrapT = (u8)Wrap((p->texImage >> 17) & 1, (p->texImage >> 19) & 1);
    b.translucent = IsTranslucent(p);
    b.blend = b.translucent && (disp3d & 8);
    b.depthEqual = (attr & REG_G3_POLYGON_ATTR_DT_MASK) != 0;
    b.depthWrite = !b.translucent || (attr & REG_G3_POLYGON_ATTR_XL_MASK);
#ifdef GPU3D_DEBUG_NO_TRANSLUCENT_RULES
    b.depthWrite = true;
#endif
    b.polyID = (u8)((attr >> 24) & 63);

    int sizeS = 8 << ((p->texImage >> 20) & 7), sizeT = 8 << ((p->texImage >> 23) & 7);
    float invS = 1.0f / (16.0f * sizeS), invT = 1.0f / (16.0f * sizeT);
    const u16 *toon = (const u16 *)&gNtrIoRegs[0x380];
    bool toonMode = mode == GX_POLYGONMODE_TOON && !(disp3d & 2);

    /* Where a vertex lands (NtrG3_OutputPos: x / (w * wide), through the
     * viewport, into the target's top rows), times w: linear in clip space,
     * so no vertex needs a division. */
    int vx1 = p->viewport & 0xff, vy1 = (p->viewport >> 8) & 0xff;
    int vx2 = (p->viewport >> 16) & 0xff, vy2 = (p->viewport >> 24) & 0xff;
    float vw = (float)(vx2 - vx1 + 1), vh = (float)(vy2 - vy1 + 1);
    float ax = vw * 0.5f / p->wideX * (2.0f / TARGET_W);
    float bx = (vw * 0.5f + vx1 + (sOutW - 256) * 0.5f) * (2.0f / TARGET_W) - 1.0f;
    float ay = vh * 0.5f / p->wideY * (2.0f / TARGET_H);
    float by = (vh * 0.5f + vy1 + (sOutH - 192) * 0.5f + (TARGET_H - sOutH)) * (2.0f / TARGET_H) - 1.0f;

    GpuVertex fan[NTR_G3_MAX_POLY_VERTS];
    for (int i = 0; i < p->numVerts; i++) {
        const NtrG3Vertex *v = &list->verts[p->firstVert + i];
        float w = v->w > 1e-6f ? v->w : 1e-6f;
        GpuVertex *o = &fan[i];
        o->pos[0] = v->c[0] * ax + w * bx;
        o->pos[1] = v->c[1] * ay + w * by;
        /* Nearer pixels get the larger depth value. Z buffer: depth = -z/w,
         * the DS's 24-bit depth of z/w (times w here, clamped to 0..w);
         * W buffer: depth = -z. */
#ifndef GPU3D_WMODE
#define GPU3D_WMODE 1
#endif
        float d = 0;
        if (wBuffer) {
            /* The GPU's W-buffer mode stores -z, interpolated perspective-
             * correctly like the DS's W values, so z carries 1 - depth. */
            d = w * (4096.0f / 16777215.0f);
            d = d < 0 ? 0 : (d > 1 ? 1 : d);
        }
        if (!wBuffer) {
            float dw = (v->c[2] * 16384.0f + 16383.0f * w) * (512.0f / 16777215.0f);
            dw = dw < 0 ? 0 : (dw > w ? w : dw);
            o->pos[2] = dw - w;
        } else if (GPU3D_WMODE == 1) {
            o->pos[2] = -(1.0f - d);
        } else if (GPU3D_WMODE == 2) {
            o->pos[2] = -(1.0f - d) * w;
        } else {
            o->pos[2] = -(1.0f / 64); /* depth = 1/(64 w): exact order, linear in screen space */
        }
        o->pos[3] = w;
        float r = v->color[0], g = v->color[1], bl = v->color[2];
        if (toonMode) {
            int idx = (int)(r + 0.5f) & 31;
            u16 t = toon[idx];
            r = t & 31, g = (t >> 5) & 31, bl = (t >> 10) & 31;
        }
        o->color[0] = r * (1.0f / 31);
        o->color[1] = g * (1.0f / 31);
        o->color[2] = bl * (1.0f / 31);
        o->color[3] = alpha * (1.0f / 31);
        o->tc[0] = v->s * invS;
        o->tc[1] = 1.0f - v->t * invT; /* PICA textures run bottom-up */
    }

    b.first = sNumVerts;
    for (int i = 1; i + 1 < p->numVerts; i++) {
        sVerts[sNumVerts++] = fan[0];
        sVerts[sNumVerts++] = fan[i];
        sVerts[sNumVerts++] = fan[i + 1];
    }
    b.count = triVerts;

    if (sNumBatches > 0) {
        Batch *last = &sBatches[sNumBatches - 1];
        if (SameState(last, &b) && last->first + last->count == b.first) {
            last->count += b.count;
            return;
        }
    }
    sBatches[sNumBatches++] = b;
}

/* Called at SWAP_BUFFERS (on the game's thread) instead of the software
 * rasterizer: turns the polygon list into GPU draws for the next frame. */
#ifdef GPU3D_DEBUG_DUMP_FRAME
#include <stdio.h>
static int sDumpCountdown = -1;

static void DumpImage(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (f != NULL) {
        u16 size[2] = { (u16)sOutW, (u16)sOutH };
        fwrite(size, sizeof(size), 1, f);
        for (int y = 0; y < sOutH; y++) {
            fwrite(gNtr3DColor[y], 2, (size_t)sOutW, f);
        }
        for (int y = 0; y < sOutH; y++) {
            fwrite(gNtr3DAlpha[y], 1, (size_t)sOutW, f);
        }
        fclose(f);
    }
}
#endif

static void Submit(const NtrG3PolyList *list, u32 swapParam)
{
#ifdef GPU3D_DEBUG_DUMP_FRAME
    /* Debug: also render this frame in software and save both images. */
    static bool dumped;
    if (!dumped && (int)Ntr_FramesEnded() + 1 >= GPU3D_DEBUG_DUMP_FRAME) {
        dumped = true;
        gNtrRender3DHook = NULL;
        NtrRaster_Render(list, swapParam);
        gNtrRender3DHook = Submit;
        NtrG3_OutputSize(&sOutW, &sOutH);
        DumpImage("sdmc:/gpu3d_sw.bin");
        sDumpCountdown = 1;
    }
#endif
    bool manualSort = swapParam & 1;
    bool wBuffer = (swapParam >> 1) & 1;
    sWBuffer = wBuffer;
    NtrG3_OutputSize(&sOutW, &sOutH);
    sFrame++;
    sNumVerts = 0;
    sNumBatches = 0;

    static int opaque[NTR_G3_MAX_POLYS], trans[NTR_G3_MAX_POLYS];
    int numOpaque = 0, numTrans = 0;
    for (int i = 0; i < list->count; i++) {
        if (IsTranslucent(&list->polys[i])) {
            trans[numTrans++] = i;
        } else {
            opaque[numOpaque++] = i;
        }
    }
    if (!manualSort) {
        static SortKey keys[NTR_G3_MAX_POLYS];
        for (int i = 0; i < numTrans; i++) {
            keys[i].top = PolyTopY(list, &list->polys[trans[i]]);
            keys[i].index = trans[i];
        }
        qsort(keys, numTrans, sizeof(SortKey), CompareTopY);
        for (int i = 0; i < numTrans; i++) {
            trans[i] = keys[i].index;
        }
    }
    for (int i = 0; i < numOpaque; i++) {
        AddPolygon(list, &list->polys[opaque[i]], wBuffer);
    }
    for (int i = 0; i < numTrans; i++) {
        AddPolygon(list, &list->polys[trans[i]], wBuffer);
    }

    u32 clear = NTR_IO32(0x350);
    u32 cr = clear & 31, cg = (clear >> 5) & 31, cb = (clear >> 10) & 31, ca = (clear >> 16) & 31;
    sClearColor = ((cr << 3 | cr >> 2) << 24) | ((cg << 3 | cg >> 2) << 16) | ((cb << 3 | cb >> 2) << 8) | (ca * 255 / 31);
    /* Depth in the low 24 bits, stencil above: cleared pixels with alpha 0
     * are marked for the translucent polygon rule. */
    sClearDepth = 0xffffff - ((NTR_IO16(0x354) & 0x7fff) * 0x200 + 0x1ff);
    if (ca == 0) {
        sClearDepth |= (u32)STENCIL_CLEARED << 24;
    }
    sAlphaRef = (NTR_IO16(0x060) & 4) ? (NTR_IO16(0x340) & 31) * 255 / 31 : 0;

    EvictTextures();
    sPending = true;
}

/* Drawing ------------------------------------------------------------------- */

/* Translucent polygons are drawn in two passes: over cleared pixels (alpha
 * 0) without blending, then over the others. */
enum { PASS_OPAQUE, PASS_OVER_CLEARED, PASS_OVER_DRAWN };

static void SetupState(const Batch *b, int pass)
{
    C3D_TexEnv *env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    switch (b->tev) {
    case TEV_COLOR:
        C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
        C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
        break;
    case TEV_MODULATE:
        C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
        C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
        break;
    case TEV_DECAL:
        /* RGB: texture over vertex color by texture alpha; alpha: the polygon's. */
        C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_TEXTURE0);
        C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA);
        C3D_TexEnvFunc(env, C3D_RGB, GPU_INTERPOLATE);
        C3D_TexEnvSrc(env, C3D_Alpha, GPU_PRIMARY_COLOR, 0, 0);
        C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
        break;
    }
    for (int i = 1; i < 6; i++) {
        C3D_TexEnvInit(C3D_GetTexEnv(i));
    }
    if (b->tex != NULL) {
        C3D_TexSetWrap(&b->tex->tex, (GPU_TEXTURE_WRAP_PARAM)b->wrapS, (GPU_TEXTURE_WRAP_PARAM)b->wrapT);
        C3D_TexBind(0, &b->tex->tex);
    }

    if (b->blend && pass == PASS_OVER_DRAWN) {
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_MAX, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_ONE, GPU_ONE);
    } else {
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    }
    C3D_AlphaTest(true, GPU_GREATER, sAlphaRef);
#ifdef GPU3D_DEBUG_NO_DEPTH
    C3D_DepthTest(true, GPU_ALWAYS, GPU_WRITE_ALL);
#else
    C3D_DepthTest(true, b->depthEqual ? GPU_GEQUAL : GPU_GREATER, b->depthWrite ? GPU_WRITE_ALL : GPU_WRITE_COLOR);
#endif
#ifdef GPU3D_DEBUG_NO_TRANSLUCENT_RULES
    if (false) {
#else
    if (b->translucent) {
#endif
        /* The DS draws a translucent polygon ID only once per pixel. The
         * reference also serves the cleared-pixel test (bit 7 of it is 0). */
        int ref = STENCIL_TRANSLUCENT | b->polyID;
        if (pass == PASS_OVER_CLEARED) {
            C3D_StencilTest(true, GPU_NOTEQUAL, ref, STENCIL_CLEARED, 0xff);
        } else {
            C3D_StencilTest(true, GPU_NOTEQUAL, ref, 0xff, 0xff);
        }
    } else {
        C3D_StencilTest(true, GPU_ALWAYS, 0, 0xff, 0xff);
    }
    C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_REPLACE);
}

void Gpu3D_ReadBack(void)
{
    if (!sDrawn) {
        return;
    }
    /* Inside a frame citro3d only queues GPU work (it runs from
     * C3D_FrameEnd), so end the frame here, before any screen is drawn (so
     * nothing is shown), read back outside it, and start the next. */
    C3D_FrameEnd(0);
    C3D_SyncDisplayTransfer((u32 *)sTarget->frameBuf.colorBuf, GX_BUFFER_DIM(TARGET_W * sScale, TARGET_H * sScale), sReadback,
        GX_BUFFER_DIM(TARGET_W, TARGET_H), TRANSFER_FLAGS(sScale > 1 ? GX_TRANSFER_SCALE_XY : GX_TRANSFER_SCALE_NO));
    GSPGPU_InvalidateDataCache(sReadback, TARGET_W * TARGET_H * 4);
    C3D_FrameBegin(0);

    for (int y = 0; y < sOutH; y++) {
        const u32 *src = sReadback + y * TARGET_W;
        u16 *color = gNtr3DColor[y];
        u8 *alpha = gNtr3DAlpha[y];
        u8 translucent = 0;
        for (int x = 0; x < sOutW; x++) {
            u32 p = src[x];
            u32 a = p & 0xff;
            u32 r = p >> 24, g = (p >> 16) & 0xff, b = (p >> 8) & 0xff;
            if (a < 8) { /* 5-bit alpha 0: no pixel */
                alpha[x] = 0;
                color[x] = 0x8000;
                continue;
            }
            translucent |= a < 248;
            color[x] = (u16)((r >> 3) | (g >> 3) << 5 | (b >> 3) << 10);
            alpha[x] = (u8)(a >> 3);
        }
        gNtr3DRowTranslucent[y] = translucent;
    }
}

/* (Re)creates the target at a scale; the GPU is done with the old one (the
 * citro3d frame began after the last one finished). */
static bool CreateTarget(int scale)
{
    if (sTarget != NULL) {
        C3D_RenderTargetDelete(sTarget);
        C3D_TexDelete(&sTargetTex);
        sTarget = NULL;
    }
    sScale = 0;
    sDrawn = false;
    if (!C3D_TexInitVRAM(&sTargetTex, TARGET_W * scale, TARGET_H * scale, GPU_RGBA8)) {
        return false;
    }
    GPU_TEXTURE_FILTER_PARAM filter = scale > 1 ? GPU_LINEAR : GPU_NEAREST;
    C3D_TexSetFilter(&sTargetTex, filter, filter);
    C3D_TexSetWrap(&sTargetTex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    sTarget = C3D_RenderTargetCreateFromTex(&sTargetTex, GPU_TEXFACE_2D, 0, GPU_RB_DEPTH24_STENCIL8);
    if (sTarget == NULL) {
        C3D_TexDelete(&sTargetTex);
        return false;
    }
    sScale = scale;
    return true;
}

void Gpu3D_SetSupersample(bool on)
{
    sWantScale = on ? 2 : 1;
}

bool Gpu3D_Render(void)
{
    if (!sPending) {
        return false;
    }
    sPending = false;
    if (sScale != sWantScale && !CreateTarget(sWantScale)) {
        /* Not enough VRAM for the large target: stay at 1x. */
        sWantScale = 1;
        CreateTarget(1);
    }

    /* Supersampled frames clear transparent pixels to black too, so edges
     * averaged with them darken rather than take on the clear color. */
    u32 clearColor = sScale > 1 && (sClearColor & 0xff) == 0 ? 0 : sClearColor;
    C3D_RenderTargetClear(sTarget, C3D_CLEAR_ALL, clearColor, sClearDepth);
    C3D_FrameDrawOn(sTarget);
    C3D_BindProgram(&sProgram);

    C3D_AttrInfo *attr = C3D_GetAttrInfo();
    AttrInfo_Init(attr);
    AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 4); /* position */
    AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 4); /* color */
    AttrInfo_AddLoader(attr, 2, GPU_FLOAT, 2); /* texcoord */
    C3D_BufInfo *buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, sVerts, sizeof(GpuVertex), 3, 0x210);
    Gpu_FlushDataCache(sVerts, (u32)sNumVerts * sizeof(GpuVertex));

    C3D_CullFace(GPU_CULL_NONE);
    C3D_DepthMap(!sWBuffer || GPU3D_WMODE != 1, -1.0f, 0.0f);
    for (int k = 0; k < sNumBatches; k++) {
#ifdef GPU3D_DEBUG_REVERSE
        int i = sNumBatches - 1 - k;
#else
        int i = k;
#endif
        const Batch *b = &sBatches[i];
        if (b->translucent) {
            SetupState(b, PASS_OVER_CLEARED);
            C3D_DrawArrays(GPU_TRIANGLES, b->first, b->count);
            SetupState(b, PASS_OVER_DRAWN);
        } else {
            SetupState(b, PASS_OPAQUE);
        }
        C3D_DrawArrays(GPU_TRIANGLES, b->first, b->count);
    }
    C3D_StencilTest(false, GPU_ALWAYS, 0, 0xff, 0);
    sDrawn = true;
#ifdef GPU3D_DEBUG_DUMP_FRAME
    if (sDumpCountdown == 1) {
        Gpu3D_ReadBack();
        DumpImage("sdmc:/gpu3d_gpu.bin");
        sDumpCountdown = -1;
    }
#endif
    return true;
}

C3D_Tex *Gpu3D_Texture(void)
{
    return sDrawn ? &sTargetTex : NULL;
}

void Gpu3D_Init(void)
{
    CreateTarget(1);
    sShader = DVLB_ParseFile((u32 *)gGpu3DShader, gGpu3DShader_size);
    shaderProgramInit(&sProgram);
    shaderProgramSetVsh(&sProgram, &sShader->DVLE[0]);
    sReadback = linearAlloc(TARGET_W * TARGET_H * 4);
    sVerts = linearAlloc(MAX_VERTS * sizeof(GpuVertex));
#ifndef GPU3D_DEBUG_SOFTWARE
    gNtrRender3DHook = Submit;
#endif
}
