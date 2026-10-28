/*
 * Software renderer for the emulated DS 2D engines.
 *
 * Covers BGs (text, affine, extended 256x16 palette / 256-color bitmap /
 * direct color bitmap), sprites (OBJ: 1D/2D mapping, 16/256 colors,
 * extended palettes, affine and double-size, semi-transparent, OBJ window),
 * windows, priorities, alpha and brightness blending, and master
 * brightness, and the 3D layer as BG0 of the main engine. Mosaic and bitmap
 * sprites are not drawn yet.
 */
#include <stdio.h>
#include <stdlib.h>

#include "ntr_shim.h"

/* Time per renderer stage (NtrHost_Ticks units), for profiling. Reading the
 * clock per line is not free on the 3DS (a system call), so it is only done
 * when built with -DNTR_PROFILE_STAGES. */
u64 gNtrRenderTicks[2][3]; /* per engine: BG lines, OBJ lines, composition */
u64 gNtrExtTicks[4]; /* composition: column search, columns, rows, regions and passes */
#ifdef NTR_PROFILE_STAGES
#define STAGE_TICKS() NtrHost_Ticks()
#else
#define STAGE_TICKS() 0
#endif

/* Registers of one engine, decoded from the emulated I/O image at the start of
 * a frame, plus its VRAM/palette/OAM views. */
typedef struct Eng {
    u32 dispcnt;
    u16 bgcnt[4];
    u16 bghofs[4];
    u16 bgvofs[4];
    s16 bgpa[2], bgpb[2], bgpc[2], bgpd[2]; /* BG2, BG3 */
    s32 bgx[2], bgy[2];
    u16 bldcnt, bldalpha, bldy;
    u16 mosaic;
    u16 masterBright;
    u8 win0[4], win1[4]; /* x1, x2, y1, y2 (x2/y2 exclusive) */
    u16 winin, winout;
    const u8 *bgVram;
    u32 bgVramSize;
    const u8 *objVram;
    u32 objVramSize;
    const u8 *bgExtPltt;
    const u8 *objExtPltt;
    const u16 *bgPltt;
    const u16 *objPltt;
    const u8 *oam;
} Eng;

static void Snapshot(const NtrEngine *src, Eng *e)
{
    u32 io = src->io;
    e->dispcnt = NTR_IO32(io + 0x000);
    for (int i = 0; i < 4; i++) {
        e->bgcnt[i] = NTR_IO16(io + 0x008 + i * 2);
        e->bghofs[i] = NTR_IO16(io + 0x010 + i * 4) & 0x1ff;
        e->bgvofs[i] = NTR_IO16(io + 0x012 + i * 4) & 0x1ff;
    }
    for (int i = 0; i < 2; i++) {
        u32 b = io + 0x020 + i * 0x10;
        e->bgpa[i] = (s16)NTR_IO16(b + 0);
        e->bgpb[i] = (s16)NTR_IO16(b + 2);
        e->bgpc[i] = (s16)NTR_IO16(b + 4);
        e->bgpd[i] = (s16)NTR_IO16(b + 6);
        e->bgx[i] = (s32)(NTR_IO32(b + 8) << 4) >> 4; /* 28-bit signed */
        e->bgy[i] = (s32)(NTR_IO32(b + 12) << 4) >> 4;
    }
    u16 w0h = NTR_IO16(io + 0x040), w1h = NTR_IO16(io + 0x042), w0v = NTR_IO16(io + 0x044), w1v = NTR_IO16(io + 0x046);
    e->win0[0] = (u8)(w0h >> 8), e->win0[1] = (u8)w0h, e->win0[2] = (u8)(w0v >> 8), e->win0[3] = (u8)w0v;
    e->win1[0] = (u8)(w1h >> 8), e->win1[1] = (u8)w1h, e->win1[2] = (u8)(w1v >> 8), e->win1[3] = (u8)w1v;
    e->winin = NTR_IO16(io + 0x048);
    e->winout = NTR_IO16(io + 0x04a);
    e->mosaic = NTR_IO16(io + 0x04c);
    e->bldcnt = NTR_IO16(io + 0x050);
    e->bldalpha = NTR_IO16(io + 0x052);
    e->bldy = NTR_IO16(io + 0x054);
    e->masterBright = NTR_IO16(io + 0x06c);
    e->bgVram = src->bgVram;
    e->bgVramSize = src->bgVramSize;
    e->objVram = src->objVram;
    e->objVramSize = src->objVramSize;
    e->bgExtPltt = src->bgExtPltt;
    e->objExtPltt = src->objExtPltt;
    e->bgPltt = src->bgPltt;
    e->objPltt = src->objPltt;
    e->oam = src->oam;
}

#define W 256
#define H 192
#define TRANSPARENT 0x8000u /* bit 15 marks "no pixel" in the layer buffers */

enum { BGTYPE_OFF, BGTYPE_TEXT, BGTYPE_AFFINE, BGTYPE_EXTENDED, BGTYPE_3D };

static int BgType(const Eng *e, int bg, BOOL isMain)
{
    int mode = e->dispcnt & 7;

    if (bg == 0 && isMain && (e->dispcnt & 8)) {
        return BGTYPE_3D; /* BG0 shows the 3D engine's output */
    }
    if (bg < 2) {
        return mode == 6 ? BGTYPE_OFF : BGTYPE_TEXT;
    }
    static const int bg2[7] = { BGTYPE_TEXT, BGTYPE_TEXT, BGTYPE_AFFINE, BGTYPE_TEXT, BGTYPE_AFFINE, BGTYPE_EXTENDED, BGTYPE_OFF };
    static const int bg3[7] = { BGTYPE_TEXT, BGTYPE_AFFINE, BGTYPE_AFFINE, BGTYPE_EXTENDED, BGTYPE_EXTENDED, BGTYPE_EXTENDED, BGTYPE_OFF };
    return bg == 2 ? bg2[mode] : bg3[mode];
}

static inline u8 VramByte(const Eng *e, u32 addr)
{
    return addr < e->bgVramSize ? e->bgVram[addr] : 0;
}

static inline u16 VramHalf(const Eng *e, u32 addr)
{
    return addr + 1 < e->bgVramSize ? (u16)(e->bgVram[addr] | (e->bgVram[addr + 1] << 8)) : 0;
}

static inline u16 ExtColor(const Eng *e, int slot, int pal, int idx)
{
    if (e->bgExtPltt == NULL) {
        return e->bgPltt[idx];
    }
    const u8 *p = e->bgExtPltt + slot * 0x2000 + pal * 0x200 + idx * 2;
    return (u16)(p[0] | (p[1] << 8));
}

/* Output pixel formats: RGBA8888 (u32) or RGBA5551 (u16, the 3DS GPU's). */
static BOOL sOut16;

static inline u16 To5551(u16 c)
{
    return (u16)(((c & 0x1f) << 11) | ((c & 0x3e0) << 1) | ((c >> 9) & 0x3e) | 1);
}

/* Palette lookups for one frame's BG lines, in the format the lines are
 * written in: RGB555 with bit 15 for "no pixel" (the software compositor's)
 * or RGBA5551 with alpha 0 (GPU composition). Entries are 32-bit so a lookup
 * is one load with a scaled index; index 0 of every 16-color palette and of
 * the 256-color palette holds the transparent value. */
typedef struct LineFormat {
    u32 pal4[256];
    u32 pal8[256];
    u32 ext[4][16 * 256]; /* extended palette slots the frame's 256-color BGs use */
    u16 clear;
    BOOL rgba5551;
} LineFormat;

static void BuildLineFormat(const Eng *e, LineFormat *f, BOOL rgba5551)
{
    f->rgba5551 = rgba5551;
    f->clear = rgba5551 ? 0 : TRANSPARENT;
    for (int i = 0; i < 256; i++) {
        u16 c = e->bgPltt[i] & 0x7fff;
        u32 v = rgba5551 ? To5551(c) : c;
        f->pal8[i] = i == 0 ? f->clear : v;
        f->pal4[i] = (i & 15) == 0 ? f->clear : v;
    }
    if (!(e->dispcnt & (1u << 30))) {
        return;
    }
    u32 slots = 0;
    int mode = e->dispcnt & 7;
    for (int bg = 0; bg < 4; bg++) {
        if ((e->dispcnt & (0x100u << bg)) && (e->bgcnt[bg] & 0x80)) {
            slots |= 1u << (bg < 2 && (e->bgcnt[bg] & (1 << 13)) ? bg + 2 : bg);
        }
        /* Extended BGs with 16-bit map entries use their own slot. */
        BOOL extended = (bg == 2 && mode == 5) || (bg == 3 && mode >= 3 && mode <= 5);
        if ((e->dispcnt & (0x100u << bg)) && extended && !(e->bgcnt[bg] & 0x80)) {
            slots |= 1u << bg;
        }
    }
    for (int slot = 0; slot < 4; slot++) {
        if (slots & (1u << slot)) {
            for (int i = 0; i < 16 * 256; i++) {
                u16 c = ExtColor(e, slot, i >> 8, i & 255) & 0x7fff;
                f->ext[slot][i] = (i & 255) == 0 ? f->clear : rgba5551 ? To5551(c) : c;
            }
        }
    }
}

static inline u16 FormatColor(const LineFormat *f, u16 c)
{
    return f->rgba5551 ? To5551(c & 0x7fff) : (u16)(c & 0x7fff);
}

/* Eight pixels of a 4bpp tile row, from the row's 32 bits. */
#define TILE4(o, p, bits, sh)                                                                                \
    do {                                                                                                     \
        (o)[0] = (u16)(p)[((bits) >> sh(0)) & 15], (o)[1] = (u16)(p)[((bits) >> sh(1)) & 15];               \
        (o)[2] = (u16)(p)[((bits) >> sh(2)) & 15], (o)[3] = (u16)(p)[((bits) >> sh(3)) & 15];               \
        (o)[4] = (u16)(p)[((bits) >> sh(4)) & 15], (o)[5] = (u16)(p)[((bits) >> sh(5)) & 15];               \
        (o)[6] = (u16)(p)[((bits) >> sh(6)) & 15], (o)[7] = (u16)(p)[((bits) >> sh(7)) & 15];               \
    } while (0)
#define SHIFT_FWD(i) ((i) * 4)
#define SHIFT_FLIP(i) (28 - (i) * 4)

/* Pixels x0 .. x0 + count - 1 of line y (DS coordinates; outside the picture
 * the BG map continues, wrapping as the hardware does). Returns whether any
 * pixel is opaque. */
static BOOL RenderTextLine(const Eng *e, int bg, BOOL isMain, int y, u16 *out, const LineFormat *f, int x0, int count)
{
    u32 nonzero = 0;
    u16 cnt = e->bgcnt[bg];
    int size = cnt >> 14;
    int width = (size & 1) ? 512 : 256;
    int height = (size & 2) ? 512 : 256;
    BOOL is256 = (cnt >> 7) & 1;
    u32 scr = ((cnt >> 8) & 0x1f) * 0x800;
    u32 chr = ((cnt >> 2) & 0xf) * 0x4000;
    if (isMain) {
        scr += ((e->dispcnt >> 27) & 7) * 0x10000;
        chr += ((e->dispcnt >> 24) & 7) * 0x10000;
    }
    BOOL extPltt = is256 && (e->dispcnt & (1u << 30));
    int slot = bg;
    if (bg < 2 && (cnt & (1 << 13))) {
        slot += 2;
    }
    u16 clear = f->clear;

    int sy = (y + e->bgvofs[bg]) & (height - 1);
    u32 rowBase = scr + ((sy >> 8) * (width >> 8)) * 0x800 + ((sy & 255) >> 3) * 64;
    int sx = (x0 + e->bghofs[bg]) & (width - 1);

    /* Decode one map entry per 8-pixel tile column rather than per pixel. */
    for (int x = 0; x < count;) {
        u16 entry = VramHalf(e, rowBase + (sx >> 8) * 0x800 + ((sx & 255) >> 3) * 2);
        int tile = entry & 0x3ff;
        int py = (entry & 0x800) ? 7 - (sy & 7) : (sy & 7);
        int pal = entry >> 12;
        BOOL hflip = (entry & 0x400) != 0;
        int run = 8 - (sx & 7);
        if (run > count - x) {
            run = count - x;
        }
        u16 *o = out + x;

        if (is256) {
            u32 base = chr + tile * 64 + py * 8;
            const u8 *row = base + 8 <= e->bgVramSize ? e->bgVram + base : NULL;
            const u32 *p8 = extPltt ? &f->ext[slot][pal * 256] : f->pal8;
            if (row == NULL) {
                for (int i = 0; i < run; i++) {
                    o[i] = clear;
                }
            } else if (run == 8) {
                u32 w0, w1;
                memcpy(&w0, row, 4);
                memcpy(&w1, row + 4, 4);
                nonzero |= w0 | w1;
                if (hflip) {
                    for (int i = 0; i < 8; i++) {
                        o[i] = (u16)p8[row[7 - i]];
                    }
                } else {
                    for (int i = 0; i < 8; i++) {
                        o[i] = (u16)p8[row[i]];
                    }
                }
            } else {
                for (int i = 0; i < run; i++) {
                    int px = hflip ? 7 - ((sx + i) & 7) : ((sx + i) & 7);
                    nonzero |= row[px];
                    o[i] = (u16)p8[row[px]];
                }
            }
        } else {
            u32 base = chr + tile * 32 + py * 4;
            const u8 *row = base + 4 <= e->bgVramSize ? e->bgVram + base : NULL;
            const u32 *p4 = &f->pal4[pal * 16];
            if (row == NULL) {
                for (int i = 0; i < run; i++) {
                    o[i] = clear;
                }
            } else if (run == 8) {
                /* A whole tile row: eight 4-bit pixels in one word. */
                u32 bits;
                memcpy(&bits, row, 4);
                nonzero |= bits;
                if (bits == 0) {
                    for (int i = 0; i < 8; i++) {
                        o[i] = clear;
                    }
                } else if (hflip) {
                    TILE4(o, p4, bits, SHIFT_FLIP);
                } else {
                    TILE4(o, p4, bits, SHIFT_FWD);
                }
            } else {
                for (int i = 0; i < run; i++) {
                    int px = hflip ? 7 - ((sx + i) & 7) : ((sx + i) & 7);
                    int idx = (row[px >> 1] >> ((px & 1) * 4)) & 0xf;
                    nonzero |= idx;
                    o[i] = (u16)p4[idx];
                }
            }
        }
        x += run;
        sx = (sx + run) & (width - 1);
    }
    return nonzero != 0;
}

