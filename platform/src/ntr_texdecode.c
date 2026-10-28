/*
 * Whole DS textures decoded at once, for GPU renderers: every texel as
 * NtrRaster_TexelAt reads it, in RGBA8 (r << 24 | g << 16 | b << 8 | a, the
 * 5-bit channels widened as (c << 3) | (c >> 2), alpha as a * 255 / 31), in
 * the 3DS GPU's tiled order (8x8 tiles in rows, Morton order inside).
 *
 * The palette formats look their texels up in a table of the palette's
 * colors made once per texture, rather than reading and converting the
 * palette for every texel.
 */
#include <string.h>

#include "ntr_g3_internal.h"

#define TEXVRAM_SIZE 0x80000
#define TEXPLTT_SIZE 0x18000

/* Morton bits of a texel's x and y inside its 8x8 tile. */
static const u8 sMortonX[8] = { 0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15 };
static const u8 sMortonY[8] = { 0x00, 0x02, 0x08, 0x0a, 0x20, 0x22, 0x28, 0x2a };

static inline u16 Rd16(const u8 *base, u32 size, u32 addr)
{
    return addr + 1 < size ? (u16)(base[addr] | base[addr + 1] << 8) : 0;
}

static inline u32 Rgba(u16 c, u32 alpha)
{
    u32 r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    r = (r << 3) | (r >> 2);
    g = (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);
    return r << 24 | g << 16 | b << 8 | (alpha * 255 / 31);
}

static inline u16 Pltt(u32 addr)
{
    return Rd16(gNtrTexPltt, TEXPLTT_SIZE, addr);
}

/* Texels of a palette format: `bits` per texel, looked up in lut. */
static void DecodeIndexed(u32 addr, u32 w, u32 y0, u32 y1, int bits, const u32 *lut, u32 *dst)
{
    const u8 *vram = gNtrTexVram;
    u32 perByte = 8 / (u32)bits, mask = (1u << bits) - 1;
    u32 tilesPerRow = w >> 3;
    u32 rowBytes = w / perByte;
    if (addr + y1 * rowBytes <= TEXVRAM_SIZE) {
        /* Not wrapping around texture memory: eight texels at a time, to
         * their places in the tile (sMortonX: 0, 1, 4, 5, 16, 17, 20, 21). */
        for (u32 y = y0; y < y1; y++) {
            const u8 *src = vram + addr + y * rowBytes;
            u32 *o = dst + (y >> 3) * tilesPerRow * 64 + sMortonY[y & 7];
            for (u32 t = 0; t < tilesPerRow; t++, o += 64) {
                if (bits == 8) {
                    const u8 *b = src + t * 8;
                    o[0] = lut[b[0]], o[1] = lut[b[1]], o[4] = lut[b[2]], o[5] = lut[b[3]];
                    o[16] = lut[b[4]], o[17] = lut[b[5]], o[20] = lut[b[6]], o[21] = lut[b[7]];
                } else if (bits == 4) {
                    u32 v;
                    memcpy(&v, src + t * 4, 4);
                    o[0] = lut[v & 15], o[1] = lut[(v >> 4) & 15], o[4] = lut[(v >> 8) & 15], o[5] = lut[(v >> 12) & 15];
                    o[16] = lut[(v >> 16) & 15], o[17] = lut[(v >> 20) & 15], o[20] = lut[(v >> 24) & 15], o[21] = lut[v >> 28];
                } else {
                    u32 v = src[t * 2] | src[t * 2 + 1] << 8;
                    o[0] = lut[v & 3], o[1] = lut[(v >> 2) & 3], o[4] = lut[(v >> 4) & 3], o[5] = lut[(v >> 6) & 3];
                    o[16] = lut[(v >> 8) & 3], o[17] = lut[(v >> 10) & 3], o[20] = lut[(v >> 12) & 3], o[21] = lut[v >> 14];
                }
            }
        }
        return;
    }
    for (u32 y = y0; y < y1; y++) {
        u32 *row = dst + (y >> 3) * tilesPerRow * 64 + sMortonY[y & 7];
        u32 idx = y * w;
        for (u32 x = 0; x < w; x += perByte, idx += perByte) {
            u32 b = vram[(addr + idx / perByte) & 0x7ffff];
            for (u32 k = 0; k < perByte; k++) {
                u32 xx = x + k;
                row[(xx >> 3) * 64 + sMortonX[xx & 7]] = lut[(b >> (k * (u32)bits)) & mask];
            }
        }
    }
}

static void DecodeDirect(u32 addr, u32 w, u32 y0, u32 y1, u32 *dst)
{
    u32 tilesPerRow = w >> 3;
    for (u32 y = y0; y < y1; y++) {
        u32 *row = dst + (y >> 3) * tilesPerRow * 64 + sMortonY[y & 7];
        for (u32 x = 0; x < w; x++) {
            u16 c = Rd16(gNtrTexVram, TEXVRAM_SIZE, (addr + (y * w + x) * 2) & 0x7ffff);
            row[(x >> 3) * 64 + sMortonX[x & 7]] = Rgba(c & 0x7fff, (c & 0x8000) ? 31 : 0);
        }
    }
}

/* 4x4 blocks: two bits per texel choose one of four colors that the block's
 * palette entry and mode make. */
