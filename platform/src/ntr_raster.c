/*
 * Software rasterizer for the DS 3D engine.
 *
 * Renders the polygon list built by ntr_g3.c into the 3D frame (shown as BG0
 * of the main 2D engine). Implements the DS pixel pipeline: all texture
 * formats with repeat/flip/clamp, modulate/decal/toon/highlight shading,
 * alpha test, alpha blending, Z or W depth buffering, the equal-depth decal
 * test, the translucent polygon ID rule, and opaque-then-translucent order
 * (translucent sorted by Y unless manual sort). Interpolation is
 * perspective-correct.
 *
 * Not implemented yet: edge marking, fog, anti-aliasing, shadow polygons,
 * wireframe polygons and the rear-plane clear image.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "ntr_g3_internal.h"

/* Buffers hold the largest output; sOutW x sOutH is the current one. */
#define W NTR_MAX_SCREEN_W
#define H NTR_MAX_SCREEN_H
static int sOutW = 256, sOutH = 192;

u16 gNtr3DColor[H][W];
u8 gNtr3DAlpha[H][W];
u8 gNtr3DRowTranslucent[H];

/* Texture memory as the 3D engine sees it (banks mapped as texture slots). */
#define sTexVram gNtrTexVram
#define sTexPltt gNtrTexPltt
#define TEXVRAM_SIZE 0x80000
#define TEXPLTT_SIZE 0x18000

/* Working buffers: 6-bit color per channel. */
static u8 sR[H][W], sG[H][W], sB[H][W], sA[H][W];
static u32 sDepth[H][W];
static u8 sOpaqueID[H][W];
static u8 sTransID[H][W]; /* 0xff = no translucent pixel yet */

/* VRAM transfers ------------------------------------------------------------- */

/* The SDK maps the texture banks to the CPU while loading and maps them back
 * afterwards, which (re)enables texture mapping in DISP3DCNT. Here the loads
 * write straight to texture memory, so only that register changes (remapping
 * the banks would copy all of them out and back, and mark every texture as
 * changed). */
void GX_BeginLoadTex(void) {}
void GX_EndLoadTex(void)
{
    u16 v = NTR_IO16(0x060) & (u16) ~(REG_G3X_DISP3DCNT_TME_MASK | REG_G3X_DISP3DCNT_RO_MASK | REG_G3X_DISP3DCNT_GO_MASK);
    NTR_IO16(0x060) = GX_GetBankForTex() != 0 ? v | REG_G3X_DISP3DCNT_TME_MASK : v;
}
void GX_BeginLoadTexPltt(void) {}
void GX_EndLoadTexPltt(void) {}
void GX_BeginLoadClearImage(void) {}
void GX_EndLoadClearImage(void) {}
void GX_LoadClearImageColor(const void *src, u32 size) { (void)src; (void)size; }
void GX_LoadClearImageDepth(const void *src, u32 size) { (void)src; (void)size; }

void GX_LoadTex(const void *src, u32 destSlotAddr, u32 size)
{
#ifdef NTR_DEBUG_HOOKS
    if (getenv("PORT_PLTTLOG")) {
        printf("LoadTex %05x+%x\n", destSlotAddr, size);
    }
#endif
    if (destSlotAddr < TEXVRAM_SIZE) {
        if (destSlotAddr + size > TEXVRAM_SIZE) {
            size = TEXVRAM_SIZE - destSlotAddr;
        }
        memcpy(sTexVram + destSlotAddr, src, size);
        Ntr_TexMemoryWritten(FALSE, destSlotAddr, size);
    }
}

void GX_LoadTexPltt(const void *src, u32 destSlotAddr, u32 size)
{
#ifdef NTR_DEBUG_HOOKS
    if (getenv("PORT_PLTTLOG")) {
        printf("LoadTexPltt %05x+%x\n", destSlotAddr, size);
    }
#endif
    if (destSlotAddr < TEXPLTT_SIZE) {
        if (destSlotAddr + size > TEXPLTT_SIZE) {
            size = TEXPLTT_SIZE - destSlotAddr;
        }
        memcpy(sTexPltt + destSlotAddr, src, size);
        Ntr_TexMemoryWritten(TRUE, destSlotAddr, size);
    }
}

/* Rendering control ------------------------------------------------------------ */