/* Pixels x0 .. x0 + count - 1 of line y, in RGB555 with TRANSPARENT. */
static void RenderAffineLine(const Eng *e, int bg, BOOL isMain, int y, BOOL extended, u16 *out, int x0, int count)
{
    u16 cnt = e->bgcnt[bg];
    int i = bg - 2;
    s32 rx = e->bgx[i] + e->bgpb[i] * y + e->bgpa[i] * x0;
    s32 ry = e->bgy[i] + e->bgpd[i] * y + e->bgpc[i] * x0;
    BOOL wrap = (cnt >> 13) & 1;
    u32 scr = ((cnt >> 8) & 0x1f) * 0x800;
    u32 chr = ((cnt >> 2) & 0xf) * 0x4000;
    if (isMain) {
        scr += ((e->dispcnt >> 27) & 7) * 0x10000;
        chr += ((e->dispcnt >> 24) & 7) * 0x10000;
    }

    int kind = 0; /* 0 = affine, 1 = 256x16 pltt, 2 = 256 bitmap, 3 = direct color bitmap */
    int width, height;
    if (!extended) {
        width = height = 128 << (cnt >> 14);
    } else if (!(cnt & 0x80)) {
        kind = 1;
        width = height = 128 << (cnt >> 14);
    } else {
        static const int bw[4] = { 128, 256, 512, 512 };
        static const int bh[4] = { 128, 256, 256, 512 };
        kind = (cnt & 4) ? 3 : 2;
        width = bw[cnt >> 14];
        height = bh[cnt >> 14];
        scr = ((cnt >> 8) & 0x1f) * 0x4000;
    }
    BOOL extPltt = (e->dispcnt & (1u << 30)) != 0;

    for (int x = 0; x < count; x++, rx += e->bgpa[i], ry += e->bgpc[i]) {
        int sx = rx >> 8;
        int sy = ry >> 8;
        if (wrap && kind < 2) {
            sx &= width - 1;
            sy &= height - 1;
        } else if (sx < 0 || sy < 0 || sx >= width || sy >= height) {
            out[x] = TRANSPARENT;
            continue;
        }

        u16 color = TRANSPARENT;
        if (kind == 0) {
            int tile = VramByte(e, scr + (sy >> 3) * (width >> 3) + (sx >> 3));
            int idx = VramByte(e, chr + tile * 64 + (sy & 7) * 8 + (sx & 7));
            if (idx) {
                color = e->bgPltt[idx] & 0x7fff;
            }
        } else if (kind == 1) {
            u16 entry = VramHalf(e, scr + ((sy >> 3) * (width >> 3) + (sx >> 3)) * 2);
            int px = (entry & 0x400) ? 7 - (sx & 7) : (sx & 7);
            int py = (entry & 0x800) ? 7 - (sy & 7) : (sy & 7);
            int idx = VramByte(e, chr + (entry & 0x3ff) * 64 + py * 8 + px);
            if (idx) {
                color = (extPltt ? ExtColor(e, bg, entry >> 12, idx) : e->bgPltt[idx]) & 0x7fff;
            }
        } else if (kind == 2) {
            int idx = VramByte(e, scr + sy * width + sx);
            if (idx) {
                color = e->bgPltt[idx] & 0x7fff;
            }
        } else {
            u16 c = VramHalf(e, scr + (sy * width + sx) * 2);
            if (c & 0x8000) {
                color = c & 0x7fff;
            }
        }
        out[x] = color;
    }
}

static inline u16 Brighten(u16 c, int evy)
{
    int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    r += ((31 - r) * evy) >> 4;
    g += ((31 - g) * evy) >> 4;
    b += ((31 - b) * evy) >> 4;
    return (u16)(r | (g << 5) | (b << 10));
}

static inline u16 Darken(u16 c, int evy)
{
    int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    r -= (r * evy) >> 4;
    g -= (g * evy) >> 4;
    b -= (b * evy) >> 4;
    return (u16)(r | (g << 5) | (b << 10));
}

static inline u16 Alpha(u16 a, u16 b, int eva, int evb)
{
    int r = ((a & 31) * eva + (b & 31) * evb) >> 4;
    int g = (((a >> 5) & 31) * eva + ((b >> 5) & 31) * evb) >> 4;
    int bl = (((a >> 10) & 31) * eva + ((b >> 10) & 31) * evb) >> 4;
    if (r > 31) r = 31;
    if (g > 31) g = 31;
    if (bl > 31) bl = 31;
    return (u16)(r | (g << 5) | (bl << 10));
}

/* A 3D pixel over the layer below it, weighted by its own alpha (0-30):
 * (a * (alpha + 1) + b * (31 - alpha)) / 32. */
static inline u16 Alpha3D(u16 a, u16 b, int alpha)
{
    int eva = alpha + 1, evb = 31 - alpha;
    int r = ((a & 31) * eva + (b & 31) * evb) >> 5;
    int g = (((a >> 5) & 31) * eva + ((b >> 5) & 31) * evb) >> 5;
    int bl = (((a >> 10) & 31) * eva + ((b >> 10) & 31) * evb) >> 5;
    return (u16)(r | (g << 5) | (bl << 10));
}

/* RGB555 -> RGBA8888, filled on first use. */
static u32 sRGBA[0x8000];

static void InitRGBA(void);

static inline u32 ToRGBA(u16 c)
{
    u32 r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    r = (r << 3) | (r >> 2);
    g = (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);
    return (r << 24) | (g << 16) | (b << 8) | 0xff;
}

/* Sprites ------------------------------------------------------------------- */

#define OBJ_NONE 0xff

/* One line of sprites across the whole output; DS x is at index x + ofs.
 * Sprites are drawn for DS x in [xMin, xMax), so beside the DS picture too. */
typedef struct ObjLine {
    u16 color[NTR_MAX_SCREEN_W];
    u8 prio[NTR_MAX_SCREEN_W]; /* OBJ_NONE = no sprite pixel */
    u8 semi[NTR_MAX_SCREEN_W]; /* semi-transparent sprite pixel */
    u8 window[NTR_MAX_SCREEN_W]; /* covered by an OBJ-window sprite */
    int ofs, xMin, xMax;
    int lo, hi; /* the range of indices written (lo > hi: none) */
    u8 prioMask; /* priorities with sprite pixels on this line */
    BOOL dirty; /* any pixel written since the arrays were cleared */
} ObjLine;

static const u8 sObjSize[3][4][2] = {
    { { 8, 8 }, { 16, 16 }, { 32, 32 }, { 64, 64 } },
    { { 16, 8 }, { 32, 8 }, { 32, 16 }, { 64, 32 } },
    { { 8, 16 }, { 8, 32 }, { 16, 32 }, { 32, 64 } },
};

static inline u8 ObjByte(const Eng *e, u32 addr)
{
    return addr < e->objVramSize ? e->objVram[addr] : 0;
}

/* A sprite's attributes, decoded once per frame. */
typedef struct ObjInfo {
    s16 ox; /* -256 .. 255 */
    u8 oy, w, h, boundW, boundH;
    u8 prio, mode, is256, affine, hflip, vflip;
    u16 tile;
    const u16 *palette; /* 4bpp: the sprite's 16 colors; 8bpp: the 256-color palette in use */
    s16 pa, pb, pc, pd;
} ObjInfo;

/* The sprites that can appear this frame (enabled, drawable), and which lines
 * have any, found once per frame instead of on every line. */
typedef struct ObjList {
    u8 index[128];
    ObjInfo info[128];
    int count;
    u8 lineHasObj[256];
} ObjList;

static void FindSprites(const Eng *e, ObjList *list)
{
    BOOL extPltt = (e->dispcnt & (1u << 31)) != 0;
    list->count = 0;
    memset(list->lineHasObj, 0, sizeof(list->lineHasObj));
    for (int i = 0; i < 128; i++) {
        const u8 *o = &e->oam[i * 8];
        u16 a0 = (u16)(o[0] | o[1] << 8);
        u16 a1 = (u16)(o[2] | o[3] << 8);
        u16 a2 = (u16)(o[4] | o[5] << 8);
        BOOL affine = (a0 >> 8) & 1;
        int mode = (a0 >> 10) & 3, shape = a0 >> 14;
        if ((!affine && (a0 & 0x200)) || shape == 3 || mode == 3) {
            continue;
        }
        int w = sObjSize[shape][a1 >> 14][0], h = sObjSize[shape][a1 >> 14][1];
        int boundW = w, boundH = h;
        if (affine && (a0 & 0x200)) {
            boundW *= 2;
            boundH *= 2;
        }
        int oy = a0 & 0xff;
        if (oy >= H && oy + boundH <= 256) {
            /* Wholly below the picture without wrapping to its top: the DS
             * shows none of it, and neither do the rows beside the picture
             * (RenderObjLine). Games park unused sprites there. */
            continue;
        }
        for (int ly = 0; ly < boundH; ly++) {
            list->lineHasObj[(oy + ly) & 0xff] = 1;
        }
        ObjInfo *n = &list->info[list->count];
        int ox = a1 & 0x1ff;
        n->ox = (s16)(ox >= 256 ? ox - 512 : ox);
        n->oy = (u8)oy;
        n->w = (u8)w, n->h = (u8)h, n->boundW = (u8)boundW, n->boundH = (u8)boundH;
        n->prio = (u8)((a2 >> 10) & 3);
        n->mode = (u8)mode;
        n->is256 = (a0 >> 13) & 1;
        n->affine = (u8)affine;
        n->hflip = !affine && (a1 & 0x1000);
        n->vflip = !affine && (a1 & 0x2000);
        n->tile = a2 & 0x3ff;
        int pal = a2 >> 12;
        n->palette = !n->is256 ? e->objPltt + pal * 16
            : (extPltt && e->objExtPltt) ? (const u16 *)(const void *)(e->objExtPltt + pal * 0x200)
                                         : e->objPltt;
        n->pa = 256, n->pb = 0, n->pc = 0, n->pd = 256;
        if (affine) {
            const u8 *p = &e->oam[((a1 >> 9) & 0x1f) * 32];
            n->pa = (s16)(p[6] | p[7] << 8);
            n->pb = (s16)(p[14] | p[15] << 8);
            n->pc = (s16)(p[22] | p[23] << 8);
            n->pd = (s16)(p[30] | p[31] << 8);
        }
        list->index[list->count++] = (u8)i;
#ifdef NTR_DEBUG_HOOKS
        if (getenv("PORT_OAM")) {
            printf("frame %u oam %d: x %d y %d shape %d size %d affine %d mode %d tile %d prio %d\n", Ntr_FramesEnded(), i, a1 & 0x1ff, oy,
                shape, a1 >> 14, affine, mode, a2 & 0x3ff, (a2 >> 10) & 3);
        }
#endif
    }
}

static void ClearObjLine(ObjLine *line)
{
    if (line->dirty) {
        memset(line->prio, OBJ_NONE, sizeof(line->prio));
        memset(line->semi, 0, sizeof(line->semi));
        memset(line->window, 0, sizeof(line->window));
        line->dirty = FALSE;
    }
    line->prioMask = 0;
    line->lo = NTR_MAX_SCREEN_W;
    line->hi = -1;
}

/* A sprite pixel at output index dx: lower priority values win, and on a tie
 * the lower OAM index (drawn first) stays. */
static inline void PutObjPixel(ObjLine *line, const ObjInfo *n, int dx, u16 color)
{
    if (n->mode == 2) {
        line->window[dx] = 1;
        return;
    }
    if (line->prio[dx] <= n->prio) { /* (OBJ_NONE is above every priority) */
        return;
    }
    line->color[dx] = color & 0x7fff;
    line->prio[dx] = n->prio;
    line->semi[dx] = n->mode == 1;
    line->prioMask |= (u8)(1 << n->prio);
    line->lo = dx < line->lo ? dx : line->lo;
    line->hi = dx > line->hi ? dx : line->hi;
}

