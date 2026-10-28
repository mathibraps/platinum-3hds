/* Shared between the geometry engine (ntr_g3.c) and the rasterizer (ntr_raster.c). */
#ifndef NTR_G3_INTERNAL_H
#define NTR_G3_INTERNAL_H

#include "ntr_shim.h"

#define NTR_G3_MAX_POLYS 2048
#define NTR_G3_MAX_VERTS 6144
#define NTR_G3_MAX_POLY_VERTS 10 /* a quad clipped by all six planes */

/* A vertex after transform and clipping: clip-space position, 5-bit color,
 * texcoord in 1/16 texels. */
typedef struct NtrG3Vertex {
    float c[3];
    float w;
    float color[3];
    float s, t;
} NtrG3Vertex;

typedef struct NtrG3Polygon {
    int numVerts;
    int firstVert;
    u32 polyAttr;
    u32 texImage;
    u32 texPltt;
    u32 viewport;
    BOOL front;
    float wideX, wideY; /* clip space x/y scale for widescreen (1 = DS) */
} NtrG3Polygon;

typedef struct NtrG3PolyList {
    int count;
    int vertexCount;
    NtrG3Polygon polys[NTR_G3_MAX_POLYS];
    NtrG3Vertex verts[NTR_G3_MAX_VERTS];
} NtrG3PolyList;

extern NtrG3PolyList gNtrG3Polys;

/* Where a vertex lands on the 3D output (outW x outH, the DS 256x192 in the
 * middle): x from the left and y from the bottom, in pixels. Clip x/y were
 * scaled by wideX/wideY, so full-screen geometry fills the wider output and
 * other viewports keep their DS place. */
static inline void NtrG3_OutputPos(const NtrG3Polygon *p, const NtrG3Vertex *v, int outW, int outH, float *x, float *yUp)
{
    int vx1 = p->viewport & 0xff, vy1 = (p->viewport >> 8) & 0xff;
    int vx2 = (p->viewport >> 16) & 0xff, vy2 = (p->viewport >> 24) & 0xff;
    float vw = (float)(vx2 - vx1 + 1), vh = (float)(vy2 - vy1 + 1);
    float w = v->w > 1e-6f ? v->w : 1e-6f;
    *x = (v->c[0] / (w * p->wideX) + 1.0f) * vw * 0.5f + vx1 + (outW - 256) * 0.5f;
    *yUp = (v->c[1] / (w * p->wideY) + 1.0f) * vh * 0.5f + vy1 + (outH - 192) * 0.5f;
}

/* Output size of the 3D frame being built (the main engine's screen). */
void NtrG3_OutputSize(int *w, int *h);

/* Executes one geometry command whose parameters are complete. */
void NtrGe_Execute(u32 op, const u32 *params);

/* Renders a polygon list into gNtr3DColor/gNtr3DAlpha (SWAP_BUFFERS parameter). */
void NtrRaster_Render(const NtrG3PolyList *list, u32 swapParam);

/* Another 3D renderer (the 3DS GPU) can take over from the software
 * rasterizer: it is then called instead, with the same arguments, and must
 * fill gNtr3DColor/gNtr3DAlpha before the frame is composited. */
extern void (*gNtrRender3DHook)(const NtrG3PolyList *list, u32 swapParam);

/* Texel (u, v) of a DS texture (TEXIMAGE_PARAM, PLTT_BASE), u and v in range:
 * RGB555, alpha 0..31. */
u16 NtrRaster_TexelAt(u32 image, u32 pltt, int u, int v, int *alpha);

/* A whole texture, every texel as NtrRaster_TexelAt reads it, in RGBA8
 * (r << 24 | g << 16 | b << 8 | a) and the 3DS GPU's tiled order
 * (ntr_texdecode.c): width * height words at dst. */
void NtrTex_DecodeTiled(u32 image, u32 pltt, u32 *dst);
/* The same for rows [y0, y1) only (multiples of 8: whole rows of tiles). */
void NtrTex_DecodeTiledRows(u32 image, u32 pltt, u32 *dst, u32 y0, u32 y1);

#endif