/* Rendering control registers (DS I/O offsets): CLEAR_COLOR 0x350,
 * CLEAR_DEPTH 0x354, EDGE_COLOR 0x330, FOG_TABLE 0x360, TOON_TABLE 0x380. */
void G3X_SetClearColor(GXRgb rgb, int alpha, int depth, int polygonID, BOOL fog)
{
    NTR_IO32(0x350) = (u32)(rgb & 0x7fff) | ((u32)(fog != 0) << 15) | ((u32)(alpha & 31) << 16) | ((u32)(polygonID & 63) << 24);
    NTR_IO16(0x354) = (u16)(depth & 0x7fff);
}

void G3X_SetToonTable(const GXRgb *rgb_32) { memcpy(&gNtrIoRegs[0x380], rgb_32, 64); }
void G3X_SetEdgeColorTable(const GXRgb *rgb_8) { memcpy(&gNtrIoRegs[0x330], rgb_8, 16); }
void G3X_SetFogTable(const u32 *fogTable) { memcpy(&gNtrIoRegs[0x360], fogTable, 32); }

void G3X_SetFog(BOOL enable, GXFogBlend fogMode, GXFogSlope fogSlope, int fogOffset)
{
    u16 cnt = NTR_IO16(0x060) & ~(REG_G3X_DISP3DCNT_FME_MASK | REG_G3X_DISP3DCNT_FMOD_MASK | REG_G3X_DISP3DCNT_FOG_SHIFT_MASK);
    if (enable) {
        cnt |= REG_G3X_DISP3DCNT_FME_MASK | (fogMode << REG_G3X_DISP3DCNT_FMOD_SHIFT) | (fogSlope << REG_G3X_DISP3DCNT_FOG_SHIFT_SHIFT);
    }
    NTR_IO16(0x060) = cnt;
    NTR_IO16(0x35c) = (u16)fogOffset;
}

void G3X_SetDisp1DotDepth(fx32 w) { NTR_IO16(0x610) = (u16)(w >> 9); }
void G3X_SetHOffset(int hOffset) { NTR_IO16(0x010) = (u16)(hOffset & 0x1ff); }

/* Textures ----------------------------------------------------------------------- */

static inline u16 Rd16(const u8 *base, u32 size, u32 addr)
{
    return addr + 1 < size ? (u16)(base[addr] | base[addr + 1] << 8) : 0;
}

static inline int Wrap(int c, int size, BOOL repeat, BOOL flip)
{
    if (!repeat) {
        return c < 0 ? 0 : (c >= size ? size - 1 : c);
    }
    if (flip) {
        int m = c & (2 * size - 1);
        return m >= size ? 2 * size - 1 - m : m;
    }
    return c & (size - 1);
}