static void RenderObjLine(const Eng *e, int y, ObjLine *line, const ObjList *list)
{
    ClearObjLine(line);
    if (e->objVram == NULL || !list->lineHasObj[y & 0xff]) {
        return;
    }
    line->dirty = TRUE;

    BOOL map1D = (e->dispcnt & 0x10) != 0;
    u32 boundaryShift = 5 + ((e->dispcnt >> 20) & 3);

    for (int k = 0; k < list->count; k++) {
        const ObjInfo *n = &list->info[k];
        int ly = (y - n->oy) & 0xff;
        if (ly >= n->boundH) {
            continue;
        }
        if ((y < 0 || y >= H) && n->oy >= H && n->oy + n->boundH <= 256) {
            continue; /* above or below the picture, and not in it at all */
        }
        int w = n->w, h = n->h;
        BOOL is256 = n->is256;
        const u16 *palette = n->palette;

        /* X is 9 bits and wraps: beside the DS picture a sprite can show at
         * either position. */
        if (!n->affine) {
            /* Unrotated: one tile row lookup per 8 pixels. */
            int ty = n->vflip ? h - 1 - ly : ly;
            BOOL hflip = n->hflip;
            u32 rowBytes = is256 ? 8 : 4;
            for (int copy = 0; copy < 2; copy++) {
                int bx = n->ox + copy * 512;
                int lx = line->xMin - bx > 0 ? line->xMin - bx : 0;
                int lx1 = line->xMax - bx < w ? line->xMax - bx : w;
                while (lx < lx1) {
                    int tx = hflip ? w - 1 - lx : lx;
                    u32 addr;
                    if (map1D) {
                        addr = ((u32)n->tile << boundaryShift) + (u32)((ty >> 3) * (w >> 3) + (tx >> 3)) * rowBytes * 8;
                    } else {
                        addr = (u32)(n->tile + (ty >> 3) * 32 + (tx >> 3) * (is256 ? 2 : 1)) * 32;
                    }
                    addr += (u32)(ty & 7) * rowBytes;
                    const u8 *row = addr + rowBytes <= e->objVramSize ? e->objVram + addr : NULL;
                    int cnt = hflip ? (tx & 7) + 1 : 8 - (tx & 7);
                    if (cnt > lx1 - lx) {
                        cnt = lx1 - lx;
                    }
                    if (row == NULL) {
                        lx += cnt; /* color 0: nothing drawn */
                        continue;
                    }
                    int dx0 = bx + lx + line->ofs;
                    if (cnt == 8 && !is256) {
                        /* A whole 4bpp tile row: eight pixels in one word. */
                        u32 bits;
                        memcpy(&bits, row, 4);
                        for (int i = 0; bits != 0 && i < 8; i++) {
                            int ci = hflip ? (int)(bits >> 28) : (int)(bits & 15);
                            bits = hflip ? bits << 4 : bits >> 4;
                            if (ci != 0) {
                                PutObjPixel(line, n, dx0 + i, palette[ci]);
                            }
                        }
                        lx += 8;
                        continue;
                    }
                    if (cnt == 8) {
                        u32 bits0, bits1;
                        memcpy(&bits0, row, 4);
                        memcpy(&bits1, row + 4, 4);
                        if ((bits0 | bits1) == 0) {
                            lx += 8; /* a transparent tile row */
                            continue;
                        }
                    }
                    for (int i = 0; i < cnt; i++, lx++) {
                        int px = hflip ? (tx - i) & 7 : (tx + i) & 7;
                        int ci = is256 ? row[px] : (row[px >> 1] >> ((px & 1) * 4)) & 0xf;
                        if (ci != 0) {
                            PutObjPixel(line, n, dx0 + i, palette[ci]);
                        }
                    }
                }
            }
            continue;
        }
        int boundW = n->boundW, boundH = n->boundH;
        for (int copy = 0; copy < 2; copy++) {
            int bx = n->ox + copy * 512;
            int lx0 = line->xMin - bx > 0 ? line->xMin - bx : 0;
            int lx1 = line->xMax - bx < boundW ? line->xMax - bx : boundW;
            int cy = ly - boundH / 2;
            /* The texel position in 8.8 fixed point, stepped along the line:
             * ((pa * cx + pb * cy) >> 8) + w / 2 for each pixel's cx. */
            s32 sx8 = n->pa * (lx0 - boundW / 2) + n->pb * cy + (w / 2) * 256;
            s32 sy8 = n->pc * (lx0 - boundW / 2) + n->pd * cy + (h / 2) * 256;
            for (int lx = lx0; lx < lx1; lx++, sx8 += n->pa, sy8 += n->pc) {
                int tx = sx8 >> 8, ty = sy8 >> 8;
                if (tx < 0 || ty < 0 || tx >= w || ty >= h) {
                    continue;
                }
                u32 addr;
                int px = tx & 7, py = ty & 7;
                if (map1D) {
                    u32 tileBytes = is256 ? 64 : 32;
                    u32 tileIdx = (u32)((ty >> 3) * (w >> 3) + (tx >> 3));
                    addr = ((u32)n->tile << boundaryShift) + tileIdx * tileBytes;
                } else {
                    u32 idx = is256 ? (u32)(n->tile + (ty >> 3) * 32 + (tx >> 3) * 2) : (u32)(n->tile + (ty >> 3) * 32 + (tx >> 3));
                    addr = idx * 32;
                }
                int ci;
                if (is256) {
                    ci = ObjByte(e, addr + py * 8 + px);
                } else {
                    u8 b = ObjByte(e, addr + py * 4 + (px >> 1));
                    ci = (px & 1) ? b >> 4 : b & 0xf;
                }
                if (ci != 0) {
                    PutObjPixel(line, n, bx + lx + line->ofs, palette[ci]);
                }
            }
        }
    }
}

/* Windows -------------------------------------------------------------------- */

static inline BOOL InWindowRange(int v, int lo, int hi)
{
    return lo <= hi ? (v >= lo && v < hi) : (v >= lo || v < hi);
}

/* Sets a window's control bits on its DS columns [lo, hi), wrapping when
 * lo > hi as the hardware does. */
/* Widescreen: a window spanning the whole DS picture one way (as games set
 * up bands across the screen) spans the whole output that way. */
static inline BOOL WindowFull(int lo, int hi, int size)
{
    return lo == 0 && hi >= size - 1;
}

static void FillWindowSpan(u8 *win, int lo, int hi, u8 control);

/* WIN1, then WIN0 over it, on an output row (DS line y) whose outside and
 * OBJ window control is filled in already. */
static void FillWindows(const Eng *e, u8 *win, int y, int ox, int outW)
{
    u32 windows = (e->dispcnt >> 13) & 3;
    for (int k = 1; k >= 0; k--) {
        if (!(windows & (1u << k))) {
            continue;
        }
        const u8 *r = k ? e->win1 : e->win0;
        u8 control = (u8)((e->winin >> (k * 8)) & 0x3f);
        BOOL fullY = WindowFull(r[2], r[3], 192);
        if (!(fullY || (y >= 0 && y < 192 && InWindowRange(y, r[2], r[3])))) {
            continue;
        }
        if (WindowFull(r[0], r[1], 256)) {
            memset(win, control, (size_t)outW);
        } else {
            FillWindowSpan(win + ox, r[0], r[1], control);
        }
    }
}

static void FillWindowSpan(u8 *win, int lo, int hi, u8 control)
{
    if (lo <= hi) {
        memset(win + lo, control, (size_t)(hi - lo));
    } else {
        memset(win + lo, control, (size_t)(W - lo));
        memset(win, control, (size_t)hi);
    }
}


/* Widescreen: BG layers beyond the DS picture -------------------------------------
 *
 * Each BG layer is decoded into an image of the whole output with the DS
 * picture in the middle. Beside it, per side:
 *  - a background (a layer that fills at least half of its edge on that side,
 *    or a band reaching the opposite edge too) that scrolls continues its own
 *    map, so the world goes on;
 *  - a still background is widened in the outermost run of at least a tile's
 *    width of identical columns there, so plain areas and vertical gradients
 *    just get wider: what lies beyond (edge decoration) moves to the screen's
 *    edge while the middle stays centered with the other layers. Without
 *    such a run it continues its map if that is wider than the screen, else
 *    repeats its edge column (its map would show the other side of the
 *    picture);
 *  - other layers (windows, text, pictures) end at the DS picture, scrolling
 *    or not: beyond it their maps hold other pictures (a row of characters to
 *    choose from, say).
 * Rows above and below are done the same way, except that a background
 * without identical rows repeats its edge row. Sprites are drawn beside the
 * picture, and above and below it when they reach into it (games park
 * unused sprites just below the screen). */

#define SCROLL_FRAMES 60 /* a layer whose scroll changed this recently is scrolling */

typedef struct ScrollTrack {
    BOOL valid;
    u16 cnt;
    s32 x, y;
    u32 xFrame, yFrame; /* when x and y last changed */
} ScrollTrack;

static ScrollTrack sScroll[2][4];

/* The last column decisions per layer: DecodeLayerRows decodes the sides that
 * continued their map along with the picture (on both cores), and the search
 * for a column to widen at is only redone every SEARCH_FRAMES frames, the
 * column found being checked every frame. */
#define SEARCH_FRAMES 16

enum { EXT_STRETCH, EXT_WRAP, EXT_NONE };

typedef struct ExtCache {
    BOOL valid;
    u8 mode[2]; /* left, right */
    s16 run[2]; /* what OuterRun returned */
    u32 searched[2];
} ExtCache;

static ExtCache sExtCache[2][4];

/* Once per frame and engine. */
static void TrackScroll(int engine, const Eng *e)
{
    u32 now = Ntr_FramesEnded();
    for (int bg = 0; bg < 4; bg++) {
        ScrollTrack *t = &sScroll[engine][bg];
        s32 x = e->bghofs[bg], y = e->bgvofs[bg];
        if (bg >= 2) {
            x ^= e->bgx[bg - 2] << 9;
            y ^= e->bgy[bg - 2] << 9;
        }
        if (!t->valid || t->cnt != e->bgcnt[bg]) {
            sExtCache[engine][bg].valid = FALSE;
            t->valid = TRUE;
            t->cnt = e->bgcnt[bg];
            t->xFrame = t->yFrame = now - SCROLL_FRAMES;
        } else {
            if (x != t->x) {
                t->xFrame = now;
            }
            if (y != t->y) {
                t->yFrame = now;
            }
        }
        t->x = x;
        t->y = y;
    }
}

/* DS columns x0 .. x0 + count - 1 of DS line v, in the line format. */
/* RenderAffineLine for a layer that is not rotated or scaled along the line
 * (PA = 1, PC = 0): texels in a row, so map entries are looked up once per
 * tile, and colors come from the line format directly. Returns whether any
 * pixel is opaque. */
static BOOL RenderAffineLineStraight(const Eng *e, int bg, BOOL isMain, int y, BOOL extended, const LineFormat *f, u16 *out, int x0,
    int count)
{
    u16 cnt = e->bgcnt[bg];
    int i = bg - 2;
    s32 rx = e->bgx[i] + e->bgpb[i] * y + e->bgpa[i] * x0;
    s32 ry = e->bgy[i] + e->bgpd[i] * y + e->bgpc[i] * x0;
    BOOL wrap = (cnt >> 13) & 1;
    u32 scr = ((cnt >> 8) & 0x1f) * 0x800;
    u32 chr = ((cnt >> 2) & 0xf) * 0x4000;
    if (isMain) {
        scr += ((e->dispcnt >> 27) & 7) * 0x10000;
        chr += ((e->dispcnt >> 24) & 7) * 0x10000;
    }
    int kind = 0;
    int width, height;
    if (!extended) {
        width = height = 128 << (cnt >> 14);
    } else if (!(cnt & 0x80)) {
        kind = 1;
        width = height = 128 << (cnt >> 14);
    } else {
        static const int bw[4] = { 128, 256, 512, 512 };
        static const int bh[4] = { 128, 256, 256, 512 };
        kind = (cnt & 4) ? 3 : 2;
        width = bw[cnt >> 14];
        height = bh[cnt >> 14];
        scr = ((cnt >> 8) & 0x1f) * 0x4000;
    }
    BOOL extPltt = (e->dispcnt & (1u << 30)) != 0;
    u16 clear = f->clear;
    BOOL wrapping = wrap && kind < 2;
    int sy = ry >> 8, sx = rx >> 8;
    if (wrapping) {
        sy &= height - 1;
    } else if (sy < 0 || sy >= height) {
        for (int x = 0; x < count; x++) {
            out[x] = clear;
        }
        return FALSE;
    }
    u32 any = 0;
    int lastTile = -1;
    u32 rowAddr = 0; /* kinds 0 and 1: the tile row of the current tile */
    const u32 *pal = f->pal8;
    BOOL flipX = FALSE;
    for (int x = 0; x < count; x++, sx++) {
        int tx = sx;
        if (wrapping) {
            tx &= width - 1;
        } else if (tx < 0 || tx >= width) {
            out[x] = clear;
            continue;
        }
        u16 o;
        if (kind == 0) {
            if ((tx >> 3) != lastTile) {
                lastTile = tx >> 3;
                int tile = VramByte(e, scr + (sy >> 3) * (width >> 3) + (tx >> 3));
                rowAddr = chr + tile * 64 + (sy & 7) * 8;
            }
            o = (u16)f->pal8[VramByte(e, rowAddr + (tx & 7))];
        } else if (kind == 1) {
            if ((tx >> 3) != lastTile) {
                lastTile = tx >> 3;
                u16 entry = VramHalf(e, scr + ((sy >> 3) * (width >> 3) + (tx >> 3)) * 2);
                int py = (entry & 0x800) ? 7 - (sy & 7) : (sy & 7);
                rowAddr = chr + (entry & 0x3ff) * 64 + py * 8;
                flipX = (entry & 0x400) != 0;
                pal = extPltt ? &f->ext[bg][(entry >> 12) * 256] : f->pal8;
            }
            o = (u16)pal[VramByte(e, rowAddr + (flipX ? 7 - (tx & 7) : (tx & 7)))];
        } else if (kind == 2) {
            o = (u16)f->pal8[VramByte(e, scr + sy * width + tx)];
        } else {
            u16 c = VramHalf(e, scr + (sy * width + tx) * 2);
            o = (c & 0x8000) ? FormatColor(f, c) : clear;
        }
        out[x] = o;
        any |= o != clear;
    }
    return any != 0;
}

static BOOL DecodeRun(const Eng *e, int bg, int type, BOOL isMain, const LineFormat *f, int v, int x0, int count, u16 *out, u16 *tmp)
{
    if (type == BGTYPE_TEXT) {
        return RenderTextLine(e, bg, isMain, v, out, f, x0, count);
    }
    if (e->bgpa[bg - 2] == 256 && e->bgpc[bg - 2] == 0) {
        return RenderAffineLineStraight(e, bg, isMain, v, type == BGTYPE_EXTENDED, f, out, x0, count);
    }
    RenderAffineLine(e, bg, isMain, v, type == BGTYPE_EXTENDED, tmp, x0, count);
    BOOL any = FALSE;
    for (int x = 0; x < count; x++) {
        u16 c = tmp[x];
        any |= !(c & TRANSPARENT);
        out[x] = f->rgba5551 ? ((c & TRANSPARENT) ? 0 : To5551(c)) : c;
    }
    return any;
}

/* Whether a layer scrolls, per axis: it then continues its map beside the
 * picture, decoded with the picture itself (DecodeLayerRows). */
static void LayerScrolling(int engine, int bg, BOOL *x, BOOL *y)
{
    const ScrollTrack *t = &sScroll[engine][bg];
    u32 now = Ntr_FramesEnded();
    *x = t->valid && now - t->xFrame < SCROLL_FRAMES;
    *y = t->valid && now - t->yFrame < SCROLL_FRAMES;
}

