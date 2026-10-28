/*
 * Software stand-in for the GPU in GPU composition: runs the passes of an
 * NtrComposition the way the 3DS GPU does (alpha test, stencil test and
 * write, 8-bit blending), so the desktop harness can check them against the
 * software compositor (--gpu-compose).
 */
#include <stdio.h>
#include <stdlib.h>

#include "compose_ref.h"

static inline u32 Expand5551(u16 v)
{
    u32 r = v >> 11, g = (v >> 6) & 31, b = (v >> 1) & 31;
    return ((r << 3 | r >> 2) << 24) | ((g << 3 | g >> 2) << 16) | ((b << 3 | b >> 2) << 8) | ((v & 1) ? 0xff : 0);
}

static inline BOOL Test(int func, int a, int b)
{
    switch (func) {
    case NTR_TEST_EQUAL:
        return a == b;
    case NTR_TEST_NOTEQUAL:
        return a != b;
    case NTR_TEST_LESS:
        return a < b;
    case NTR_TEST_GREATER:
        return a > b;
    default:
        return TRUE;
    }
}

static inline u32 Mix(u32 s, u32 d, u32 fs, u32 fd)
{
    u32 out = 0;
    for (int sh = 8; sh < 32; sh += 8) {
        u32 v = (((s >> sh) & 0xff) * fs + ((d >> sh) & 0xff) * fd + 127) / 255;
        out |= (v > 255 ? 255 : v) << sh;
    }
    return out | 0xff;
}

static u32 Fetch(const NtrComposition *c, const NtrLayerImages *img, const u32 *img3d, int w3d, int h3d, int stride3d, int source,
    u32 color, int x, int y)
{
    int lx = x, ly = y;
    switch (source) {
    case NTR_SRC_BG0:
    case NTR_SRC_BG1:
    case NTR_SRC_BG2:
    case NTR_SRC_BG3:
        return Expand5551(img->bg[source - NTR_SRC_BG0][ly * img->stride + lx]);
    case NTR_SRC_OBJ:
        return img->obj[ly * img->stride + lx];
    case NTR_SRC_3D: {
        int tx = x + c->ofs3D;
        return (tx >= 0 && tx < w3d && y < h3d) ? img3d[y * stride3d + tx] : 0;
    }
    default:
        return color;
    }
}

void ComposeRef_Run(const NtrComposition *c, const NtrLayerImages *img, const u32 *img3d, int w3d, int h3d, int stride3d, u32 *out, int outStride)
{
    static u8 stencil[NTR_MAX_SCREEN_W * NTR_MAX_SCREEN_H];
    for (int i = 0; i < c->numPasses; i++) {
        const NtrPass *p = &c->passes[i];
        u32 eva8 = (p->eva * 255 + 8) / 16, evb8 = (p->evb * 255 + 8) / 16;
        for (int k = 0; k < p->numRects; k++) {
            const NtrRect *r = &c->rects[p->firstRect + k];
            for (int y = r->y0; y < r->y1; y++) {
                for (int x = r->x0; x < r->x1; x++) {
                    u32 src = Fetch(c, img, img3d, w3d, h3d, stride3d, p->source, p->color, x, y);
                    u32 a = src & 0xff;
                    u8 *st = &stencil[y * c->outW + x];
                    if (!Test(p->alphaTest, (int)a, p->alphaRef) || !Test(p->stencilTest, p->stencilRef & p->stencilMask, *st & p->stencilMask)) {
                        continue;
                    }
                    if (p->objWindow && (img->objWin[y * img->stride + x] != 0) != (p->objWindow == 1)) {
                        continue; /* the other part of the OBJ window */
                    }
#ifdef NTR_DEBUG_HOOKS
                    {
                        static int px = -2, py;
                        if (px == -2) {
                            px = -1;
                            if (getenv("PORT_COMPOSE_PROBE")) {
                                sscanf(getenv("PORT_COMPOSE_PROBE"), "%d,%d", &px, &py);
                            }
                        }
                        if (x == px && y == py) {
                            printf("  pass %d: src %d below %d blend %d eva %d evb %d st %d/%02x/%02x write %02x objwin %d color %d: src %08x dst %08x\n", i,
                                p->source, p->below, p->blend, p->eva, p->evb, p->stencilTest, p->stencilRef, p->stencilMask, p->stencilWrite,
                                p->objWindow, p->colorWrite, src, out[y * outStride + x]);
                        }
                    }
#endif
                    if (p->colorWrite) {
                        u32 *d = &out[y * outStride + x];
                        u32 below = p->below != NTR_SRC_COLOR ? Fetch(c, img, img3d, w3d, h3d, stride3d, p->below, 0, x, y) : *d;
                        switch (p->blend) {
                        case NTR_BLEND_EVA_EVB:
                            *d = Mix(src, below, eva8, evb8);
                            break;
                        case NTR_BLEND_SRC_ALPHA:
                            *d = Mix(src, below, a, 255 - a);
                            break;
                        default:
                            *d = src | 0xff;
                            break;
                        }
                    }
                    if (p->stencilWrite) {
                        *st = (u8)((*st & ~p->stencilWrite) | (p->stencilRef & p->stencilWrite));
                    }
                }
            }
        }
    }
}