/* Returns texel (u, v) of a texture (in range) as RGB555 and sets *alpha to 0..31. */
u16 NtrRaster_TexelAt(u32 image, u32 pltt, int u, int v, int *alpha)
{
    int fmt = (image >> 26) & 7;
    int sizeS = 8 << ((image >> 20) & 7);
    u32 addr = (image & 0xffff) << 3;
    BOOL color0Transparent = (image >> 29) & 1;
    u32 palBase = pltt << 4;
    u32 idx = (u32)(v * sizeS + u);
    *alpha = 31;

    switch (fmt) {
    case GX_TEXFMT_A3I5: {
        u8 b = sTexVram[(addr + idx) & 0x7ffff];
        int a3 = b >> 5;
        *alpha = a3 * 4 + a3 / 2;
        return Rd16(sTexPltt, TEXPLTT_SIZE, palBase + (b & 31) * 2);
    }
    case GX_TEXFMT_PLTT4: {
        u8 b = sTexVram[(addr + idx / 4) & 0x7ffff];
        int i = (b >> ((idx & 3) * 2)) & 3;
        if (i == 0 && color0Transparent) {
            *alpha = 0;
        }
        return Rd16(sTexPltt, TEXPLTT_SIZE, (pltt << 3) + i * 2);
    }
    case GX_TEXFMT_PLTT16: {
        u8 b = sTexVram[(addr + idx / 2) & 0x7ffff];
        int i = (idx & 1) ? b >> 4 : b & 15;
        if (i == 0 && color0Transparent) {
            *alpha = 0;
        }
        return Rd16(sTexPltt, TEXPLTT_SIZE, palBase + i * 2);
    }
    case GX_TEXFMT_PLTT256: {
        int i = sTexVram[(addr + idx) & 0x7ffff];
        if (i == 0 && color0Transparent) {
            *alpha = 0;
        }
        return Rd16(sTexPltt, TEXPLTT_SIZE, palBase + i * 2);
    }
    case GX_TEXFMT_COMP4x4: {
        u32 block = (u32)((v >> 2) * (sizeS >> 2) + (u >> 2));
        u32 wordAddr = (addr + block * 4) & 0x7ffff;
        u32 word = sTexVram[wordAddr] | sTexVram[wordAddr + 1] << 8 | sTexVram[wordAddr + 2] << 16 | (u32)sTexVram[wordAddr + 3] << 24;
        int texel = (word >> ((v & 3) * 8 + (u & 3) * 2)) & 3;
        u32 idxAddr = 0x20000 + ((addr & 0x1ffff) >> 1) + ((addr & 0x40000) >> 2) + block * 2;
        u16 info = Rd16(sTexVram, TEXVRAM_SIZE, idxAddr);
        u32 pal = palBase + ((info & 0x3fff) << 2);
        int mode = info >> 14;
        u16 c0 = Rd16(sTexPltt, TEXPLTT_SIZE, pal);
        u16 c1 = Rd16(sTexPltt, TEXPLTT_SIZE, pal + 2);
        if (texel < 2) {
            return texel ? c1 : c0;
        }
        if (mode == 2 || (mode == 0 && texel == 2)) {
            return Rd16(sTexPltt, TEXPLTT_SIZE, pal + texel * 2);
        }
        if (texel == 3 && mode < 2) {
            *alpha = 0;
            return 0;
        }
        int w0 = mode == 1 ? 4 : (texel == 2 ? 5 : 3);
        int w1 = 8 - w0;
        int r = (((c0 & 31) * w0) + ((c1 & 31) * w1)) / 8;
        int g = ((((c0 >> 5) & 31) * w0) + (((c1 >> 5) & 31) * w1)) / 8;
        int b = ((((c0 >> 10) & 31) * w0) + (((c1 >> 10) & 31) * w1)) / 8;
        return (u16)(r | g << 5 | b << 10);
    }
    case GX_TEXFMT_A5I3: {
        u8 b = sTexVram[(addr + idx) & 0x7ffff];
        *alpha = b >> 3;
        return Rd16(sTexPltt, TEXPLTT_SIZE, palBase + (b & 7) * 2);
    }
    case GX_TEXFMT_DIRECT: {
        u16 c = Rd16(sTexVram, TEXVRAM_SIZE, (addr + idx * 2) & 0x7ffff);
        *alpha = (c & 0x8000) ? 31 : 0;
        return c & 0x7fff;
    }
    default:
        return 0x7fff;
    }
}

static u16 Texel(u32 image, u32 pltt, float s, float t, int *alpha)
{
    int sizeS = 8 << ((image >> 20) & 7);
    int sizeT = 8 << ((image >> 23) & 7);
    int u = Wrap((int)floorf(s / 16.0f), sizeS, (image >> 16) & 1, (image >> 18) & 1);
    int v = Wrap((int)floorf(t / 16.0f), sizeT, (image >> 17) & 1, (image >> 19) & 1);
    return NtrRaster_TexelAt(image, pltt, u, v, alpha);
}

/* Polygon rasterization --------------------------------------------------------------- */

typedef struct ScreenVert {
    float x, y; /* pixels, y down */
    float invW;
    float z; /* Z-buffer depth (24-bit scale), linear in screen space */
    float cw[3], sw, tw; /* attributes divided by w */
} ScreenVert;

static inline int Expand6(int c5) { return c5 ? c5 * 2 + 1 : 0; }

static BOOL IsTranslucent(const NtrG3Polygon *p)
{
    int alpha = (p->polyAttr >> 16) & 31;
    int fmt = (p->texImage >> 26) & 7;
    BOOL tex = (NTR_IO16(0x060) & 1) && fmt != GX_TEXFMT_NONE;
    return (alpha > 0 && alpha < 31) || (tex && (fmt == GX_TEXFMT_A3I5 || fmt == GX_TEXFMT_A5I3));
}