/* The sides of a layer that DecodeLayerRows decodes beyond the picture: all
 * of a horizontally scrolling layer, else the sides that continued their map
 * last frame. */
static void DecodedSides(int engine, int bg, BOOL scrollX, BOOL *left, BOOL *right)
{
    const ExtCache *c = &sExtCache[engine][bg];
    *left = scrollX || (c->valid && c->mode[0] == EXT_WRAP);
    *right = scrollX || (c->valid && c->mode[1] == EXT_WRAP);
}

/* Part `part` of `numParts` of a layer's rows into its output image: the DS
 * picture's lines, with the rows above and below when the layer scrolls
 * vertically, and the whole width when it scrolls horizontally (the rest is
 * ExtendLayer's). */
static BOOL DecodeLayerRows(int engine, const Eng *e, int bg, int type, BOOL isMain, const LineFormat *f, u16 *img, int stride, int outW,
    int outH, int part, int numParts, u16 *tmp)
{
    int ox = (outW - W) / 2, oy = (outH - H) / 2;
    BOOL scrollX, scrollY;
    LayerScrolling(engine, bg, &scrollX, &scrollY);
    int v0 = scrollY ? -oy : 0, v1 = scrollY ? H + oy : H;
    int n = v1 - v0;
    int a = v0 + n * part / numParts, b = v0 + n * (part + 1) / numParts;
    BOOL wrapL, wrapR;
#ifdef NTR_DEBUG_HOOKS
    if (getenv("PORT_NOCACHE")) {
        sExtCache[engine][bg].valid = FALSE;
    }
#endif
    DecodedSides(engine, bg, scrollX, &wrapL, &wrapR);
    int x0 = wrapL ? -ox : 0, x1 = wrapR ? W + ox : W;
    BOOL any = FALSE;
    for (int v = a; v < b; v++) {
        any |= DecodeRun(e, bg, type, isMain, f, v, x0, x1 - x0, img + (oy + v) * stride + ox + x0, tmp);
    }
    return any;
}

/* How a layer goes on beside the picture, per side (left, right, top,
 * bottom): widened at column s1 / s2 - 1 and row t1 / t2 - 1, its own map,
 * or not at all (the modes are with ExtCache). */

typedef struct LayerExt {
    u8 mode[4];
    int s1, s2, t1, t2;
} LayerExt;

/* How many of n pixels (step apart) are opaque. */
static int CountOpaque(const u16 *p, int n, int step, u16 clear)
{
    int opaque = 0;
    for (int i = 0; i < n; i++) {
        opaque += p[i * step] != clear;
    }
    return opaque;
}

/* How many of n pixels (step apart) are opaque in both a and b. */
static int CountOpaqueBoth(const u16 *a, const u16 *b, int n, int step, u16 clear)
{
    int opaque = 0;
    for (int i = 0; i < n; i++) {
        opaque += a[i * step] != clear && b[i * step] != clear;
    }
    return opaque;
}

/* Whether a layer is a background on one side, from its edge pixels: it
 * fills half the edge, or is a band reaching the opposite edge too. */
static BOOL EdgeIsBackground(int opaque, int both, int n)
{
    return opaque * 2 >= n || (both > 0 && both * 2 >= opaque);
}

static BOOL ColumnsMatch(const u16 *ds, int stride, int a, int b)
{
    for (int v = 0; v < H; v++) {
        if (ds[v * stride + a] != ds[v * stride + b]) {
            return FALSE;
        }
    }
    return TRUE;
}

/* A run of at least MIN_RUN identical columns (rows: byRows) among u0 .. u1
 * (each compared with the next), the one nearest the outer edge (u0 when
 * fromStart, else u1): returns a column inside it, or -1 if there is none. */
#define MIN_RUN 8

static int OuterRun(const u16 *ds, int stride, int rowLen, BOOL byRows, int u0, int u1, BOOL fromStart)
{
    int run = 0;
    for (int k = 0; k < u1 - u0; k++) {
        int u = fromStart ? u0 + k : u1 - 1 - k;
        BOOL same = byRows ? memcmp(ds + u * stride, ds + (u + 1) * stride, (size_t)rowLen * 2) == 0 : ColumnsMatch(ds, stride, u, u + 1);
        run = same ? run + 1 : 0;
        if (run >= MIN_RUN) {
            return u;
        }
    }
    return -1;
}

/* Whether the run OuterRun found at u (scanning from the start, else from
 * the end) still holds. */
static BOOL RunHolds(const u16 *ds, int stride, int u, BOOL fromStart)
{
    for (int k = 0; k < MIN_RUN; k++) {
        int a = fromStart ? u - k : u + k;
        if (!ColumnsMatch(ds, stride, a, a + 1)) {
            return FALSE;
        }
    }
    return TRUE;
}

/* The parts of an output row beside the DS picture, from the row's DS part. */
static BOOL ExtendRow(const Eng *e, int bg, int type, BOOL isMain, const LineFormat *f, const LayerExt *x, u16 *row, int ox, int v, u16 *tmp,
    BOOL decodedL, BOOL decodedR)
{
    BOOL any = FALSE;
    u16 *d = row + ox;
    u16 left = d[x->s1], right = d[x->s2 - 1];
    if (x->mode[1] == EXT_WRAP) {
        if (!decodedR) {
            any |= DecodeRun(e, bg, type, isMain, f, v, W, ox, d + W, tmp);
        }
    } else if (x->mode[1] == EXT_STRETCH) {
        memmove(row + 2 * ox + x->s2, d + x->s2, (size_t)(W - x->s2) * 2);
        for (int i = x->s2 + ox; i < x->s2 + 2 * ox; i++) {
            row[i] = right;
        }
    } else {
        for (int i = 0; i < ox; i++) {
            d[W + i] = f->clear;
        }
    }
    if (x->mode[0] == EXT_WRAP) {
        if (!decodedL) {
            any |= DecodeRun(e, bg, type, isMain, f, v, -ox, ox, row, tmp);
        }
    } else if (x->mode[0] == EXT_STRETCH) {
        memmove(row, d, (size_t)x->s1 * 2);
        for (int i = x->s1; i < x->s1 + ox; i++) {
            row[i] = left;
        }
    } else {
        for (int i = 0; i < ox; i++) {
            row[i] = f->clear;
        }
    }
    return any;
}

/* Fills the output image of a layer beside its decoded DS picture. Returns
 * whether that added opaque pixels. */
static BOOL ExtendLayer(int engine, const Eng *e, int bg, int type, BOOL isMain, const LineFormat *f, u16 *img, int stride, int outW,
    int outH, u16 *tmp)
{
    int ox = (outW - W) / 2, oy = (outH - H) / 2;
    if (ox <= 0 && oy <= 0) {
        return FALSE;
    }
    BOOL any = FALSE;
    u16 *ds = img + oy * stride + ox;
    BOOL scrollX, scrollY;
    LayerScrolling(engine, bg, &scrollX, &scrollY);
    int r0 = scrollY ? -oy : 0, r1 = scrollY ? H + oy : H; /* the lines DecodeLayerRows did */

    LayerExt x = { { EXT_WRAP, EXT_WRAP, EXT_WRAP, EXT_WRAP }, 0, W, 0, H };
    u16 clear = f->clear;
    ExtCache *cache = &sExtCache[engine][bg];
#ifdef NTR_DEBUG_HOOKS
    if (getenv("PORT_NOCACHE")) {
        cache->valid = FALSE; /* (DecodeLayerRows saw it as invalid too) */
    }
#endif
    BOOL decodedL, decodedR;
    DecodedSides(engine, bg, scrollX, &decodedL, &decodedR);
    u32 now = Ntr_FramesEnded();
    BOOL wideMap = type == BGTYPE_TEXT && ((e->bgcnt[bg] >> 14) & 1);
    u64 e0 = STAGE_TICKS();
    if (ox > 0) {
        {
            int both = CountOpaqueBoth(ds, ds + W - 1, H, stride, clear);
            for (int side = 0; side < 2; side++) {
                BOOL background = EdgeIsBackground(CountOpaque(ds + (side ? W - 1 : 0), H, stride, clear), both, H);
                if (!background || scrollX) {
                    /* (a scrolling layer was decoded across the whole width) */
                    x.mode[side] = background ? EXT_WRAP : EXT_NONE;
                    /* No run searched for: the next search is due at once
                     * (a run kept from before, or none ever found, would
                     * stretch the picture at the wrong column). */
                    cache->searched[side] = now - SEARCH_FRAMES;
                    continue;
                }
                /* Without a run: its map if wider than the screen, else the edge column. */
                x.mode[side] = wideMap ? EXT_WRAP : EXT_STRETCH;
                int u;
                if (cache->valid && now - cache->searched[side] < SEARCH_FRAMES
                    && (cache->run[side] < 0 || RunHolds(ds, stride, cache->run[side], side == 0))) {
                    u = cache->run[side];
                } else {
                    u = side == 0 ? OuterRun(ds, stride, 0, FALSE, 0, W / 2, TRUE) : OuterRun(ds, stride, 0, FALSE, W / 2, W - 1, FALSE);
                    cache->searched[side] = now;
                }
                cache->run[side] = (s16)u;
                if (u >= 0) {
                    x.mode[side] = EXT_STRETCH;
                    if (side == 0) {
                        x.s1 = u;
                    } else {
                        x.s2 = u + 1;
                    }
                }
            }
        }
        if (!scrollX) {
            cache->mode[0] = x.mode[0];
            cache->mode[1] = x.mode[1];
            cache->valid = TRUE;
        }
        u64 e1 = STAGE_TICKS();
        gNtrExtTicks[0] += e1 - e0;
        for (int v = r0; v < r1; v++) {
            any |= ExtendRow(e, bg, type, isMain, f, &x, img + (oy + v) * stride, ox, v, tmp, decodedL, decodedR);
        }
        gNtrExtTicks[1] += STAGE_TICKS() - e1;
    }
#ifdef NTR_DEBUG_HOOKS
    if (getenv("PORT_EXT")) {
        static const char *names[] = { "stretch", "wrap", "none" };
        printf("frame %u engine %d BG%d type %d: scroll %d/%d  left %s %d right %s %d (top/bottom decided below)\n", Ntr_FramesEnded(), engine, bg,
            type, scrollX, scrollY, names[x.mode[0]], x.s1, names[x.mode[1]], x.s2);
    }
#endif
    if (oy <= 0) {
        return any;
    }
    u64 e2 = STAGE_TICKS();
    size_t rowBytes = (size_t)outW * 2;
    if (scrollY) {
        /* Decoded above and below already; only pictures end there. */
        int both = CountOpaqueBoth(ds, ds + (H - 1) * stride, W, 1, clear);
        for (int side = 0; side < 2; side++) {
            if (!EdgeIsBackground(CountOpaque(ds + (side ? (H - 1) * stride : 0), W, 1, clear), both, W)) {
                for (int y = side ? oy + H : 0; y < (side ? outH : oy); y++) {
                    for (int i = 0; i < outW; i++) {
                        img[y * stride + i] = clear;
                    }
                }
            }
        }
        gNtrExtTicks[2] += STAGE_TICKS() - e2;
        return any;
    }
    if (!scrollY) {
        /* Backgrounds without identical rows repeat the edge row. */
        int both = CountOpaqueBoth(ds, ds + (H - 1) * stride, W, 1, clear);
        x.mode[2] = EdgeIsBackground(CountOpaque(ds, W, 1, clear), both, W) ? EXT_STRETCH : EXT_NONE;
        x.mode[3] = EdgeIsBackground(CountOpaque(ds + (H - 1) * stride, W, 1, clear), both, W) ? EXT_STRETCH : EXT_NONE;
        int v = OuterRun(img + oy * stride, stride, outW, TRUE, 0, H / 2, TRUE);
        x.t1 = v < 0 ? 0 : v;
        v = OuterRun(img + oy * stride, stride, outW, TRUE, H / 2, H - 1, FALSE);
        x.t2 = v < 0 ? H : v + 1;
    }
    if (x.mode[3] == EXT_WRAP) {
        for (int v = H; v < H + oy; v++) {
            u16 *row = img + (oy + v) * stride;
            if (scrollX) {
                any |= DecodeRun(e, bg, type, isMain, f, v, -ox, outW, row, tmp);
            } else {
                any |= DecodeRun(e, bg, type, isMain, f, v, 0, W, row + ox, tmp);
                any |= ExtendRow(e, bg, type, isMain, f, &x, row, ox, v, tmp, FALSE, FALSE);
            }
        }
    } else if (x.mode[3] == EXT_STRETCH) {
        for (int v = H - 1; v >= x.t2; v--) {
            memcpy(img + (2 * oy + v) * stride, img + (oy + v) * stride, rowBytes);
        }
        const u16 *band = img + (oy + x.t2 - 1) * stride;
        for (int y = x.t2 + oy; y < x.t2 + 2 * oy; y++) {
            memcpy(img + y * stride, band, rowBytes);
        }
    } else {
        for (int y = oy + H; y < outH; y++) {
            for (int i = 0; i < outW; i++) {
                img[y * stride + i] = clear;
            }
        }
    }
    if (x.mode[2] == EXT_WRAP) {
        for (int v = -oy; v < 0; v++) {
            u16 *row = img + (oy + v) * stride;
            if (scrollX) {
                any |= DecodeRun(e, bg, type, isMain, f, v, -ox, outW, row, tmp);
            } else {
                any |= DecodeRun(e, bg, type, isMain, f, v, 0, W, row + ox, tmp);
                any |= ExtendRow(e, bg, type, isMain, f, &x, row, ox, v, tmp, FALSE, FALSE);
            }
        }
    } else if (x.mode[2] == EXT_STRETCH) {
        for (int v = 0; v < x.t1; v++) {
            memcpy(img + v * stride, img + (oy + v) * stride, rowBytes);
        }
        const u16 *band = img + (oy + x.t1) * stride;
        for (int y = x.t1; y < x.t1 + oy; y++) {
            memcpy(img + y * stride, band, rowBytes);
        }
    } else {
        for (int y = 0; y < oy; y++) {
            for (int i = 0; i < outW; i++) {
                img[y * stride + i] = clear;
            }
        }
    }
    gNtrExtTicks[2] += STAGE_TICKS() - e2;
    return any;
}