static void DecodeComp4x4(u32 addr, u32 palBase, u32 w, u32 y0, u32 y1, u32 *dst)
{
    const u8 *vram = gNtrTexVram;
    u32 blocksPerRow = w >> 2, tilesPerRow = w >> 3;
    for (u32 by = y0 >> 2; by < y1 >> 2; by++) {
        for (u32 bx = 0; bx < blocksPerRow; bx++) {
            u32 block = by * blocksPerRow + bx;
            u32 wordAddr = (addr + block * 4) & 0x7ffff;
            u32 word = vram[wordAddr] | vram[wordAddr + 1] << 8 | vram[wordAddr + 2] << 16 | (u32)vram[wordAddr + 3] << 24;
            u32 idxAddr = 0x20000 + ((addr & 0x1ffff) >> 1) + ((addr & 0x40000) >> 2) + block * 2;
            u16 info = Rd16(vram, TEXVRAM_SIZE, idxAddr);
            u32 pal = palBase + ((info & 0x3fffu) << 2);
            int mode = info >> 14;
            u16 c0 = Pltt(pal), c1 = Pltt(pal + 2);
            u32 colors[4];
            colors[0] = Rgba(c0, 31);
            colors[1] = Rgba(c1, 31);
            for (int texel = 2; texel < 4; texel++) {
                if (mode == 2 || (mode == 0 && texel == 2)) {
                    colors[texel] = Rgba(Pltt(pal + (u32)texel * 2), 31);
                } else if (texel == 3 && mode < 2) {
                    colors[texel] = 0;
                } else {
                    int w0 = mode == 1 ? 4 : (texel == 2 ? 5 : 3);
                    int w1 = 8 - w0;
                    int r = (((c0 & 31) * w0) + ((c1 & 31) * w1)) / 8;
                    int g = ((((c0 >> 5) & 31) * w0) + (((c1 >> 5) & 31) * w1)) / 8;
                    int b = ((((c0 >> 10) & 31) * w0) + (((c1 >> 10) & 31) * w1)) / 8;
                    colors[texel] = Rgba((u16)(r | g << 5 | b << 10), 31);
                }
            }
            for (u32 v = 0; v < 4; v++) {
                u32 y = by * 4 + v;
                u32 *row = dst + (y >> 3) * tilesPerRow * 64 + sMortonY[y & 7];
                for (u32 u = 0; u < 4; u++) {
                    u32 x = bx * 4 + u;
                    row[(x >> 3) * 64 + sMortonX[x & 7]] = colors[(word >> (v * 8 + u * 2)) & 3];
                }
            }
        }
    }
}

void NtrTex_DecodeTiled(u32 image, u32 pltt, u32 *dst)
{
    NtrTex_DecodeTiledRows(image, pltt, dst, 0, 8u << ((image >> 23) & 7));
}

void NtrTex_DecodeTiledRows(u32 image, u32 pltt, u32 *dst, u32 y0, u32 y1)
{
    int fmt = (image >> 26) & 7;
    u32 w = 8u << ((image >> 20) & 7);
    u32 addr = (image & 0xffff) << 3;
    BOOL color0Transparent = (image >> 29) & 1;
    u32 palBase = pltt << 4;
    u32 lut[256];

    switch (fmt) {
    case GX_TEXFMT_A3I5:
        for (u32 b = 0; b < 256; b++) {
            u32 a3 = b >> 5;
            lut[b] = Rgba(Pltt(palBase + (b & 31) * 2), a3 * 4 + a3 / 2);
        }
        DecodeIndexed(addr, w, y0, y1, 8, lut, dst);
        break;
    case GX_TEXFMT_A5I3:
        for (u32 b = 0; b < 256; b++) {
            lut[b] = Rgba(Pltt(palBase + (b & 7) * 2), b >> 3);
        }
        DecodeIndexed(addr, w, y0, y1, 8, lut, dst);
        break;
    case GX_TEXFMT_PLTT4:
        for (u32 i = 0; i < 4; i++) {
            lut[i] = Rgba(Pltt((pltt << 3) + i * 2), i == 0 && color0Transparent ? 0 : 31);
        }
        DecodeIndexed(addr, w, y0, y1, 2, lut, dst);
        break;
    case GX_TEXFMT_PLTT16:
        for (u32 i = 0; i < 16; i++) {
            lut[i] = Rgba(Pltt(palBase + i * 2), i == 0 && color0Transparent ? 0 : 31);
        }
        DecodeIndexed(addr, w, y0, y1, 4, lut, dst);
        break;
    case GX_TEXFMT_PLTT256:
        for (u32 i = 0; i < 256; i++) {
            lut[i] = Rgba(Pltt(palBase + i * 2), i == 0 && color0Transparent ? 0 : 31);
        }
        DecodeIndexed(addr, w, y0, y1, 8, lut, dst);
        break;
    case GX_TEXFMT_DIRECT:
        DecodeDirect(addr, w, y0, y1, dst);
        break;
    case GX_TEXFMT_COMP4x4:
        DecodeComp4x4(addr, palBase, w, y0, y1, dst);
        break;
    default:
        for (u32 i = y0 * w; i < y1 * w; i++) {
            dst[i] = Rgba(0x7fff, 31);
        }
        break;
    }
}