static void ShadePixel(const NtrG3Polygon *p, int x, int y, u32 depth, const float col[3], float s, float t)
{
    u32 attr = p->polyAttr;
    int polyAlpha = (attr >> 16) & 31;
    int mode = (attr >> 4) & 3;
    int polyID = (attr >> 24) & 63;
    BOOL translucent;

    /* Depth test: less than, or equal (within one unit of 0x200) for decal polygons. */
    if (attr & REG_G3_POLYGON_ATTR_DT_MASK) {
        u32 d = sDepth[y][x];
        if (depth + 0x200 < d || depth > d + 0x200) {
            return;
        }
    } else if (depth >= sDepth[y][x]) {
        return;
    }

    int vr = Expand6((int)(col[0] + 0.5f)), vg = Expand6((int)(col[1] + 0.5f)), vb = Expand6((int)(col[2] + 0.5f));
    int r, g, b, a;

    int fmt = (p->texImage >> 26) & 7;
    BOOL textured = (NTR_IO16(0x060) & 1) && fmt != GX_TEXFMT_NONE;
    int tr = 63, tg = 63, tb = 63, ta = 31;
    if (textured) {
        u16 c = Texel(p->texImage, p->texPltt, s, t, &ta);
        tr = Expand6(c & 31);
        tg = Expand6((c >> 5) & 31);
        tb = Expand6((c >> 10) & 31);
    }

    if (mode == GX_POLYGONMODE_DECAL && textured) {
        if (ta == 0) {
            r = vr, g = vg, b = vb;
        } else if (ta == 31) {
            r = tr, g = tg, b = tb;
        } else {
            r = (tr * ta + vr * (31 - ta)) / 32;
            g = (tg * ta + vg * (31 - ta)) / 32;
            b = (tb * ta + vb * (31 - ta)) / 32;
        }
        a = polyAlpha;
    } else {
        if (mode == GX_POLYGONMODE_TOON) {
            u16 toon = ((const u16 *)&gNtrIoRegs[0x380])[(vr >> 1) & 31];
            int toonR = Expand6(toon & 31), toonG = Expand6((toon >> 5) & 31), toonB = Expand6((toon >> 10) & 31);
            if (!(NTR_IO16(0x060) & 2)) {
                vr = toonR, vg = toonG, vb = toonB; /* toon: table color replaces the vertex color */
            }
            r = ((tr + 1) * (vr + 1) - 1) / 64;
            g = ((tg + 1) * (vg + 1) - 1) / 64;
            b = ((tb + 1) * (vb + 1) - 1) / 64;
            if (NTR_IO16(0x060) & 2) {
                r = r + toonR > 63 ? 63 : r + toonR;
                g = g + toonG > 63 ? 63 : g + toonG;
                b = b + toonB > 63 ? 63 : b + toonB;
            }
        } else {
            r = ((tr + 1) * (vr + 1) - 1) / 64;
            g = ((tg + 1) * (vg + 1) - 1) / 64;
            b = ((tb + 1) * (vb + 1) - 1) / 64;
        }
        a = ((ta + 1) * (polyAlpha + 1) - 1) / 32;
    }

    if (a == 0) {
        return;
    }
    if ((NTR_IO16(0x060) & 4) && a <= (NTR_IO16(0x340) & 31)) {
        return;
    }

    translucent = a < 31;
    if (!translucent) {
        sR[y][x] = (u8)r;
        sG[y][x] = (u8)g;
        sB[y][x] = (u8)b;
        sA[y][x] = 31;
        sDepth[y][x] = depth;
        sOpaqueID[y][x] = (u8)polyID;
        sTransID[y][x] = 0xff;
        return;
    }

    if (sTransID[y][x] == polyID) {
        return; /* same translucent polygon ID already drawn here */
    }
    if ((NTR_IO16(0x060) & 8) && sA[y][x] != 0) {
        sR[y][x] = (u8)((r * (a + 1) + sR[y][x] * (31 - a)) / 32);
        sG[y][x] = (u8)((g * (a + 1) + sG[y][x] * (31 - a)) / 32);
        sB[y][x] = (u8)((b * (a + 1) + sB[y][x] * (31 - a)) / 32);
        if (a > sA[y][x]) {
            sA[y][x] = (u8)a;
        }
    } else {
        sR[y][x] = (u8)r;
        sG[y][x] = (u8)g;
        sB[y][x] = (u8)b;
        sA[y][x] = (u8)a;
    }
    sTransID[y][x] = (u8)polyID;
    if (attr & REG_G3_POLYGON_ATTR_XL_MASK) {
        sDepth[y][x] = depth;
    }
}