/* Engine composition ------------------------------------------------------- */


/* Fills a line with the backdrop inside the DS picture and the border color
 * beside it. */
static void FillLine(u16 *line, int outW, int ox, BOOL inDS, u16 backdrop, u16 border)
{
    int i0 = inDS ? ox : outW, i1 = inDS ? ox + W : outW;
    for (int x = 0; x < i0; x++) {
        line[x] = border;
    }
    for (int x = i0; x < i1; x++) {
        line[x] = backdrop;
    }
    for (int x = i1; x < outW; x++) {
        line[x] = border;
    }
}

/* Line buffers across the whole output, one set per engine so both engines
 * can render at once. */
#define OW NTR_MAX_SCREEN_W
typedef struct Scratch {
    u16 lines[4][OW];
    u16 img[4][NTR_MAX_SCREEN_H * OW]; /* the software compositor's layer images */
    ObjLine objLine;
    u8 win[OW];
    u16 topC[OW], belowC[OW];
    u8 topL[OW], belowL[OW];
    ObjList objList;
    LineFormat fmt;
    u8 objRowUsed[NTR_MAX_SCREEN_H]; /* GPU composition: OBJ image rows with sprites */
} Scratch;

static Scratch sScratch[2];
static Eng sEngs[2]; /* the registers of the frame being rendered */

/* Renders an engine at outW x outH. The DS picture (256x192) sits in the
 * middle; around it the 3D layer, the BG layers' extensions (ExtendLayer) and
 * sprites are drawn with the same color effects and master brightness, and
 * the window settings of "outside all windows". */
static void RenderEngine(const Eng *e, BOOL isMain, void *outPixels, int outW, int outH, int stride)
{
    u32 *out = outPixels;
    u16 *out16 = outPixels;
    int dispMode = isMain ? (int)((e->dispcnt >> 16) & 3) : 1;

    if (dispMode != 1) {
        /* Display off (white) or VRAM/main-memory display modes, not supported yet. */
        for (int y = 0; y < outH; y++) {
            for (int x = 0; x < outW; x++) {
                if (sOut16) {
                    out16[y * stride + x] = 0xffff;
                } else {
                    out[y * stride + x] = 0xffffffff;
                }
            }
        }
        return;
    }

    int ox = (outW - W) / 2, oy = (outH - H) / 2;
    int visible = (e->dispcnt >> 8) & 0x1f;
    int types[4];
    int order[4];
    int count = 0;

#ifdef NTR_DEBUG_HOOKS
    if (getenv("PORT_HIDE")) {
        visible &= ~(int)strtol(getenv("PORT_HIDE"), NULL, 16) >> (isMain ? 0 : 8); /* main bits 0-4, sub 8-12 */
    }
#endif
    for (int bg = 0; bg < 4; bg++) {
        types[bg] = (visible & (1 << bg)) ? BgType(e, bg, isMain) : BGTYPE_OFF;
    }
    /* Highest priority (lowest value) first; ties go to the lower BG number. */
    for (int prio = 0; prio < 4; prio++) {
        for (int bg = 0; bg < 4; bg++) {
            if (types[bg] != BGTYPE_OFF && (e->bgcnt[bg] & 3) == prio) {
                order[count++] = bg;
            }
        }
    }
    BOOL objOn = (visible & 0x10) != 0;
    BOOL bg0Is3D = types[0] == BGTYPE_3D;

    int effect = (e->bldcnt >> 6) & 3;
    int target1 = e->bldcnt & 0x3f;
    int target2 = (e->bldcnt >> 8) & 0x3f;
    int eva = e->bldalpha & 0x1f, evb = (e->bldalpha >> 8) & 0x1f;
    int evy = e->bldy & 0x1f;
    if (eva > 16) eva = 16;
    if (evb > 16) evb = 16;
    if (evy > 16) evy = 16;

    int mbMode = e->masterBright >> 14;
    int mbFactor = e->masterBright & 0x1f;
    if (mbFactor > 16) mbFactor = 16;

    u16 backdrop = e->bgPltt[0] & 0x7fff;
    u16 border = backdrop; /* the layers go on beside the picture (ExtendLayer) */
    int eng = isMain ? 0 : 1;
    Scratch *sc = &sScratch[eng];
    u16(*lines)[OW] = sc->lines;
#define objLine (sc->objLine)
    u8 *win = sc->win;
    u16 *topC = sc->topC, *belowC = sc->belowC;
    u8 *topL = sc->topL, *belowL = sc->belowL;
    BuildLineFormat(e, &sc->fmt, FALSE);

    /* The BG layers as images of the whole output. */
    BOOL any[4] = { FALSE, FALSE, FALSE, FALSE };
    for (int bg = 0; bg < 4; bg++) {
        if (types[bg] == BGTYPE_TEXT || types[bg] == BGTYPE_AFFINE || types[bg] == BGTYPE_EXTENDED) {
            u16 *img = sc->img[bg];
            any[bg] = DecodeLayerRows(eng, e, bg, types[bg], isMain, &sc->fmt, img, OW, outW, outH, 0, 1, lines[bg]);
            any[bg] |= ExtendLayer(eng, e, bg, types[bg], isMain, &sc->fmt, img, OW, outW, outH, lines[bg]);
        } else if (types[bg] == BGTYPE_3D) {
            any[bg] = TRUE;
        }
    }
    objLine.ofs = ox;
    objLine.xMin = -ox;
    objLine.xMax = outW - ox;
    if (objOn) {
        FindSprites(e, &sc->objList);
    }
    objLine.dirty = TRUE; /* the ofs (and so the layout) may have changed */
    ClearObjLine(&objLine);

    /* The 3D frame is at output size; BG0's scroll moves it (9-bit, signed). */
    int ofs3d = e->bghofs[0] & 0x1ff;
    if (ofs3d >= 256) {
        ofs3d -= 512;
    }
    int w3d, h3d;
    Ntr_EngineScreenSize(0, &w3d, &h3d);
    u32 windows = (e->dispcnt >> 13) & 7;
    u8 winOutside = (u8)(e->winout & 0x3f);

    for (int yo = 0; yo < outH; yo++) {
        int y = yo - oy;
        BOOL inDS = y >= 0 && y < H;
        u64 t0 = STAGE_TICKS();

        /* The layers with pixels; they cover the whole output. */
        int rowOrder[4], rowCount = 0;
        for (int n = 0; n < count; n++) {
            if (any[order[n]]) {
                rowOrder[rowCount++] = order[n];
            }
        }
        BOOL rowObj = objOn;

        /* Each layer's line; the 3D layer is used in place when it lines up. */
        const u16 *lp[4] = { lines[0], lines[1], lines[2], lines[3] };
        BOOL row3DTranslucent = FALSE;
        for (int n = 0; n < rowCount; n++) {
            int bg = rowOrder[n];
            if (types[bg] == BGTYPE_3D) {
                row3DTranslucent = yo < h3d && gNtr3DRowTranslucent[yo];
                if (ofs3d == 0 && w3d == outW && yo < h3d) {
                    lp[0] = gNtr3DColor[yo];
                } else {
                    const u16 *c3d = yo < h3d ? gNtr3DColor[yo] : NULL;
                    for (int x = 0; x < outW; x++) {
                        int sx = x + ofs3d;
                        lines[0][x] = (c3d && sx >= 0 && sx < w3d) ? c3d[sx] : TRANSPARENT;
                    }
                }
            } else {
                lp[bg] = sc->img[bg] + yo * OW;
            }
        }
        u64 t1 = STAGE_TICKS();
        if (rowObj) {
            RenderObjLine(e, y, &objLine, &sc->objList);
        } else {
            ClearObjLine(&objLine);
        }

        u64 t2 = STAGE_TICKS();
        gNtrRenderTicks[eng][0] += t1 - t0;
        gNtrRenderTicks[eng][1] += t2 - t1;

        /* Window control bits per pixel (all layers on without windows):
         * outside, then the OBJ window, WIN1 and WIN0 on top in that order
         * of priority, filled span by span. */
        if (windows) {
            memset(win, winOutside, (size_t)outW);
            if (windows & 4) {
                u8 objWin = (u8)((e->winout >> 8) & 0x3f);
                for (int x = 0; x < outW; x++) {
                    if (objLine.window[x]) {
                        win[x] = objWin;
                    }
                }
            }
            FillWindows(e, win, y, ox, outW);
        }

        u32 *dst = out + yo * stride;
        u16 *dst16 = out16 + yo * stride;
        const u8 *alpha3d = yo < h3d ? gNtr3DAlpha[yo] : NULL;

        /* Rows above and below the DS picture without 3D: one color. */
        if (rowCount == 0 && !inDS && !(rowObj && objLine.prioMask)) {
            u16 c = border;
            if (effect >= 2 && evy != 0 && (target1 & 0x20) && (!windows || (winOutside & 0x20))) {
                c = effect == 2 ? Brighten(c, evy) : Darken(c, evy);
            }
            if (mbMode == 1) {
                c = Brighten(c, mbFactor);
            } else if (mbMode == 2) {
                c = Darken(c, mbFactor);
            }
            if (sOut16) {
                u16 p = To5551(c);
                for (int x = 0; x < outW; x++) {
                    dst16[x] = p;
                }
            } else {
                u32 rgba = sRGBA[c & 0x7fff];
                for (int x = 0; x < outW; x++) {
                    dst[x] = rgba;
                }
            }
            gNtrRenderTicks[eng][2] += STAGE_TICKS() - t2;
            continue;
        }

        /* Fast path, the common case: no windows, semi-transparent sprites
         * or brightness effects. Layers are painted back to front; the color
         * effect and master brightness are applied per line. */
        BOOL anySemi = FALSE;
        if (rowObj && objLine.prioMask) {
            for (int x = 0; x < outW; x++) {
                anySemi |= objLine.semi[x] && objLine.prio[x] != OBJ_NONE;
            }
        }
        if (!windows && !anySemi && !((effect == 2 || effect == 3) && evy != 0)) {
            /* Layer tracking is needed for alpha blending, and for 3D pixels,
             * which blend with the layer below by their own alpha. */
            BOOL blend = row3DTranslucent || (effect == 1 && target1 != 0 && target2 != 0 && !(eva == 16 && evb == 0));
            FillLine(topC, outW, ox, inDS, backdrop, border);
            if (blend) {
                memset(topL, 5, outW);
                memset(belowL, 5, outW);
                FillLine(belowC, outW, ox, inDS, backdrop, border);
            }
            for (int prio = 3; prio >= 0; prio--) {
                for (int n = rowCount - 1; n >= 0; n--) {
                    int bg = rowOrder[n];
                    if ((e->bgcnt[bg] & 3) != prio) {
                        continue;
                    }
                    int x0 = 0, x1 = outW;
                    const u16 *line = lp[bg];
                    if (blend) {
                        for (int x = x0; x < x1; x++) {
                            u16 c = line[x];
                            if (!(c & TRANSPARENT)) {
                                belowC[x] = topC[x], belowL[x] = topL[x];
                                topC[x] = c, topL[x] = (u8)bg;
                            }
                        }
                    } else {
                        for (int x = x0; x < x1; x++) {
                            u16 c = line[x];
                            if (!(c & TRANSPARENT)) {
                                topC[x] = c;
                            }
                        }
                    }
                }
                if (rowObj && (objLine.prioMask & (1 << prio))) {
                    for (int x = 0; x < outW; x++) {
                        if (objLine.prio[x] == prio) {
                            if (blend) {
                                belowC[x] = topC[x], belowL[x] = topL[x], topL[x] = 4;
                            }
                            topC[x] = objLine.color[x];
                        }
                    }
                }
            }
            if (blend) {
                BOOL alphaEffect = effect == 1;
                /* Inside the DS picture: the DS rules. */
                int i0 = inDS ? ox : 0, i1 = inDS ? ox + W : 0;
                for (int x = i0; x < i1; x++) {
                    int tl = topL[x];
                    if (!((target2 >> belowL[x]) & 1)) {
                        continue;
                    }
                    if (tl == 0 && row3DTranslucent) {
                        int a3d = alpha3d[x + ofs3d];
                        if (a3d < 31) {
                            topC[x] = Alpha3D(topC[x], belowC[x], a3d);
                            continue;
                        }
                    }
                    if (alphaEffect && ((target1 >> tl) & 1)) {
                        topC[x] = Alpha(topC[x], belowC[x], eva, evb);
                    }
                }
                /* Widescreen: beside the DS picture nothing of the 2D scene is
                 * drawn, so translucent 3D blends with the backdrop as it would
                 * with the BG behind it inside the picture. */
                if (row3DTranslucent || alphaEffect) {
                    for (int part = 0; part < 2; part++) {
                        int x0 = part == 0 ? 0 : (inDS ? ox + W : outW), x1 = part == 0 ? (inDS ? ox : outW) : outW;
                        for (int x = x0; x < x1; x++) {
                            int tl = topL[x];
                            if (tl == 0 && row3DTranslucent) {
                                int a3d = alpha3d[x + ofs3d];
                                if (a3d < 31) {
                                    topC[x] = Alpha3D(topC[x], belowC[x], a3d);
                                    continue;
                                }
                            }
                            if (alphaEffect && ((target2 >> belowL[x]) & 1) && ((target1 >> tl) & 1)) {
                                topC[x] = Alpha(topC[x], belowC[x], eva, evb);
                            }
                        }
                    }
                }
            }
            if (mbMode == 1 && mbFactor != 0) {
                for (int x = 0; x < outW; x++) {
                    topC[x] = Brighten(topC[x], mbFactor);
                }
            } else if (mbMode == 2 && mbFactor != 0) {
                for (int x = 0; x < outW; x++) {
                    topC[x] = Darken(topC[x], mbFactor);
                }
            }
            if (sOut16) {
                for (int x = 0; x < outW; x++) {
                    dst16[x] = To5551(topC[x]);
                }
            } else {
                for (int x = 0; x < outW; x++) {
                    dst[x] = sRGBA[topC[x] & 0x7fff];
                }
            }
            gNtrRenderTicks[eng][2] += STAGE_TICKS() - t2;
            continue;
        }

        /* The two front-most pixels of each column, painted back to front.
         * Layers: 0-3 BGs, 4 OBJ, 5 backdrop. A sprite is in front of BGs of
         * the same priority; among BGs, the lower number is in front. */
        FillLine(topC, outW, ox, inDS, backdrop, border);
        FillLine(belowC, outW, ox, inDS, backdrop, border);
        memset(topL, 5, outW);
        memset(belowL, 5, outW);
        for (int prio = 3; prio >= 0; prio--) {
            for (int n = rowCount - 1; n >= 0; n--) {
                int bg = rowOrder[n];
                if ((e->bgcnt[bg] & 3) != prio) {
                    continue;
                }
                const u16 *line = lp[bg];
                u8 bit = (u8)(1 << bg);
                int x0 = 0, x1 = outW;
                for (int x = x0; x < x1; x++) {
                    u16 c = line[x];
                    if (!(c & TRANSPARENT) && (!windows || (win[x] & bit))) {
                        belowC[x] = topC[x], belowL[x] = topL[x];
                        topC[x] = c, topL[x] = (u8)bg;
                    }
                }
            }
            if (rowObj && (objLine.prioMask & (1 << prio))) {
                for (int x = 0; x < outW; x++) {
                    if (objLine.prio[x] == prio && (!windows || (win[x] & 0x10))) {
                        belowC[x] = topC[x], belowL[x] = topL[x];
                        topC[x] = objLine.color[x], topL[x] = 4;
                    }
                }
            }
        }

        BOOL lineSemi = FALSE;
        if (rowObj && objLine.prioMask) {
            for (int x = 0; x < outW && !lineSemi; x++) {
                lineSemi = objLine.semi[x] && objLine.prio[x] != OBJ_NONE;
            }
        }
        if (effect == 0 && !lineSemi && !row3DTranslucent) {
            /* Nothing blends on this line: the front pixel, then master brightness. */
            for (int x = 0; x < outW; x++) {
                u16 color = topC[x];
                if (mbMode == 1) {
                    color = Brighten(color, mbFactor);
                } else if (mbMode == 2) {
                    color = Darken(color, mbFactor);
                }
                if (sOut16) {
                    dst16[x] = To5551(color);
                } else {
                    dst[x] = sRGBA[color & 0x7fff];
                }
            }
            gNtrRenderTicks[eng][2] += STAGE_TICKS() - t2;
            continue;
        }

        for (int x = 0; x < outW; x++) {
            u16 top = topC[x], color = top;
            int topLayer = topL[x], belowLayer = belowL[x];
            BOOL effectsOn = !windows || (win[x] & 0x20);
            int a3d = (topLayer == 0 && bg0Is3D) ? alpha3d[x + ofs3d] : 31;
            if (topLayer == 4 && objLine.semi[x] && (target2 & (1 << belowLayer))) {
                color = Alpha(top, belowC[x], eva, evb);
            } else if (a3d < 31 && ((target2 & (1 << belowLayer)) || !inDS || x < ox || x >= ox + W)) {
                /* 3D pixels with partial alpha blend with the layer below using their own alpha. */
                color = Alpha3D(top, belowC[x], a3d);
            } else if (effect != 0 && effectsOn && (target1 & (1 << topLayer))) {
                if (effect == 1 && (target2 & (1 << belowLayer))) {
                    color = Alpha(top, belowC[x], eva, evb);
                } else if (effect == 2) {
                    color = Brighten(top, evy);
                } else if (effect == 3) {
                    color = Darken(top, evy);
                }
            }

            if (mbMode == 1) {
                color = Brighten(color, mbFactor);
            } else if (mbMode == 2) {
                color = Darken(color, mbFactor);
            }
            if (sOut16) {
                dst16[x] = To5551(color);
            } else {
                dst[x] = sRGBA[color & 0x7fff];
            }
        }
        gNtrRenderTicks[eng][2] += STAGE_TICKS() - t2;
    }
}

#undef objLine

/* GPU composition ---------------------------------------------------------------
 *
 * The same rules as RenderEngine, as passes for a GPU (see NtrComposition).
 * Layers are painted back to front; the stencil keeps the top layer of each
 * pixel as one bit (BG0-3 = bits 0-3, OBJ = bit 4, backdrop = bit 5), so a
 * pass can test whether the pixel it covers is a second target of blending.
 * Brightness effects come last, on the pixels whose top layer is a first
 * target. Painting blends with the pixel below as it is at that point; where
 * that pixel may have been blended itself, the blend takes the lower layer's
 * raw color from its image instead (the DS blends raw colors). The OBJ
 * window's shape is an image (the window bits of the sprite lines), and the
 * regions where it can apply are made twice, for the pixels inside it and
 * those outside, with passes limited to either. Left to the software
 * compositor: display modes other than graphics, and brightness effects in
 * frames with blended sprites or 3D pixels. */

#define ST_BACKDROP 0x20
#define ST_LAYERS 0x3f

typedef struct Region {
    NtrRect r;
    u8 ctl; /* window control: BG0-3, OBJ, color effects */
    u8 border; /* beside the DS picture */
    u8 objWin; /* as NtrPass.objWindow: the pixels of the rectangle it is for */
} Region;

#define MAX_REGIONS 160

static int AddRegion(Region *reg, int n, int x0, int y0, int x1, int y1, u8 ctl, u8 border)
{
    if (x0 < x1 && y0 < y1 && n < MAX_REGIONS) {
        reg[n].r = (NtrRect){ (s16)x0, (s16)y0, (s16)x1, (s16)y1 };
        reg[n].ctl = ctl;
        reg[n].border = border;
        reg[n].objWin = 0;
        n++;
    }
    return n;
}

/* A region outside WIN0 and WIN1 with the OBJ window on: twice, the OBJ
 * window's control for the pixels inside it, the outside's for the rest. */
static int AddOutsideRegion(const Eng *e, Region *reg, int n, int x0, int y0, int x1, int y1, u8 outside, u8 border)
{
    if (!(e->dispcnt & 0x8000)) {
        return AddRegion(reg, n, x0, y0, x1, y1, outside, border);
    }
    int first = n;
    n = AddRegion(reg, n, x0, y0, x1, y1, (u8)((e->winout >> 8) & 0x3f), border);
    if (n > first) {
        reg[first].objWin = 1;
    }
    first = n;
    n = AddRegion(reg, n, x0, y0, x1, y1, outside, border);
    if (n > first) {
        reg[first].objWin = 2;
    }
    return n;
}

/* The window control of every part of the output, as rectangles: bands of DS
 * lines with the same windows, cut at the windows' left and right edges. */
static int BuildRegions(const Eng *e, int ox, int oy, int outW, int outH, Region *reg)
{
    u32 windows = (e->dispcnt >> 13) & 7;
    u8 outside = windows ? (u8)(e->winout & 0x3f) : 0x3f;
    int n = 0;
    if (!windows) {
        n = AddRegion(reg, n, 0, 0, outW, oy, outside, 1);
        n = AddRegion(reg, n, 0, oy + H, outW, outH, outside, 1);
        n = AddRegion(reg, n, 0, oy, ox, oy + H, outside, 1);
        n = AddRegion(reg, n, ox + W, oy, outW, oy + H, outside, 1);
        return AddRegion(reg, n, ox, oy, ox + W, oy + H, 0x3f, 0);
    }
    /* With windows: the control along each row (bit 6: beside the DS
     * picture, bit 7: outside WIN0 and WIN1), made only where a window
     * starts or ends, as rectangles. */
    u8 row[NTR_MAX_SCREEN_W];
    int bandStart = 0, bandKey = -1;
    for (int y = 0; y <= outH; y++) {
        int key = -1;
        if (y < outH) {
            int v = y - oy;
            key = v >= 0 && v < H;
            for (int k = 0; k < 2; k++) {
                const u8 *r = k ? e->win1 : e->win0;
                BOOL in = (windows & (1u << k))
                    && (WindowFull(r[2], r[3], 192) || (v >= 0 && v < 192 && InWindowRange(v, r[2], r[3])));
                key |= in << (k + 1);
            }
        }
        if (key == bandKey) {
            continue;
        }
        if (bandKey >= 0) {
            for (int x = 0; x < outW;) {
                int x1 = x + 1;
                while (x1 < outW && row[x1] == row[x]) {
                    x1++;
                }
                if (row[x] & 0x80) {
                    n = AddOutsideRegion(e, reg, n, x, bandStart, x1, y, row[x] & 0x3f, (row[x] >> 6) & 1);
                } else {
                    n = AddRegion(reg, n, x, bandStart, x1, y, row[x] & 0x3f, (row[x] >> 6) & 1);
                }
                x = x1;
            }
        }
        if (y < outH) {
            int v = y - oy;
            memset(row, outside | 0x80, (size_t)outW);
            FillWindows(e, row, v, ox, outW);
            if (v < 0 || v >= H) {
                for (int x = 0; x < outW; x++) {
                    row[x] |= 0x40;
                }
            } else {
                for (int x = 0; x < ox; x++) {
                    row[x] |= 0x40;
                    row[ox + W + x] |= 0x40;
                }
            }
        }
        bandKey = key;
        bandStart = y;
    }
    return n;
}

typedef struct Emitter {
    NtrComposition *c;
    const Region *reg;
    int numReg;
    /* Blending */
    BOOL alphaOn; /* alpha blending of first over second targets */
    u8 target1, target2, eva, evb;
    u8 preBlended; /* layers whose pixels may be blended already */
    u8 source[5]; /* each layer's image: BG0-3 (or 3D), OBJ */
} Emitter;

/* Appends pass p over the regions where window control bit `layer` is on
 * (-1: everywhere), with color effects on (eff 1), off (0) or either (-1), in
 * the DS picture (where 0), beside it (1) or both (-1), cut to `area`. */
static void Emit(Emitter *m, NtrPass p, int layer, int eff, int where, NtrRect area)
{
    NtrComposition *c = m->c;
    for (int w = 0; w < (c->objWindow ? 3 : 1); w++) { /* a pass for each part of the OBJ window */
    if (c->numPasses >= NTR_COMPOSE_MAX_PASSES) {
        c->overflow = TRUE;
        return;
    }
    p.objWindow = (u8)w;
    p.firstRect = (u16)c->numRects;
    p.numRects = 0;
    for (int i = 0; i < m->numReg; i++) {
        const Region *g = &m->reg[i];
        if ((layer >= 0 && !(g->ctl & (1 << layer))) || (eff >= 0 && ((g->ctl >> 5) & 1) != eff)
            || (where >= 0 && g->border != where) || g->objWin != w) {
            continue;
        }
        NtrRect r = g->r;
        r.x0 = r.x0 > area.x0 ? r.x0 : area.x0;
        r.y0 = r.y0 > area.y0 ? r.y0 : area.y0;
        r.x1 = r.x1 < area.x1 ? r.x1 : area.x1;
        r.y1 = r.y1 < area.y1 ? r.y1 : area.y1;
        if (r.x0 >= r.x1 || r.y0 >= r.y1) {
            continue;
        }
        if (c->numRects >= NTR_COMPOSE_MAX_RECTS) {
            c->overflow = TRUE;
            break;
        }
        c->rects[c->numRects++] = r;
        p.numRects++;
    }
    if (p.numRects > 0) {
        c->passes[c->numPasses++] = p;
    }
    }
}

static NtrPass Pass(int source, int alphaTest, int alphaRef)
{
    NtrPass p;
    memset(&p, 0, sizeof(p));
    p.source = (u8)source;
    p.alphaTest = (u8)alphaTest;
    p.alphaRef = (u8)alphaRef;
    p.stencilTest = NTR_TEST_ALWAYS;
    p.colorWrite = 1;
    p.blend = NTR_BLEND_NONE;
    return p;
}

/* p, drawn only where the pixel below is (test EQUAL: is not) a second target. */
static NtrPass OverTarget2(NtrPass p, const Emitter *m, int test)
{
    p.stencilTest = (u8)test;
    p.stencilRef = 0;
    p.stencilMask = m->target2;
    return p;
}

/* p (a blend) where the pixel below is a second target: with the pixel as it
 * is, or, below layers that may hold blended pixels, with that layer's raw
 * color from its image. */
static void EmitBlendOverTarget2(Emitter *m, NtrPass p, int layer, int eff, int where, NtrRect area)
{
    u8 raw = (u8)(m->target2 & ~m->preBlended);
    if (raw) {
        NtrPass q = p;
        q.stencilTest = NTR_TEST_NOTEQUAL;
        q.stencilRef = 0;
        q.stencilMask = raw;
        Emit(m, q, layer, eff, where, area);
    }
    for (int below = 0; below < 5; below++) {
        if (below != layer && (m->target2 & m->preBlended & (1 << below))) {
            NtrPass q = p;
            q.stencilTest = NTR_TEST_EQUAL;
            q.stencilRef = (u8)(1 << below);
            q.stencilMask = ST_LAYERS;
            q.below = m->source[below];
            Emit(m, q, layer, eff, where, area);
        }
    }
}

/* Makes `layer` (0-5) the top layer where the selected source pixels are. */
static void EmitTop(Emitter *m, NtrPass p, int layer, int visLayer, NtrRect area)
{
    p.colorWrite = 0;
    p.blend = NTR_BLEND_NONE;
    p.stencilTest = NTR_TEST_ALWAYS;
    p.stencilRef = (u8)(1 << layer);
    p.stencilWrite = ST_LAYERS;
    Emit(m, p, visLayer, -1, -1, area);
}

/* Layer pixels under the normal rules: a first target of alpha blending is
 * blended where the pixel below is a second target and color effects are on,
 * everything else is drawn as it is. `topTest`/`topRef` select the pixels
 * that become the top layer (the drawn ones unless more passes follow). */