static inline float EdgeFn(const ScreenVert *a, const ScreenVert *b, float px, float py)
{
    return (b->x - a->x) * (py - a->y) - (b->y - a->y) * (px - a->x);
}

/* Top-left fill rule, so neighbouring triangles never draw a pixel twice. */
static inline BOOL IsTopLeft(const ScreenVert *a, const ScreenVert *b, float area)
{
    float dx = b->x - a->x, dy = b->y - a->y;
    if (area < 0) {
        dx = -dx, dy = -dy;
    }
    return (dy < 0) || (dy == 0 && dx > 0);
}

static void DrawTriangle(const NtrG3Polygon *p, const ScreenVert *v0, const ScreenVert *v1, const ScreenVert *v2, BOOL wBuffer)
{
    float area = EdgeFn(v0, v1, v2->x, v2->y);
    if (area == 0) {
        return;
    }
    int minX = (int)floorf(fminf(v0->x, fminf(v1->x, v2->x)));
    int maxX = (int)ceilf(fmaxf(v0->x, fmaxf(v1->x, v2->x)));
    int minY = (int)floorf(fminf(v0->y, fminf(v1->y, v2->y)));
    int maxY = (int)ceilf(fmaxf(v0->y, fmaxf(v1->y, v2->y)));
    if (minX < 0) minX = 0;
    if (minY < 0) minY = 0;
    if (maxX > sOutW - 1) maxX = sOutW - 1;
    if (maxY > sOutH - 1) maxY = sOutH - 1;

    BOOL tl0 = IsTopLeft(v1, v2, area), tl1 = IsTopLeft(v2, v0, area), tl2 = IsTopLeft(v0, v1, area);
    float inv = 1.0f / area;

    for (int y = minY; y <= maxY; y++) {
        float py = y + 0.5f;
        for (int x = minX; x <= maxX; x++) {
            float px = x + 0.5f;
            float w0 = EdgeFn(v1, v2, px, py) * inv;
            float w1 = EdgeFn(v2, v0, px, py) * inv;
            float w2 = EdgeFn(v0, v1, px, py) * inv;
            if (w0 < 0 || w1 < 0 || w2 < 0) {
                continue;
            }
            if ((w0 == 0 && !tl0) || (w1 == 0 && !tl1) || (w2 == 0 && !tl2)) {
                continue;
            }

            float invW = w0 * v0->invW + w1 * v1->invW + w2 * v2->invW;
            float pw = 1.0f / invW;
            u32 depth;
            if (wBuffer) {
                float d = pw * 4096.0f;
                depth = d >= 16777215.0f ? 0xffffff : (u32)(d < 0 ? 0 : d);
            } else {
                float d = w0 * v0->z + w1 * v1->z + w2 * v2->z;
                depth = d >= 16777215.0f ? 0xffffff : (u32)(d < 0 ? 0 : d);
            }
            float col[3];
            for (int k = 0; k < 3; k++) {
                col[k] = (w0 * v0->cw[k] + w1 * v1->cw[k] + w2 * v2->cw[k]) * pw;
            }
            float s = (w0 * v0->sw + w1 * v1->sw + w2 * v2->sw) * pw;
            float t = (w0 * v0->tw + w1 * v1->tw + w2 * v2->tw) * pw;
            ShadePixel(p, x, y, depth, col, s, t);
        }
    }
}