static void EmitNormal(Emitter *m, NtrPass p, int layer, NtrRect area, BOOL setTop)
{
    u8 bit = (u8)(1 << layer);
    if (m->alphaOn && (m->target1 & bit)) {
        NtrPass b = p;
        b.blend = NTR_BLEND_EVA_EVB;
        b.eva = m->eva;
        b.evb = m->evb;
        EmitBlendOverTarget2(m, b, layer, 1, -1, area);
        Emit(m, OverTarget2(p, m, NTR_TEST_EQUAL), layer, 1, -1, area);
        Emit(m, p, layer, 0, -1, area);
        if (setTop) {
            EmitTop(m, p, layer, layer, area);
        }
    } else if (setTop) {
        p.stencilRef = bit;
        p.stencilWrite = ST_LAYERS;
        Emit(m, p, layer, -1, -1, area);
    } else {
        Emit(m, p, layer, -1, -1, area);
    }
}

/* Semi-transparent sprites blend with a second target below them whatever
 * the first targets and windows say; elsewhere they are normal sprites. */
static void EmitSemiObj(Emitter *m, int prio, NtrRect area)
{
    NtrPass p = Pass(NTR_SRC_OBJ, NTR_TEST_EQUAL, NTR_OBJ_ALPHA(prio, 1));
    if (m->target2 == 0) {
        EmitNormal(m, p, 4, area, TRUE);
        return;
    }
    NtrPass b = p;
    b.blend = NTR_BLEND_EVA_EVB;
    b.eva = m->eva;
    b.evb = m->evb;
    EmitBlendOverTarget2(m, b, 4, -1, -1, area);
    Emit(m, OverTarget2(p, m, NTR_TEST_EQUAL), 4, -1, -1, area);
    EmitTop(m, p, 4, 4, area);
}

/* The 3D layer: translucent pixels (alpha below 31) over a second target, or
 * beside the DS picture, blend by their own alpha; elsewhere they are drawn as
 * they are. Opaque pixels follow the normal rules. */
static void Emit3D(Emitter *m, NtrRect area)
{
    NtrPass t = Pass(NTR_SRC_3D, NTR_TEST_LESS, 255);
    t.blend = NTR_BLEND_SRC_ALPHA;
    if (m->target2) {
        EmitBlendOverTarget2(m, t, 0, -1, 0, area);
    }
    Emit(m, t, 0, -1, 1, area);
    NtrPass o = Pass(NTR_SRC_3D, NTR_TEST_GREATER, 0);
    if (m->target2) {
        Emit(m, OverTarget2(o, m, NTR_TEST_EQUAL), 0, -1, 0, area);
    } else {
        Emit(m, o, 0, -1, 0, area);
    }
    EmitNormal(m, Pass(NTR_SRC_3D, NTR_TEST_EQUAL, 255), 0, area, FALSE);
    EmitTop(m, Pass(NTR_SRC_3D, NTR_TEST_GREATER, 0), 0, 0, area);
}

const char *gNtrComposeFallback; /* why the last Ntr_ComposeEngine returned FALSE */

/* One engine's composition between Ntr_ComposeBegin and Ntr_ComposeEnd. */
typedef struct ComposeState {
    int types[4];
    BOOL objOn;
    BOOL objWin; /* the OBJ window is on: its image is written */
    /* Each part of the lines decodes with its own sprite line and finds
     * which layers have pixels. */
    ObjLine objLine[NTR_COMPOSE_MAX_PARTS];
    u16 line[NTR_COMPOSE_MAX_PARTS][NTR_MAX_SCREEN_W];
    BOOL any[NTR_COMPOSE_MAX_PARTS][4];
    u32 objCodes[NTR_COMPOSE_MAX_PARTS]; /* bit prio * 2 + semi */
    s16 objRow0[NTR_COMPOSE_MAX_PARTS], objRow1[NTR_COMPOSE_MAX_PARTS]; /* OBJ rows written */
    /* What each BG image holds (a hash of all it is made from), so a layer
     * that did not change is not decoded again. */
    u32 key[4];
    BOOL keyValid[4], reuse[4], lastAny[4];
} ComposeState;

static ComposeState sCompose[2];

static u32 HashWords(u32 h, const void *p, u32 bytes)
{
    const u32 *w = p;
    for (u32 i = 0; i + 4 <= bytes / 4; i += 4) {
        h = (h ^ w[i]) * 0x01000193u;
        h = (h ^ w[i + 1]) * 0x01000193u;
        h = (h ^ w[i + 2]) * 0x01000193u;
        h = (h ^ w[i + 3]) * 0x01000193u;
    }
    for (u32 i = bytes / 16 * 4; i < bytes / 4; i++) {
        h = (h ^ w[i]) * 0x01000193u;
    }
    return h;
}

static u32 HashVram(u32 h, const Eng *e, u32 addr, u32 bytes)
{
    if (addr >= e->bgVramSize) {
        return h;
    }
    return HashWords(h, e->bgVram + addr, bytes < e->bgVramSize - addr ? bytes : e->bgVramSize - addr);
}

/* A hash of everything a BG layer's output image is made from. */
/* The highest tile a map refers to (16-bit entries, tile number in bits
 * 0-9, or 8-bit entries), so only the character data in use is hashed. */
static u32 MaxTile(const Eng *e, u32 addr, u32 bytes, BOOL entries16)
{
    if (addr >= e->bgVramSize) {
        return 0;
    }
    if (bytes > e->bgVramSize - addr) {
        bytes = e->bgVramSize - addr;
    }
    u32 max = 0;
    if (entries16) {
        const u16 *m = (const u16 *)(e->bgVram + addr);
        for (u32 i = 0; i < bytes / 2; i++) {
            u32 t = m[i] & 0x3ff;
            max = t > max ? t : max;
        }
    } else {
        const u8 *m = e->bgVram + addr;
        for (u32 i = 0; i < bytes; i++) {
            max = m[i] > max ? m[i] : max;
        }
    }
    return max;
}

static u32 LayerKey(int engine, const Eng *e, int bg, int type, BOOL isMain, int outW, int outH)
{
    u16 cnt = e->bgcnt[bg];
    BOOL scrollX, scrollY;
    LayerScrolling(engine, bg, &scrollX, &scrollY);
    u32 regs[12] = { (u32)type, cnt, e->bghofs[bg], e->bgvofs[bg], e->dispcnt & 0x7f000000u, (u32)outW, (u32)outH,
        (u32)scrollX | (u32)scrollY << 1, 0, 0, 0, 0 };
    if (bg >= 2) {
        regs[8] = (u32)e->bgx[bg - 2], regs[9] = (u32)e->bgy[bg - 2];
        regs[10] = (u16)e->bgpa[bg - 2] | (u32)(u16)e->bgpb[bg - 2] << 16;
        regs[11] = (u16)e->bgpc[bg - 2] | (u32)(u16)e->bgpd[bg - 2] << 16;
    }
    u32 h = HashWords(0x811c9dc5u, regs, sizeof(regs));
    u32 scr = ((cnt >> 8) & 0x1f) * 0x800, chr = ((cnt >> 2) & 0xf) * 0x4000;
    if (isMain) {
        scr += ((e->dispcnt >> 27) & 7) * 0x10000;
        chr += ((e->dispcnt >> 24) & 7) * 0x10000;
    }
    BOOL is256 = (cnt & 0x80) != 0;
    int size = cnt >> 14;
    /* The map, and the character data up to the highest tile it uses. */
    if (type == BGTYPE_TEXT) {
        u32 mapBytes = 0x800u << ((size & 1) + (size >> 1));
        u32 tileBytes = is256 ? 64 : 32;
        h = HashVram(h, e, scr, mapBytes);
        h = HashVram(h, e, chr, (MaxTile(e, scr, mapBytes, TRUE) + 1) * tileBytes);
    } else if (type == BGTYPE_AFFINE) {
        u32 tiles = (u32)(16 << size);
        h = HashVram(h, e, scr, tiles * tiles);
        h = HashVram(h, e, chr, (MaxTile(e, scr, tiles * tiles, FALSE) + 1) * 64);
    } else if (!is256) {
        u32 tiles = (u32)(16 << size);
        h = HashVram(h, e, scr, tiles * tiles * 2);
        h = HashVram(h, e, chr, (MaxTile(e, scr, tiles * tiles * 2, TRUE) + 1) * 64);
    } else {
        static const u32 bytes[4] = { 128 * 128, 256 * 256, 512 * 256, 512 * 512 };
        h = HashVram(h, e, ((cnt >> 8) & 0x1f) * 0x4000, bytes[size] * ((cnt & 4) ? 2 : 1));
    }
    h = HashWords(h, e->bgPltt, 512);
    if (e->bgExtPltt != NULL && (e->dispcnt & (1u << 30))) {
        int slot = bg < 2 && (cnt & (1 << 13)) ? bg + 2 : bg;
        h = HashWords(h, e->bgExtPltt + slot * 0x2000, 0x2000);
    }
    return h;
}

BOOL Ntr_ComposeBegin(int engine, NtrComposition *c)
{
    Eng *e = &sEngs[engine];
    ComposeState *cs = &sCompose[engine];
    Snapshot(&gNtrEngines[engine], e);
    TrackScroll(engine, e);
    BOOL isMain = engine == 0;
    int outW, outH;
    Ntr_EngineScreenSize(engine, &outW, &outH);
    c->outW = outW, c->outH = outH;
    c->ox = (outW - W) / 2, c->oy = (outH - H) / 2;
    c->numPasses = c->numRects = 0;
    c->overflow = FALSE;
    if (sRGBA[0x7fff] == 0) {
        InitRGBA();
    }

    int dispMode = isMain ? (int)((e->dispcnt >> 16) & 3) : 1;
    if (dispMode != 1) {
        gNtrComposeFallback = "display mode";
        return FALSE;
    }
    int visible = (e->dispcnt >> 8) & 0x1f;
    for (int bg = 0; bg < 4; bg++) {
        cs->types[bg] = (visible & (1 << bg)) ? BgType(e, bg, isMain) : BGTYPE_OFF;
    }
    cs->objOn = (visible & 0x10) != 0;
    cs->objWin = c->objWindow = (e->dispcnt & 0x8000) != 0;

    int effect = (e->bldcnt >> 6) & 3;
    BOOL bright = (effect == 2 || effect == 3) && (e->bldy & 0x1f) != 0;
    Scratch *sc = &sScratch[engine];
    BOOL semiSprites = FALSE;
    if (cs->objOn) {
        FindSprites(e, &sc->objList);
        for (int k = 0; k < sc->objList.count; k++) {
            semiSprites |= ((e->oam[sc->objList.index[k] * 8 + 1] >> 2) & 3) == 1;
        }
    }
    /* Brightness effects skip pixels blended by the semi-transparent sprite
     * or 3D rules, which the passes cannot tell apart. */
    if (bright && (semiSprites || (cs->types[0] == BGTYPE_3D && (e->bldcnt >> 8) & 0x3f))) {
        gNtrComposeFallback = "brightness effect over blended pixels";
        return FALSE;
    }
    BuildLineFormat(e, &sc->fmt, TRUE);
    memset(cs->any, 0, sizeof(cs->any));
    memset(cs->objCodes, 0, sizeof(cs->objCodes));
    memset(cs->objRow0, 0, sizeof(cs->objRow0));
    memset(cs->objRow1, 0, sizeof(cs->objRow1));

    /* Layers whose image is the same as last frame's are kept. */
    c->changedBg = 0;
    for (int bg = 0; bg < 4; bg++) {
        int type = cs->types[bg];
        cs->reuse[bg] = FALSE;
        if (type != BGTYPE_TEXT && type != BGTYPE_AFFINE && type != BGTYPE_EXTENDED) {
            continue;
        }
        u32 key = LayerKey(engine, e, bg, type, isMain, outW, outH);
#ifdef NTR_DEBUG_HOOKS
        static int noReuse = -1;
        if (noReuse < 0) {
            noReuse = getenv("PORT_NOREUSE") != NULL;
        }
        cs->reuse[bg] = !noReuse && cs->keyValid[bg] && cs->key[bg] == key;
#else
        cs->reuse[bg] = cs->keyValid[bg] && cs->key[bg] == key;
#endif
        cs->key[bg] = key;
        cs->keyValid[bg] = TRUE;
        if (!cs->reuse[bg]) {
            c->changedBg |= (u8)(1 << bg);
        }
    }
#ifdef NTR_DEBUG_HOOKS
    if (getenv("PORT_LAYERLOG") && c->changedBg) {
        printf("frame %u engine %d: layers changed %x (types %d %d %d %d, sprites %d) bgcnt %04x %04x %04x %04x\n", Ntr_FramesEnded(), engine,
            c->changedBg, cs->types[0], cs->types[1], cs->types[2], cs->types[3], cs->objOn ? sc->objList.count : -1, e->bgcnt[0],
            e->bgcnt[1], e->bgcnt[2], e->bgcnt[3]);
        for (int bg = 0; bg < 4; bg++) {
            BOOL sx, sy;
            LayerScrolling(engine, bg, &sx, &sy);
            if (c->changedBg & (1 << bg)) {
                printf("    BG%d: scroll %d/%d ofs %d,%d%s\n", bg, sx, sy, e->bghofs[bg], e->bgvofs[bg],
                    bg >= 2 ? (e->bgpa[bg - 2] == 256 && e->bgpb[bg - 2] == 0 && e->bgpc[bg - 2] == 0 && e->bgpd[bg - 2] == 256 ? " identity" : " transformed") : "");
            }
        }
    }
#endif
    return TRUE;
}

void Ntr_ComposeLines(int engine, const NtrLayerImages *img, int part, int numParts)
{
    const Eng *e = &sEngs[engine];
    ComposeState *cs = &sCompose[engine];
    Scratch *sc = &sScratch[engine];
    BOOL isMain = engine == 0;
    ObjLine *objLine = &cs->objLine[part];
    BOOL *any = cs->any[part];
    u32 objCodes = 0;
    int outW, outH;
    Ntr_EngineScreenSize(engine, &outW, &outH);
    int ox = (outW - W) / 2, oy = (outH - H) / 2;

    /* The BG layers' DS lines (the rest is done by Ntr_ComposeEnd). */
    u64 t0 = STAGE_TICKS();
    int v0 = H * part / numParts, v1 = H * (part + 1) / numParts;
    for (int bg = 0; bg < 4; bg++) {
        int type = cs->types[bg];
        if ((type == BGTYPE_TEXT || type == BGTYPE_AFFINE || type == BGTYPE_EXTENDED) && !cs->reuse[bg]) {
            any[bg] |= DecodeLayerRows(engine, e, bg, type, isMain, &sc->fmt, img->bg[bg], img->stride, outW, outH, part, numParts,
                cs->line[part]);
        }
    }

    /* Sprites, on every output row. */
    u64 t1 = STAGE_TICKS();
    int written0 = NTR_MAX_SCREEN_H, written1 = 0;
    if (cs->objOn) {
        objLine->ofs = ox;
        objLine->xMin = -ox;
        objLine->xMax = outW - ox;
        objLine->dirty = TRUE;
        ClearObjLine(objLine);
        int y0 = outH * part / numParts, y1 = outH * (part + 1) / numParts;
        for (int y = y0; y < y1; y++) {
            RenderObjLine(e, y - oy, objLine, &sc->objList);
            if (cs->objWin) {
                u16 *wrow = img->objWin + y * img->stride;
                for (int x = 0; x < outW; x++) {
                    wrow[x] = objLine->window[x] ? 0xffff : 0;
                }
            }
            u32 *orow = img->obj + y * img->stride;
            if (objLine->prioMask) {
                int lo = objLine->lo, hi = objLine->hi + 1;
                memset(orow, 0, (size_t)lo * 4);
                memset(orow + hi, 0, (size_t)(outW - hi) * 4);
                for (int x = lo; x < hi; x++) {
                    u8 prio = objLine->prio[x];
                    if (prio == OBJ_NONE) {
                        /* Runs without sprite pixels, four at a time. */
                        u32 four;
                        while (x + 4 <= hi && (memcpy(&four, &objLine->prio[x], 4), four == 0xffffffffu)) {
                            orow[x] = orow[x + 1] = orow[x + 2] = orow[x + 3] = 0;
                            x += 4;
                        }
                        if (x < hi && objLine->prio[x] == OBJ_NONE) {
                            orow[x] = 0;
                            continue;
                        }
                        if (x >= hi) {
                            break;
                        }
                        prio = objLine->prio[x];
                    }
                    {
                        u32 semi = objLine->semi[x];
                        orow[x] = (sRGBA[objLine->color[x]] & 0xffffff00u) | NTR_OBJ_ALPHA(prio, semi);
                        objCodes |= 1u << (prio * 2 + semi);
                    }
                }
                sc->objRowUsed[y] = 1;
                written0 = y < written0 ? y : written0;
                written1 = y + 1;
            } else if (sc->objRowUsed[y]) {
                memset(orow, 0, (size_t)outW * 4);
                sc->objRowUsed[y] = 0;
                written0 = y < written0 ? y : written0;
                written1 = y + 1;
            }
        }
    }
    if (cs->objWin && !cs->objOn) { /* no sprites: an empty window */
        for (int y = outH * part / numParts; y < outH * (part + 1) / numParts; y++) {
            memset(img->objWin + y * img->stride, 0, (size_t)outW * 2);
        }
    }
    cs->objCodes[part] = objCodes;
    cs->objRow0[part] = (s16)written0;
    cs->objRow1[part] = (s16)written1;
    u64 t2 = STAGE_TICKS();
    gNtrRenderTicks[engine][0] += t1 - t0;
    gNtrRenderTicks[engine][1] += t2 - t1;
}

BOOL Ntr_ComposeEngine(int engine, const NtrLayerImages *img, NtrComposition *c)
{
    if (!Ntr_ComposeBegin(engine, c)) {
        return FALSE;
    }
    Ntr_ComposeLines(engine, img, 0, 1);
    return Ntr_ComposeEnd(engine, img, c);
}

BOOL Ntr_ComposeEnd(int engine, const NtrLayerImages *img, NtrComposition *c)
{
    u64 t1 = STAGE_TICKS();
    const Eng *e = &sEngs[engine];
    ComposeState *cs = &sCompose[engine];
    const int *types = cs->types;
    BOOL bg0Is3D = types[0] == BGTYPE_3D;
    int outW = c->outW, outH = c->outH, ox = c->ox, oy = c->oy;

    int effect = (e->bldcnt >> 6) & 3;
    u8 target1 = (u8)(e->bldcnt & 0x3f), target2 = (u8)((e->bldcnt >> 8) & 0x3f);
    int eva = e->bldalpha & 0x1f, evb = (e->bldalpha >> 8) & 0x1f;
    int evy = e->bldy & 0x1f;
    if (eva > 16) eva = 16;
    if (evb > 16) evb = 16;
    if (evy > 16) evy = 16;
    int mbMode = e->masterBright >> 14;
    int mbFactor = e->masterBright & 0x1f;
    if (mbFactor > 16) mbFactor = 16;
    BOOL bright = (effect == 2 || effect == 3) && evy != 0;

    BOOL any[4] = { FALSE, FALSE, FALSE, FALSE };
    u32 objCodes = 0;
    c->objRow0 = NTR_MAX_SCREEN_H, c->objRow1 = 0;
    for (int part = 0; part < NTR_COMPOSE_MAX_PARTS; part++) {
        for (int bg = 0; bg < 4; bg++) {
            any[bg] |= cs->any[part][bg];
        }
        objCodes |= cs->objCodes[part];
        if (cs->objRow1[part] > cs->objRow0[part]) {
            c->objRow0 = cs->objRow0[part] < c->objRow0 ? cs->objRow0[part] : c->objRow0;
            c->objRow1 = cs->objRow1[part] > c->objRow1 ? cs->objRow1[part] : c->objRow1;
        }
    }
    /* The BG layers beside the DS picture. */
    for (int bg = 0; bg < 4; bg++) {
        int type = types[bg];
        if (type == BGTYPE_TEXT || type == BGTYPE_AFFINE || type == BGTYPE_EXTENDED) {
            if (cs->reuse[bg]) {
                any[bg] = cs->lastAny[bg];
            } else {
                any[bg] |= ExtendLayer(engine, e, bg, type, engine == 0, &sScratch[engine].fmt, img->bg[bg], img->stride, outW, outH,
                    cs->line[0]);
                cs->lastAny[bg] = any[bg];
            }
        }
    }

    u32 present = ST_BACKDROP | (objCodes ? 0x10 : 0) | (bg0Is3D ? 1 : 0);
    for (int bg = 0; bg < 4; bg++) {
        present |= any[bg] ? 1u << bg : 0;
    }
    Emitter m = { c, NULL, 0, FALSE, target1, target2, (u8)eva, (u8)evb, 0, { 0 } };
    m.alphaOn = effect == 1 && target2 != 0 && (target1 & present) != 0 && !(eva == 16 && evb == 0);
    /* Layers that can blend, and so be a blended pixel below another. */
    m.preBlended = (u8)(((m.alphaOn ? target1 & present : 0) | ((objCodes & 0xaa) ? 0x10 : 0) | (bg0Is3D ? 1 : 0)) & 0x1f);
    for (int bg = 0; bg < 4; bg++) {
        m.source[bg] = (u8)(types[bg] == BGTYPE_3D ? NTR_SRC_3D : NTR_SRC_BG0 + bg);
    }
    m.source[4] = NTR_SRC_OBJ;

    u64 r0 = STAGE_TICKS();
    Region regions[MAX_REGIONS];
    m.reg = regions;
    m.numReg = BuildRegions(e, ox, oy, outW, outH, regions);
    NtrRect all = { 0, 0, (s16)outW, (s16)outH };
    NtrRect ds = { (s16)ox, (s16)oy, (s16)(ox + W), (s16)(oy + H) };

    /* Backdrop inside the picture, border color beside it (as RenderEngine). */
    u16 backdrop = e->bgPltt[0] & 0x7fff;
    u16 border = backdrop;
    NtrPass bd = Pass(NTR_SRC_COLOR, NTR_TEST_ALWAYS, 0);
    bd.stencilRef = ST_BACKDROP;
    bd.stencilWrite = 0xff;
    bd.color = sRGBA[backdrop];
    Emit(&m, bd, -1, -1, 0, all);
    bd.color = sRGBA[border];
    Emit(&m, bd, -1, -1, 1, all);

    /* The 3D frame is at the output size; BG0's scroll moves it. */
    int ofs3d = e->bghofs[0] & 0x1ff;
    if (ofs3d >= 256) {
        ofs3d -= 512;
    }
    c->ofs3D = ofs3d;
    int w3d, h3d;
    Ntr_EngineScreenSize(0, &w3d, &h3d);
    NtrRect area3d = { (s16)(ofs3d < 0 ? -ofs3d : 0), 0, (s16)(w3d - ofs3d < outW ? w3d - ofs3d : outW),
        (s16)(h3d < outH ? h3d : outH) };

    for (int prio = 3; prio >= 0; prio--) {
        for (int bg = 3; bg >= 0; bg--) {
            if (types[bg] == BGTYPE_OFF || (e->bgcnt[bg] & 3) != prio) {
                continue;
            }
            if (types[bg] == BGTYPE_3D) {
                Emit3D(&m, area3d);
            } else if (any[bg]) {
                EmitNormal(&m, Pass(NTR_SRC_BG0 + bg, NTR_TEST_GREATER, 0), bg, all, TRUE);
            }
        }
        if (objCodes & (1u << (prio * 2))) {
            EmitNormal(&m, Pass(NTR_SRC_OBJ, NTR_TEST_EQUAL, NTR_OBJ_ALPHA(prio, 0)), 4, all, TRUE);
        }
        if (objCodes & (2u << (prio * 2))) {
            EmitSemiObj(&m, prio, all);
        }
    }

    /* A first-target backdrop blends with itself (both line buffers start
     * with it). */
    if (m.alphaOn && (target1 & ST_BACKDROP) && (target2 & ST_BACKDROP)) {
        NtrPass p = Pass(NTR_SRC_COLOR, NTR_TEST_ALWAYS, 0);
        p.stencilTest = NTR_TEST_EQUAL;
        p.stencilRef = ST_BACKDROP;
        p.stencilMask = ST_LAYERS;
        p.color = sRGBA[Alpha(backdrop, backdrop, eva, evb)];
        Emit(&m, p, -1, 1, 0, all);
        p.color = sRGBA[Alpha(border, border, eva, evb)];
        Emit(&m, p, -1, 1, 1, all);
    }

    /* Brightness effects on first-target top pixels; master brightness. */
    if (bright) {
        NtrPass p = Pass(NTR_SRC_COLOR, NTR_TEST_ALWAYS, 0);
        p.color = (effect == 2 ? 0xffffff00u : 0) | (u32)(evy * 255 / 16);
        p.blend = NTR_BLEND_SRC_ALPHA;
        if ((present & target1) == present) {
            Emit(&m, p, -1, 1, -1, all);
        } else {
            for (int layer = 0; layer < 6; layer++) {
                if (present & target1 & (1u << layer)) {
                    p.stencilTest = NTR_TEST_EQUAL;
                    p.stencilRef = (u8)(1 << layer);
                    p.stencilMask = ST_LAYERS;
                    Emit(&m, p, -1, 1, -1, all);
                }
            }
        }
    }
    if ((mbMode == 1 || mbMode == 2) && mbFactor != 0) {
        NtrPass p = Pass(NTR_SRC_COLOR, NTR_TEST_ALWAYS, 0);
        p.color = (mbMode == 1 ? 0xffffff00u : 0) | (u32)(mbFactor * 255 / 16);
        p.blend = NTR_BLEND_SRC_ALPHA;
        Emit(&m, p, -1, -1, -1, all);
    }
    gNtrRenderTicks[engine][2] += STAGE_TICKS() - t1;
    gNtrExtTicks[3] += STAGE_TICKS() - r0;
    if (c->overflow) {
        gNtrComposeFallback = "too many passes";
    }
    return !c->overflow;
}

static void InitRGBA(void)
{
    for (u32 c = 0; c < 0x8000; c++) {
        sRGBA[c] = ToRGBA((u16)c);
    }
}

BOOL Ntr_MainEngineOnTop(void)
{
    return (NTR_IO16(0x304) & 0x8000) != 0; /* POWCNT display swap */
}

/* Output sizes: [0] top screen, [1] bottom screen. */
static int sScreenW[2] = { 256, 256 }, sScreenH[2] = { 192, 192 };

void Ntr_SetScreenSizes(int topW, int topH, int bottomW, int bottomH)
{
    sScreenW[0] = topW, sScreenH[0] = topH;
    sScreenW[1] = bottomW, sScreenH[1] = bottomH;
}

void Ntr_EngineScreenSize(int engine, int *w, int *h)
{
    int screen = (engine == 0) == Ntr_MainEngineOnTop() ? 0 : 1;
    *w = sScreenW[screen];
    *h = sScreenH[screen];
}

void Ntr_TrackFrame(void)
{
    for (int engine = 0; engine < 2; engine++) {
        Snapshot(&gNtrEngines[engine], &sEngs[engine]);
        TrackScroll(engine, &sEngs[engine]);
    }
}

void Ntr_SetOutputRGBA5551(BOOL enable)
{
    sOut16 = enable;
}

void Ntr_RenderEngine(int engine, void *out, int stride)
{
    if (sRGBA[0x7fff] == 0) {
        InitRGBA();
    }
    int w, h;
    Ntr_EngineScreenSize(engine, &w, &h);
    Snapshot(&gNtrEngines[engine], &sEngs[engine]);
    TrackScroll(engine, &sEngs[engine]);
    RenderEngine(&sEngs[engine], engine == 0, out, w, h, stride ? stride : w);
}

void Ntr_RenderScreens(u32 *top, u32 *bottom)
{
    BOOL mainOnTop = Ntr_MainEngineOnTop();
    Ntr_RenderEngine(0, mainOnTop ? top : bottom, 0);
    Ntr_RenderEngine(1, mainOnTop ? bottom : top, 0);
}