static void DrawPolygon(const NtrG3PolyList *list, const NtrG3Polygon *p, BOOL wBuffer)
{
    if (((p->polyAttr >> 4) & 3) == GX_POLYGONMODE_SHADOW) {
        return; /* shadow volumes need the stencil path, not implemented yet */
    }
    if (((p->polyAttr >> 16) & 31) == 0) {
        return; /* wireframe, not implemented yet */
    }

    ScreenVert sv[NTR_G3_MAX_POLY_VERTS];
    for (int i = 0; i < p->numVerts; i++) {
        const NtrG3Vertex *v = &list->verts[p->firstVert + i];
        float w = v->w > 1e-6f ? v->w : 1e-6f;
        float iw = 1.0f / w;
        float yUp;
        NtrG3_OutputPos(p, v, sOutW, sOutH, &sv[i].x, &yUp);
        sv[i].y = (float)sOutH - yUp;
        sv[i].invW = iw;
        sv[i].z = (v->c[2] * iw * 16384.0f + 16383.0f) * 512.0f;
        for (int k = 0; k < 3; k++) {
            sv[i].cw[k] = v->color[k] * iw;
        }
        sv[i].sw = v->s * iw;
        sv[i].tw = v->t * iw;
    }
    for (int i = 1; i + 1 < p->numVerts; i++) {
        DrawTriangle(p, &sv[0], &sv[i], &sv[i + 1], wBuffer);
    }
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

static const NtrG3PolyList *sSortList;

static int CompareTopY(const void *a, const void *b)
{
    float ya = PolyTopY(sSortList, &sSortList->polys[*(const int *)a]);
    float yb = PolyTopY(sSortList, &sSortList->polys[*(const int *)b]);
    if (ya != yb) {
        return ya < yb ? -1 : 1;
    }
    return *(const int *)a - *(const int *)b;
}

void (*gNtrRender3DHook)(const NtrG3PolyList *list, u32 swapParam);

#ifdef NTR_DEBUG_HOOKS
/* Debugging (PORT_TEXCHECK): NtrTex_DecodeTiled against NtrRaster_TexelAt,
 * for every texture of the frame. */
static void CheckTextureDecode(const NtrG3PolyList *list)
{
    static u32 *buf;
    static u32 checked, bad;
    u32 seen[64];
    int numSeen = 0;
    if (buf == NULL) {
        buf = malloc(1024 * 1024 * 4);
    }
    for (int i = 0; i < list->count; i++) {
        u32 image = list->polys[i].texImage & ~0x000f0000u, pltt = list->polys[i].texPltt;
        if (((image >> 26) & 7) == GX_TEXFMT_NONE) {
            continue;
        }
        u32 key = image ^ pltt * 0x9e3779b9u;
        int k;
        for (k = 0; k < numSeen && seen[k] != key; k++) {
        }
        if (k < numSeen) {
            continue;
        }
        if (numSeen < 64) {
            seen[numSeen++] = key;
        }
        u32 w = 8u << ((image >> 20) & 7), h = 8u << ((image >> 23) & 7);
        NtrTex_DecodeTiled(image, pltt, buf);
        u32 wrong = 0;
        for (u32 y = 0; y < h; y++) {
            for (u32 x = 0; x < w; x++) {
                int a;
                u16 c = NtrRaster_TexelAt(image, pltt, (int)x, (int)y, &a);
                u32 r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
                r = (r << 3) | (r >> 2), g = (g << 3) | (g >> 2), b = (b << 3) | (b >> 2);
                u32 want = (r << 24) | (g << 16) | (b << 8) | ((u32)a * 255 / 31);
                u32 tile = ((y >> 3) * (w >> 3) + (x >> 3)) << 6;
                u32 m = (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
                wrong += buf[tile + m] != want;
            }
        }
        checked++;
        if (wrong) {
            bad++;
            printf("texture decode: image %08x pltt %04x format %u: %u of %u texels differ\n", image, pltt, (image >> 26) & 7, wrong, w * h);
        }
    }
    static int frames;
    if (++frames % 500 == 0) {
        printf("texture decode check: %u textures, %u differ\n", checked, bad);
    }
}
#endif

void NtrRaster_Render(const NtrG3PolyList *list, u32 swapParam)
{
#ifdef NTR_DEBUG_HOOKS
    if (getenv("PORT_TEXCHECK")) {
        CheckTextureDecode(list);
    }
#endif
    if (gNtrRender3DHook != NULL) {
        gNtrRender3DHook(list, swapParam);
        return;
    }

    BOOL manualSort = swapParam & 1;
    BOOL wBuffer = (swapParam >> 1) & 1;
    NtrG3_OutputSize(&sOutW, &sOutH);
#ifdef NTR_DEBUG_HOOKS
    /* Test harness diagnostics: PORT_DUMP_POLYS=N prints polygon statistics every
     * N 3D frames, PORT_PROBE=x,y lists the polygons covering a DS pixel. */
    if (getenv("PORT_DUMP_POLYS")) {
        static int frame;
        if (++frame % atoi(getenv("PORT_DUMP_POLYS")) == 0) {
            int modes[4] = { 0 }, fmts[8] = { 0 }, trans = 0;
            for (int i = 0; i < list->count; i++) {
                const NtrG3Polygon *p = &list->polys[i];
                modes[(p->polyAttr >> 4) & 3]++;
                fmts[(p->texImage >> 26) & 7]++;
                trans += IsTranslucent(p);
            }
            printf("3D frame %d (vblank %u): %d polys, swap %x, DISP3DCNT %04x; modes mod/dec/toon/shadow %d/%d/%d/%d; translucent %d; formats",
                frame, Ntr_FramesEnded(), list->count, swapParam, NTR_IO16(0x060), modes[0], modes[1], modes[2], modes[3], trans);
            for (int f = 0; f < 8; f++) {
                printf(" %d", fmts[f]);
            }
            printf("\n");
            float wmin = 1e30f, wmax = -1e30f, zmin = 1e30f, zmax = -1e30f;
            for (int i = 0; i < list->vertexCount; i++) {
                const NtrG3Vertex *v = &list->verts[i];
                wmin = fminf(wmin, v->w), wmax = fmaxf(wmax, v->w);
                zmin = fminf(zmin, v->c[2] / v->w), zmax = fmaxf(zmax, v->c[2] / v->w);
            }
            printf("  w %g..%g  z/w %g..%g\n", wmin, wmax, zmin, zmax);
            /* Polygons covering a probe point (PORT_PROBE=x,y, DS pixels). */
            int px = -1, py = -1;
            if (getenv("PORT_PROBE")) {
                sscanf(getenv("PORT_PROBE"), "%d,%d", &px, &py);
            }
            for (int i = 0; px >= 0 && i < list->count; i++) {
                const NtrG3Polygon *p = &list->polys[i];
                float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
                for (int k = 0; k < p->numVerts; k++) {
                    float x, yUp;
                    NtrG3_OutputPos(p, &list->verts[p->firstVert + k], 256, 192, &x, &yUp);
                    x0 = fminf(x0, x), x1 = fmaxf(x1, x), y0 = fminf(y0, 192 - yUp), y1 = fmaxf(y1, 192 - yUp);
                }
                if (px >= x0 && px <= x1 && py >= y0 && py <= y1 && getenv("PORT_PROBE_W")) {
                    /* Exact coverage and perspective-correct W at the pixel centre. */
                    float sx[NTR_G3_MAX_POLY_VERTS], sy[NTR_G3_MAX_POLY_VERTS], iw[NTR_G3_MAX_POLY_VERTS];
                    for (int k = 0; k < p->numVerts; k++) {
                        float yUp;
                        const NtrG3Vertex *v = &list->verts[p->firstVert + k];
                        NtrG3_OutputPos(p, v, 256, 192, &sx[k], &yUp);
                        sy[k] = 192 - yUp;
                        iw[k] = 1.0f / (v->w > 1e-6f ? v->w : 1e-6f);
                    }
                    float cx = px + 0.5f, cy = py + 0.5f;
                    for (int k = 1; k + 1 < p->numVerts; k++) {
                        float ax = sx[0], ay = sy[0], bx = sx[k], by = sy[k], qx = sx[k + 1], qy = sy[k + 1];
                        float area = (bx - ax) * (qy - ay) - (by - ay) * (qx - ax);
                        if (area == 0) continue;
                        float w0 = ((bx - cx) * (qy - cy) - (by - cy) * (qx - cx)) / area;
                        float w1 = ((qx - cx) * (ay - cy) - (qy - cy) * (ax - cx)) / area;
                        float w2 = 1 - w0 - w1;
                        if (w0 >= 0 && w1 >= 0 && w2 >= 0) {
                            float invW = w0 * iw[0] + w1 * iw[k] + w2 * iw[k + 1];
                            float linW = w0 / iw[0] + w1 / iw[k] + w2 / iw[k + 1];
                            printf("  covers: poly %d tex %08x W(persp) %.3f W(linear) %.3f\n", i, p->texImage, 1 / invW, linW);
                        }
                    }
                }
                if (px >= x0 && px <= x1 && py >= y0 && py <= y1 && getenv("PORT_PROBE_ST")) {
                    /* The texture coordinates, and how many texels of their rectangle are opaque. */
                    float s0 = 1e9f, s1 = -1e9f, t0 = 1e9f, t1 = -1e9f;
                    for (int k = 0; k < p->numVerts; k++) {
                        const NtrG3Vertex *v = &list->verts[p->firstVert + k];
                        printf("    vertex %d: s %.1f t %.1f\n", k, v->s / 16, v->t / 16);
                        s0 = fminf(s0, v->s / 16), s1 = fmaxf(s1, v->s / 16), t0 = fminf(t0, v->t / 16), t1 = fmaxf(t1, v->t / 16);
                    }
                    int opaque = 0, total = 0;
                    for (int t = (int)t0; t < (int)t1; t++) {
                        for (int u = (int)s0; u < (int)s1; u++) {
                            int a;
                            NtrRaster_TexelAt(p->texImage, p->texPltt, u & 255, t & 255, &a);
                            opaque += a != 0;
                            total++;
                        }
                    }
                    printf("    texels in s %.0f-%.0f t %.0f-%.0f: %d of %d opaque\n", s0, s1, t0, t1, opaque, total);
                }
                if (px >= x0 && px <= x1 && py >= y0 && py <= y1 && !getenv("PORT_PROBE_W")) {
                    int a;
                    u16 c = NtrRaster_TexelAt(p->texImage, p->texPltt, 4, 4, &a);
                    printf("  poly %d: box %.0f-%.0f,%.0f-%.0f attr %08x tex %08x pltt %04x fmt %d texel(4,4) %04x a%d\n", i, x0, x1, y0, y1,
                        p->polyAttr, p->texImage, p->texPltt, (p->texImage >> 26) & 7, c, a);
                    u32 ta = (p->texImage & 0xffff) << 3, pa = p->texPltt << 4;
                    printf("    texels @%05x:", ta);
                    for (int k = 0; k < 16; k++) printf(" %02x", sTexVram[(ta + 64 + k) & 0x7ffff]);
                    printf("\n    pltt @%05x:", pa);
                    for (int k = 0; k < 16; k++) printf(" %04x", Rd16(sTexPltt, TEXPLTT_SIZE, pa + k * 2));
                    printf("\n");
                }
            }
        }
    }
#endif

    u32 clear = NTR_IO32(0x350);
    u16 cc = clear & 0x7fff;
    u8 cr = (u8)Expand6(cc & 31), cg = (u8)Expand6((cc >> 5) & 31), cb = (u8)Expand6((cc >> 10) & 31);
    u8 clearAlpha = (clear >> 16) & 31, clearPolyID = (clear >> 24) & 63;
    u32 cd = (NTR_IO16(0x354) & 0x7fff) * 0x200 + 0x1ff;
    for (int y = 0; y < sOutH; y++) {
        for (int x = 0; x < sOutW; x++) {
            sR[y][x] = cr;
            sG[y][x] = cg;
            sB[y][x] = cb;
            sA[y][x] = clearAlpha;
            sDepth[y][x] = cd;
            sOpaqueID[y][x] = clearPolyID;
            sTransID[y][x] = 0xff;
        }
    }

    static int opaque[NTR_G3_MAX_POLYS], trans[NTR_G3_MAX_POLYS];
    int numOpaque = 0, numTrans = 0;
    for (int i = 0; i < list->count; i++) {
        if (IsTranslucent(&list->polys[i])) {
            trans[numTrans++] = i;
        } else {
            opaque[numOpaque++] = i;
        }
    }
    sSortList = list;
    qsort(opaque, numOpaque, sizeof(int), CompareTopY);
    if (!manualSort) {
        qsort(trans, numTrans, sizeof(int), CompareTopY);
    }

    for (int i = 0; i < numOpaque; i++) {
        DrawPolygon(list, &list->polys[opaque[i]], wBuffer);
    }
    for (int i = 0; i < numTrans; i++) {
        DrawPolygon(list, &list->polys[trans[i]], wBuffer);
    }

    for (int y = 0; y < sOutH; y++) {
        u8 translucent = 0;
        for (int x = 0; x < sOutW; x++) {
            u8 a = sA[y][x];
            gNtr3DColor[y][x] = (u16)((sR[y][x] >> 1) | (sG[y][x] >> 1) << 5 | (sB[y][x] >> 1) << 10 | (a == 0 ? 0x8000 : 0));
            gNtr3DAlpha[y][x] = a;
            translucent |= a != 0 && a < 31;
        }
        gNtr3DRowTranslucent[y] = translucent;
    }
}
