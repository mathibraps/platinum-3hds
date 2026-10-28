/*
 * Emulated DS 3D geometry engine.
 *
 * Executes geometry commands (from SDK calls, writes to the command FIFO and
 * display lists) the way the hardware does: matrix stacks, vertex transform,
 * lighting, texture coordinate generation, primitive assembly, culling and
 * clipping. Finished polygons collect in the polygon list, which
 * ntr_raster.c renders on SWAP_BUFFERS.
 *
 * Matrices are 20.12 fixed point, row-vector convention (v' = v * M), and
 * multiplications use the hardware's 64-bit accumulate then >> 12.
 */
#include <stdio.h>
#include <math.h>
#include <stdlib.h>

#include "ntr_g3_internal.h"
#include "ntr_shim.h"

u64 gNtrRasterTicks;

/* 3D control/status registers live in the emulated I/O image. */
#define DISP3DCNT NTR_IO16(0x060)
#define GXSTAT NTR_IO32(0x600)
#define LISTRAM_COUNT NTR_IO16(0x604)
#define VTXRAM_COUNT NTR_IO16(0x606)

typedef struct Mtx {
    s32 m[4][4];
} Mtx;

/* A vertex of the primitive being assembled, with what polygons that share
 * it need: which sides of the view it is beyond (CLIP_*), and its projected
 * position once a polygon worked it out. */
enum { CLIP_LEFT = 1, CLIP_RIGHT = 2, CLIP_BOTTOM = 4, CLIP_TOP = 8, CLIP_NEAR = 16, CLIP_FAR = 32 };

typedef struct RingVertex {
    NtrG3Vertex v;
    u32 clip;
    BOOL projected;
    float px, py;
} RingVertex;

static struct {
    int mode;
    Mtx proj, projStack[1];
    int projSp;
    Mtx pos, vec, posStack[32], vecStack[32];
    int posSp;
    Mtx tex, texStack[1];
    int texSp;
    BOOL clipDirty;
    Mtx clip;

    s32 vx, vy, vz; /* last vertex (fx12) */
    u8 color[3]; /* vertex color, 5 bits per component */
    s16 s, t; /* texcoord, 1/16 texel */
    s32 genS, genT; /* texcoord after texgen */

    u32 polyAttrPending, polyAttr;
    u32 texImage, texPltt;
    u16 diffuse, ambient, specular, emission;
    BOOL shininessTable;
    u8 shininess[128];
    s32 lightDir[4][3]; /* transformed light vectors (fx12) */
    u16 lightColor[4];
    u32 viewport;
    float wideX, wideY; /* widescreen clip space scale (NtrG3Polygon) */

    int primType;
    int primCount; /* vertices submitted since BEGIN */
    RingVertex ring[4]; /* the last four vertices: as many as a primitive uses */
    int ringNext;
    RingVertex *strip[4]; /* the vertices of the primitive being assembled */
    int stripLen;

    VecFx32 posTestResult;
    fx32 posTestW;
    fx16 vecTestResult[3];
} ge;

NtrG3PolyList gNtrG3Polys;

/* Matrix helpers -------------------------------------------------------------- */

static void MtxIdentity(Mtx *m)
{
    memset(m, 0, sizeof(*m));
    for (int i = 0; i < 4; i++) {
        m->m[i][i] = FX32_ONE;
    }
}

/* r = a * b */
static void MtxMul(const Mtx *a, const Mtx *b, Mtx *r)
{
    Mtx t;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            s64 sum = 0;
            for (int k = 0; k < 4; k++) {
                sum += (s64)a->m[i][k] * b->m[k][j];
            }
            t.m[i][j] = (s32)(sum >> 12);
        }
    }
    *r = t;
}

static Mtx *CurrentMtx(void)
{
    switch (ge.mode) {
    case GX_MTXMODE_PROJECTION:
        return &ge.proj;
    case GX_MTXMODE_TEXTURE:
        return &ge.tex;
    default:
        return &ge.pos;
    }
}

/* Applies "load" or "multiply by m" to the matrices the current mode targets. */
static void ApplyMtx(const Mtx *m, BOOL multiply, BOOL scaleOnly)
{
    if (ge.mode == GX_MTXMODE_POSITION_VECTOR) {
        if (multiply) {
            MtxMul(m, &ge.pos, &ge.pos);
            if (!scaleOnly) {
                MtxMul(m, &ge.vec, &ge.vec);
            }
        } else {
            ge.pos = *m;
            ge.vec = *m;
        }
    } else {
        Mtx *cur = CurrentMtx();
        if (multiply) {
            MtxMul(m, cur, cur);
        } else {
            *cur = *m;
        }
    }
    ge.clipDirty = TRUE;
}

static inline const Mtx *ClipMtx(void)
{
    if (ge.clipDirty) {
        MtxMul(&ge.pos, &ge.proj, &ge.clip);
        ge.clipDirty = FALSE;
    }
    return &ge.clip;
}

void NtrG3_OutputSize(int *w, int *h)
{
    Ntr_EngineScreenSize(0, w, h);
}

/* Widescreen: geometry drawn with the full-screen viewport is projected onto
 * the whole output, keeping DS pixel scale, so clip space x/y shrink by
 * 256/width and 192/height (and more of the scene passes clipping). Other
 * viewports keep their DS size and place. */
static void UpdateWide(void)
{
    int w, h;
    NtrG3_OutputSize(&w, &h);
    if (ge.viewport == 0xbfff0000u) {
        ge.wideX = 256.0f / (float)w;
        ge.wideY = 192.0f / (float)h;
    } else {
        ge.wideX = ge.wideY = 1.0f;
    }
}

/* x / 31, rounding toward zero like C division, without a divide (the ARM11
 * has no divide instruction): exact for every 32-bit value. */
static inline s32 Div31(s32 x)
{
    u32 a = x < 0 ? (u32)-(s64)x : (u32)x;
    u32 q = (u32)(((u64)a * 0x84210843u) >> 36);
    return x < 0 ? -(s32)q : (s32)q;
}

static void Transform(const Mtx *m, s32 x, s32 y, s32 z, s32 w, s32 out[4])
{
    for (int j = 0; j < 4; j++) {
        s64 sum = (s64)x * m->m[0][j] + (s64)y * m->m[1][j] + (s64)z * m->m[2][j] + (s64)w * m->m[3][j];
        out[j] = (s32)(sum >> 12);
    }
}

static s32 Sext(u32 v, int bits)
{
    return (s32)(v << (32 - bits)) >> (32 - bits);
}

/* Lighting --------------------------------------------------------------------- */

/* The terms that only change with the material, the light colors and which
 * lights are on: the emission plus each light's ambient term, and each
 * light's diffuse and specular color products. Worked out again when one of
 * those changes rather than for every normal. */
static struct {
    BOOL dirty;
    u32 lights; /* the enabled lights the terms are for */
    s32 base[3];
    u32 difLc[4][3], speLc[4][3];
} sLit = { TRUE };

/* x / 31 for x >= 0 (as Div31). */
static inline u32 Div31u(u32 x)
{
    return (u32)(((u64)x * 0x84210843u) >> 36);
}

static void UpdateLighting(u32 lights)
{
    int dif[3] = { (ge.diffuse >> 0) & 31, (ge.diffuse >> 5) & 31, (ge.diffuse >> 10) & 31 };
    int amb[3] = { (ge.ambient >> 0) & 31, (ge.ambient >> 5) & 31, (ge.ambient >> 10) & 31 };
    int spe[3] = { (ge.specular >> 0) & 31, (ge.specular >> 5) & 31, (ge.specular >> 10) & 31 };
    for (int k = 0; k < 3; k++) {
        sLit.base[k] = ((ge.emission >> (k * 5)) & 31) << 12;
    }
    for (int i = 0; i < 4; i++) {
        for (int k = 0; k < 3; k++) {
            int lc = (ge.lightColor[i] >> (k * 5)) & 31;
            sLit.difLc[i][k] = (u32)(dif[k] * lc);
            sLit.speLc[i][k] = (u32)(spe[k] * lc);
            if (lights & (1u << i)) {
                sLit.base[k] += (s32)Div31u((u32)(amb[k] * lc) << 12);
            }
        }
    }
    sLit.lights = lights;
    sLit.dirty = FALSE;
}

static void Light(s32 nx, s32 ny, s32 nz)
{
    u32 lights = ge.polyAttr & 15;
    if (sLit.dirty || sLit.lights != lights) {
        UpdateLighting(lights);
    }
    const Mtx *v = &ge.vec;
    s32 n[3];
    for (int j = 0; j < 3; j++) {
        n[j] = (s32)(((s64)nx * v->m[0][j] + (s64)ny * v->m[1][j] + (s64)nz * v->m[2][j]) >> 12);
    }

    s32 acc[3] = { sLit.base[0], sLit.base[1], sLit.base[2] };
    for (int i = 0; i < 4; i++) {
        if (!(lights & (1u << i))) {
            continue;
        }
        const s32 *l = ge.lightDir[i];
        s32 diffuse = -(s32)(((s64)l[0] * n[0] + (s64)l[1] * n[1] + (s64)l[2] * n[2]) >> 12);
        if (diffuse < 0) {
            diffuse = 0;
        }
        /* Half vector between the light and the line of sight (0, 0, -1). */
        s32 h[3] = { l[0] / 2, l[1] / 2, (l[2] - FX32_ONE) / 2 };
        s32 shine = -(s32)(((s64)h[0] * n[0] + (s64)h[1] * n[1] + (s64)h[2] * n[2]) >> 12);
        if (shine < 0) {
            shine = 0;
        }
        shine = (s32)(((s64)shine * shine) >> 12);
        if (shine > FX32_ONE) {
            shine = FX32_ONE;
        }
        if (ge.shininessTable) {
            int idx = shine >> 5;
            if (idx > 127) {
                idx = 127;
            }
            shine = ge.shininess[idx] << 4;
        }
        for (int k = 0; k < 3; k++) {
            acc[k] += (s32)(Div31u(sLit.speLc[i][k] * (u32)shine) + Div31u(sLit.difLc[i][k] * (u32)diffuse));
        }
    }

    for (int k = 0; k < 3; k++) {
        int c = acc[k] >> 12;
        ge.color[k] = (u8)(c > 31 ? 31 : c);
    }
}

/* Texture coordinates ------------------------------------------------------------ */

static void TexCoordFromTexCoord(void)
{
    if ((ge.texImage >> 30) == GX_TEXGEN_TEXCOORD) {
        const Mtx *m = &ge.tex;
        ge.genS = (s32)(((s64)ge.s * m->m[0][0] + (s64)ge.t * m->m[1][0] + m->m[2][0] + m->m[3][0]) >> 12);
        ge.genT = (s32)(((s64)ge.s * m->m[0][1] + (s64)ge.t * m->m[1][1] + m->m[2][1] + m->m[3][1]) >> 12);
    } else {
        ge.genS = ge.s;
        ge.genT = ge.t;
    }
}

static void TexCoordFromVector(s32 x, s32 y, s32 z)
{
    const Mtx *m = &ge.tex;
    ge.genS = (s32)(((s64)x * m->m[0][0] + (s64)y * m->m[1][0] + (s64)z * m->m[2][0] + ((s64)ge.s << 12)) >> 12);
    ge.genT = (s32)(((s64)x * m->m[0][1] + (s64)y * m->m[1][1] + (s64)z * m->m[2][1] + ((s64)ge.t << 12)) >> 12);
}

/* Primitive assembly, culling and clipping -------------------------------------------- */

static int ClipAgainst(NtrG3Vertex *in, int n, NtrG3Vertex *out, int axis, int sign)
{
    int m = 0;
    for (int i = 0; i < n; i++) {
        NtrG3Vertex *a = &in[i], *b = &in[(i + 1) % n];
        float da = a->w - sign * a->c[axis];
        float db = b->w - sign * b->c[axis];
        if (da >= 0) {
            out[m++] = *a;
        }
        if ((da >= 0) != (db >= 0)) {
            float t = da / (da - db);
            NtrG3Vertex *v = &out[m++];
            for (int k = 0; k < 3; k++) {
                v->c[k] = a->c[k] + (b->c[k] - a->c[k]) * t;
                v->color[k] = a->color[k] + (b->color[k] - a->color[k]) * t;
            }
            v->w = a->w + (b->w - a->w) * t;
            v->s = a->s + (b->s - a->s) * t;
            v->t = a->t + (b->t - a->t) * t;
        }
    }
    return m;
}

static void EmitPolygon(RingVertex *const *pv, int n)
{
    /* Polygons entirely beyond one side of the view are dropped (beyond the
     * near or far plane they would be clipped away). Checked first, as most
     * of a scene's polygons are: the game draws whole map blocks around the
     * player. */
    u32 all = 0x3f, any = 0;
    for (int i = 0; i < n; i++) {
        all &= pv[i]->clip;
        any |= pv[i]->clip;
    }
    if (all != 0) {
        return;
    }

    /* Culling on the projected winding (y up): counter-clockwise is the front. */
    float area = 0;
    BOOL behind = FALSE;
    for (int i = 0; i < n; i++) {
        if (pv[i]->v.w <= 0) {
            behind = TRUE;
        }
    }
    if (!behind) {
        float px[4], py[4];
        for (int i = 0; i < n; i++) {
            RingVertex *r = pv[i];
            if (!r->projected) {
                float iw = 1.0f / r->v.w;
                r->px = r->v.c[0] * iw;
                r->py = r->v.c[1] * iw;
                r->projected = TRUE;
            }
            px[i] = r->px;
            py[i] = r->py;
        }
        for (int i = 0; i < n; i++) {
            int j = i + 1 < n ? i + 1 : 0;
            area += px[i] * py[j] - px[j] * py[i];
        }
    } else {
        /* Homogeneous winding test, valid with vertices behind the eye. */
        for (int i = 1; i + 1 < n; i++) {
            const NtrG3Vertex *a = &pv[0]->v, *b = &pv[i]->v, *c = &pv[i + 1]->v;
            area += a->c[0] * (b->c[1] * c->w - c->c[1] * b->w) - a->c[1] * (b->c[0] * c->w - c->c[0] * b->w) + a->w * (b->c[0] * c->c[1] - c->c[0] * b->c[1]);
        }
    }
    BOOL front = area > 0;
    if (area == 0) {
        return; /* degenerate */
    }
    if (front && !(ge.polyAttr & REG_G3_POLYGON_ATTR_FR_MASK)) {
        return;
    }
    if (!front && !(ge.polyAttr & REG_G3_POLYGON_ATTR_BK_MASK)) {
        return;
    }

    /* Polygons crossing the far plane are dropped unless far clipping is on. */
    if (!(ge.polyAttr & REG_G3_POLYGON_ATTR_FC_MASK) && (any & CLIP_FAR)) {
        return;
    }

    static NtrG3Vertex bufA[NTR_G3_MAX_POLY_VERTS], bufB[NTR_G3_MAX_POLY_VERTS];

    /* Opaque polygons are only clipped against the near and far planes here:
     * both rasterizers (the 3DS GPU and the software one) clip x and y
     * themselves, with the same result. Translucent polygons are clipped
     * fully, as their drawing order depends on the top of the clipped shape. */
    int alpha = (ge.polyAttr >> 16) & 31;
    int fmt = (ge.texImage >> 26) & 7;
    BOOL translucent = (alpha > 0 && alpha < 31) || ((DISP3DCNT & 1) && (fmt == GX_TEXFMT_A3I5 || fmt == GX_TEXFMT_A5I3));
    int lastAxis = translucent ? 0 : 2;
    BOOL inside = (any & (translucent ? 0x3fu : (CLIP_NEAR | CLIP_FAR))) == 0;

    const NtrG3Vertex *out[NTR_G3_MAX_POLY_VERTS];
    if (inside) {
        for (int i = 0; i < n; i++) {
            out[i] = &pv[i]->v;
        }
    } else {
        NtrG3Vertex *src = bufA, *dst = bufB;
        for (int i = 0; i < n; i++) {
            bufA[i] = pv[i]->v;
        }
        for (int axis = 2; axis >= lastAxis && n > 0; axis--) {
            for (int sign = 1; sign >= -1 && n > 0; sign -= 2) {
                n = ClipAgainst(src, n, dst, axis, sign);
                NtrG3Vertex *tmp = src;
                src = dst;
                dst = tmp;
            }
        }
        for (int i = 0; i < n; i++) {
            out[i] = &src[i];
        }
    }
    if (n < 3) {
        return;
    }

    NtrG3PolyList *list = &gNtrG3Polys;
    if (list->count >= NTR_G3_MAX_POLYS || list->vertexCount + n > NTR_G3_MAX_VERTS) {
        DISP3DCNT |= 1 << 13; /* polygon/vertex RAM overflow */
        return;
    }
    NtrG3Polygon *p = &list->polys[list->count++];
    p->numVerts = n;
    p->firstVert = list->vertexCount;
    p->polyAttr = ge.polyAttr;
    p->texImage = ge.texImage;
    p->texPltt = ge.texPltt;
    p->viewport = ge.viewport;
    p->front = front;
    p->wideX = ge.wideX;
    p->wideY = ge.wideY;
    for (int i = 0; i < n; i++) {
        list->verts[list->vertexCount + i] = *out[i];
    }
    list->vertexCount += n;
    LISTRAM_COUNT = (u16)list->count;
    VTXRAM_COUNT = (u16)list->vertexCount;
}

static void SubmitVertex(s32 x, s32 y, s32 z)
{
    ge.vx = x;
    ge.vy = y;
    ge.vz = z;

    if ((ge.texImage >> 30) == GX_TEXGEN_VERTEX) {
        TexCoordFromVector(x, y, z);
    }

    s32 c[4];
    Transform(ClipMtx(), x, y, z, FX32_ONE, c);

    /* Each primitive refers to at most the last four vertices, kept in a
     * ring (a quad strip's quad: the two before and the two newest). */
    RingVertex *v = &ge.ring[ge.ringNext];
    ge.ringNext = (ge.ringNext + 1) & 3;
    float cx = c[0] / 4096.0f * ge.wideX, cy = c[1] / 4096.0f * ge.wideY, cz = c[2] / 4096.0f, cw = c[3] / 4096.0f;
    v->v.c[0] = cx;
    v->v.c[1] = cy;
    v->v.c[2] = cz;
    v->v.w = cw;
    for (int k = 0; k < 3; k++) {
        v->v.color[k] = ge.color[k];
    }
    v->v.s = (float)ge.genS;
    v->v.t = (float)ge.genT;
    v->clip = (cx < -cw ? CLIP_LEFT : 0) | (cx > cw ? CLIP_RIGHT : 0) | (cy < -cw ? CLIP_BOTTOM : 0) | (cy > cw ? CLIP_TOP : 0)
        | (cz < -cw ? CLIP_NEAR : 0) | (cz > cw ? CLIP_FAR : 0);
    v->projected = FALSE;

    int i = ge.primCount++;
    switch (ge.primType) {
    case GX_BEGIN_TRIANGLES:
        ge.strip[ge.stripLen++] = v;
        if (ge.stripLen == 3) {
            EmitPolygon(ge.strip, 3);
            ge.stripLen = 0;
        }
        break;
    case GX_BEGIN_QUADS:
        ge.strip[ge.stripLen++] = v;
        if (ge.stripLen == 4) {
            EmitPolygon(ge.strip, 4);
            ge.stripLen = 0;
        }
        break;
    case GX_BEGIN_TRIANGLE_STRIP:
        if (i < 2) {
            ge.strip[i] = v;
        } else {
            RingVertex *tri[3];
            if (i & 1) {
                tri[0] = ge.strip[1];
                tri[1] = ge.strip[0];
            } else {
                tri[0] = ge.strip[0];
                tri[1] = ge.strip[1];
            }
            tri[2] = v;
            EmitPolygon(tri, 3);
            ge.strip[0] = ge.strip[1];
            ge.strip[1] = v;
        }
        break;
    case GX_BEGIN_QUAD_STRIP:
        if (i < 2) {
            ge.strip[i] = v;
        } else if (i & 1) {
            RingVertex *quad[4] = { ge.strip[0], ge.strip[1], v, ge.strip[2] };
            EmitPolygon(quad, 4);
            ge.strip[0] = ge.strip[2];
            ge.strip[1] = v;
        } else {
            ge.strip[2] = v;
        }
        break;
    }
}

/* Box / position / vector tests ------------------------------------------------- */

static void BoxTest(const u32 *p)
{
    UpdateWide();
    s32 x = Sext(p[0], 16), y = Sext(p[0] >> 16, 16), z = Sext(p[1], 16);
    s32 w = Sext(p[1] >> 16, 16), h = Sext(p[2], 16), d = Sext(p[2] >> 16, 16);
    int outsideAll[6] = { 1, 1, 1, 1, 1, 1 };

    for (int i = 0; i < 8; i++) {
        s32 c[4];
        Transform(ClipMtx(), x + ((i & 1) ? w : 0), y + ((i & 2) ? h : 0), z + ((i & 4) ? d : 0), FX32_ONE, c);
        /* Tested in widescreen clip space, so objects beside the DS picture are drawn. */
        float cw[3] = { c[0] * ge.wideX, c[1] * ge.wideY, (float)c[2] };
        for (int a = 0; a < 3; a++) {
            if (cw[a] >= -c[3]) {
                outsideAll[a * 2] = 0;
            }
            if (cw[a] <= c[3]) {
                outsideAll[a * 2 + 1] = 0;
            }
        }
    }
    BOOL visible = TRUE;
    for (int k = 0; k < 6; k++) {
        if (outsideAll[k]) {
            visible = FALSE;
        }
    }
    GXSTAT = (GXSTAT & ~2u) | (visible ? 2u : 0u);
}

/* Command execution ------------------------------------------------------------------ */

/* Parameter words per command. */
static const u8 sParamCount[256] = {
    [G3OP_MTX_MODE] = 1, [G3OP_MTX_POP] = 1, [G3OP_MTX_STORE] = 1, [G3OP_MTX_RESTORE] = 1,
    [G3OP_COLOR] = 1, [G3OP_NORMAL] = 1, [G3OP_TEXCOORD] = 1, [G3OP_VTX_10] = 1,
    [G3OP_VTX_XY] = 1, [G3OP_VTX_XZ] = 1, [G3OP_VTX_YZ] = 1, [G3OP_VTX_DIFF] = 1,
    [G3OP_POLYGON_ATTR] = 1, [G3OP_TEXIMAGE_PARAM] = 1, [G3OP_TEXPLTT_BASE] = 1,
    [G3OP_DIF_AMB] = 1, [G3OP_SPE_EMI] = 1, [G3OP_LIGHT_VECTOR] = 1, [G3OP_LIGHT_COLOR] = 1,
    [G3OP_BEGIN] = 1, [G3OP_SWAP_BUFFERS] = 1, [G3OP_VIEWPORT] = 1, [G3OP_VEC_TEST] = 1,
    [G3OP_VTX_16] = 2, [G3OP_POS_TEST] = 2,
    [G3OP_MTX_SCALE] = 3, [G3OP_MTX_TRANS] = 3, [G3OP_BOX_TEST] = 3,
    [G3OP_MTX_MULT_3x3] = 9,
    [G3OP_MTX_LOAD_4x3] = 12, [G3OP_MTX_MULT_4x3] = 12,
    [G3OP_MTX_LOAD_4x4] = 16, [G3OP_MTX_MULT_4x4] = 16,
    [G3OP_SHININESS] = 32,
};

static inline int ParamCount(u32 op)
{
    return sParamCount[op & 0xff];
}

static void LoadParams(Mtx *m, const u32 *p, int rows, int cols)
{
    MtxIdentity(m);
    for (int i = 0; i < rows; i++) {
        for (int j = 0; j < cols; j++) {
            m->m[i][j] = (s32)p[i * cols + j];
        }
    }
}

#ifdef NTR_DEBUG_HOOKS
/* Debugging (desktop harness, PORT_GESTATS): commands executed, by opcode,
 * and how they arrived: [0] whole in a FeedFifo block, [1] word by word,
 * [2] command ports, [3] SDK calls. */
u32 gNtrGeOpCount[256];
u32 gNtrGePath[4];
u32 gNtrGeFifoWords, gNtrGeFifoOps[256], gNtrGeFifoFromBlocks; /* words and commands that came one word at a time */
#define GE_PATH(i) (gNtrGePath[i]++)
#define GE_COUNT(op) (gNtrGeOpCount[op]++, gNtrGePath[0]++)
#else
#define GE_PATH(i) ((void)0)
#define GE_COUNT(op) ((void)0)
#endif

void NtrGe_Execute(u32 op, const u32 *p)
{
    Mtx m;
#ifdef NTR_DEBUG_HOOKS
    gNtrGeOpCount[op & 0xff]++;
    if (getenv("PORT_TEXMTX")) {
        /* Texture matrix commands and texture coordinates, in a range of frames. */
        u32 f = Ntr_FramesEnded(), lo = 0, hi = 0;
        sscanf(getenv("PORT_TEXMTX"), "%u,%u", &lo, &hi);
        if (f >= lo && f <= hi && ((ge.mode == GX_MTXMODE_TEXTURE && op >= 0x11 && op <= 0x1c) || op == G3OP_TEXCOORD || op == G3OP_TEXIMAGE_PARAM
                || op == G3OP_MTX_MODE)) {
            printf("frame %u op %02x:", f, op);
            for (int k = 0; k < ParamCount(op); k++) {
                printf(" %08x", p[k]);
            }
            printf("  (tex m00 %d m11 %d m20 %d m21 %d m30 %d m31 %d)\n", ge.tex.m[0][0], ge.tex.m[1][1], ge.tex.m[2][0], ge.tex.m[2][1],
                ge.tex.m[3][0], ge.tex.m[3][1]);
        }
    }
#endif

    switch (op) {
    case G3OP_MTX_MODE:
        ge.mode = p[0] & 3;
        break;
    case G3OP_MTX_PUSH:
        if (ge.mode == GX_MTXMODE_PROJECTION) {
            ge.projStack[0] = ge.proj;
            ge.projSp = 1;
        } else if (ge.mode == GX_MTXMODE_TEXTURE) {
            ge.texStack[0] = ge.tex;
            ge.texSp = 1;
        } else {
            if (ge.posSp < 31) {
                ge.posStack[ge.posSp] = ge.pos;
                ge.vecStack[ge.posSp] = ge.vec;
            }
            ge.posSp++;
            if (ge.posSp > 31) {
                GXSTAT |= 1u << 15; /* stack overflow */
                ge.posSp = 31;
            }
        }
        break;
    case G3OP_MTX_POP:
        if (ge.mode == GX_MTXMODE_PROJECTION) {
            ge.proj = ge.projStack[0];
            ge.projSp = 0;
        } else if (ge.mode == GX_MTXMODE_TEXTURE) {
            ge.tex = ge.texStack[0];
            ge.texSp = 0;
        } else {
            ge.posSp -= Sext(p[0], 6);
            if (ge.posSp < 0) {
                ge.posSp = 0;
            }
            if (ge.posSp > 31) {
                ge.posSp = 31;
            }
            ge.pos = ge.posStack[ge.posSp];
            ge.vec = ge.vecStack[ge.posSp];
        }
        ge.clipDirty = TRUE;
        break;
    case G3OP_MTX_STORE:
        if (ge.mode == GX_MTXMODE_PROJECTION) {
            ge.projStack[0] = ge.proj;
        } else if (ge.mode == GX_MTXMODE_TEXTURE) {
            ge.texStack[0] = ge.tex;
        } else {
            int i = p[0] & 31;
            if (i < 31) {
                ge.posStack[i] = ge.pos;
                ge.vecStack[i] = ge.vec;
            }
        }
        break;
    case G3OP_MTX_RESTORE:
        if (ge.mode == GX_MTXMODE_PROJECTION) {
            ge.proj = ge.projStack[0];
        } else if (ge.mode == GX_MTXMODE_TEXTURE) {
            ge.tex = ge.texStack[0];
        } else {
            int i = p[0] & 31;
            if (i < 31) {
                ge.pos = ge.posStack[i];
                ge.vec = ge.vecStack[i];
            }
        }
        ge.clipDirty = TRUE;
        break;
    case G3OP_MTX_IDENTITY:
        MtxIdentity(&m);
        ApplyMtx(&m, FALSE, FALSE);
        break;
    case G3OP_MTX_LOAD_4x4:
        LoadParams(&m, p, 4, 4);
        ApplyMtx(&m, FALSE, FALSE);
        break;
    case G3OP_MTX_LOAD_4x3:
        LoadParams(&m, p, 4, 3);
        ApplyMtx(&m, FALSE, FALSE);
        break;
    case G3OP_MTX_MULT_4x4:
        LoadParams(&m, p, 4, 4);
        ApplyMtx(&m, TRUE, FALSE);
        break;
    case G3OP_MTX_MULT_4x3:
        LoadParams(&m, p, 4, 3);
        ApplyMtx(&m, TRUE, FALSE);
        break;
    case G3OP_MTX_MULT_3x3:
        LoadParams(&m, p, 3, 3);
        ApplyMtx(&m, TRUE, FALSE);
        break;
    case G3OP_MTX_SCALE:
        /* scale * M: rows 0-2 of M scaled (what MtxMul gives, without the
         * products with zeros). The direction matrix is not scaled. */
        {
            Mtx *cur = ge.mode == GX_MTXMODE_POSITION_VECTOR ? &ge.pos : CurrentMtx();
            for (int i = 0; i < 3; i++) {
                for (int j = 0; j < 4; j++) {
                    cur->m[i][j] = (s32)(((s64)(s32)p[i] * cur->m[i][j]) >> 12);
                }
            }
            ge.clipDirty = TRUE;
        }
        break;
    case G3OP_MTX_TRANS:
        /* translation * M: row 3 gets the translated origin. */
        for (int t = 0; t < 2; t++) {
            Mtx *cur = ge.mode == GX_MTXMODE_POSITION_VECTOR ? (t == 0 ? &ge.pos : &ge.vec) : (t == 0 ? CurrentMtx() : NULL);
            if (cur == NULL) {
                break;
            }
            for (int j = 0; j < 4; j++) {
                s64 sum = (s64)(s32)p[0] * cur->m[0][j] + (s64)(s32)p[1] * cur->m[1][j] + (s64)(s32)p[2] * cur->m[2][j]
                    + (s64)FX32_ONE * cur->m[3][j];
                cur->m[3][j] = (s32)(sum >> 12);
            }
        }
        ge.clipDirty = TRUE;
        break;

    case G3OP_COLOR:
        ge.color[0] = p[0] & 31;
        ge.color[1] = (p[0] >> 5) & 31;
        ge.color[2] = (p[0] >> 10) & 31;
        break;
    case G3OP_NORMAL: {
        s32 nx = Sext(p[0], 10) << 3, ny = Sext(p[0] >> 10, 10) << 3, nz = Sext(p[0] >> 20, 10) << 3;
        if ((ge.texImage >> 30) == GX_TEXGEN_NORMAL) {
            TexCoordFromVector(nx, ny, nz);
        }
        Light(nx, ny, nz);
        break;
    }
    case G3OP_TEXCOORD:
        ge.s = (s16)(p[0] & 0xffff);
        ge.t = (s16)(p[0] >> 16);
        TexCoordFromTexCoord();
        break;
    case G3OP_VTX_16:
        SubmitVertex((s16)(p[0] & 0xffff), (s16)(p[0] >> 16), (s16)(p[1] & 0xffff));
        break;
    case G3OP_VTX_10:
        SubmitVertex(Sext(p[0], 10) << 6, Sext(p[0] >> 10, 10) << 6, Sext(p[0] >> 20, 10) << 6);
        break;
    case G3OP_VTX_XY:
        SubmitVertex((s16)(p[0] & 0xffff), (s16)(p[0] >> 16), ge.vz);
        break;
    case G3OP_VTX_XZ:
        SubmitVertex((s16)(p[0] & 0xffff), ge.vy, (s16)(p[0] >> 16));
        break;
    case G3OP_VTX_YZ:
        SubmitVertex(ge.vx, (s16)(p[0] & 0xffff), (s16)(p[0] >> 16));
        break;
    case G3OP_VTX_DIFF:
        SubmitVertex(ge.vx + Sext(p[0], 10), ge.vy + Sext(p[0] >> 10, 10), ge.vz + Sext(p[0] >> 20, 10));
        break;
    case G3OP_POLYGON_ATTR:
        ge.polyAttrPending = p[0];
        break;
    case G3OP_TEXIMAGE_PARAM:
        ge.texImage = p[0];
        break;
    case G3OP_TEXPLTT_BASE:
        ge.texPltt = p[0] & 0x1fff;
        break;
    case G3OP_DIF_AMB:
        sLit.dirty = TRUE;
        ge.diffuse = p[0] & 0x7fff;
        ge.ambient = (p[0] >> 16) & 0x7fff;
        if (p[0] & 0x8000) {
            ge.color[0] = ge.diffuse & 31;
            ge.color[1] = (ge.diffuse >> 5) & 31;
            ge.color[2] = (ge.diffuse >> 10) & 31;
        }
        break;
    case G3OP_SPE_EMI:
        sLit.dirty = TRUE;
        ge.specular = p[0] & 0x7fff;
        ge.emission = (p[0] >> 16) & 0x7fff;
        ge.shininessTable = (p[0] >> 15) & 1;
        break;
    case G3OP_LIGHT_VECTOR: {
        int id = p[0] >> 30;
        s32 l[4];
        Transform(&ge.vec, Sext(p[0], 10) << 3, Sext(p[0] >> 10, 10) << 3, Sext(p[0] >> 20, 10) << 3, 0, l);
        ge.lightDir[id][0] = l[0];
        ge.lightDir[id][1] = l[1];
        ge.lightDir[id][2] = l[2];
        break;
    }
    case G3OP_LIGHT_COLOR:
        sLit.dirty = TRUE;
        ge.lightColor[p[0] >> 30] = p[0] & 0x7fff;
        break;
    case G3OP_SHININESS:
        for (int i = 0; i < 32; i++) {
            for (int b = 0; b < 4; b++) {
                ge.shininess[i * 4 + b] = (u8)(p[i] >> (b * 8));
            }
        }
        break;
    case G3OP_BEGIN:
        ge.primType = p[0] & 3;
        ge.primCount = 0;
        ge.stripLen = 0;
        ge.polyAttr = ge.polyAttrPending;
        UpdateWide();
        break;
    case G3OP_END:
        break;
    case G3OP_SWAP_BUFFERS:
        {
            u64 t0 = NtrHost_Ticks();
            NtrRaster_Render(&gNtrG3Polys, p[0]);
            gNtrRasterTicks += NtrHost_Ticks() - t0;
        }
        gNtrG3Polys.count = 0;
        gNtrG3Polys.vertexCount = 0;
        break;
    case G3OP_VIEWPORT:
#ifdef NTR_DEBUG_HOOKS
        if (getenv("PORT_VIEWPORT") && p[0] != ge.viewport) {
            printf("frame %u: viewport %08x\n", Ntr_FramesEnded(), p[0]);
        }
#endif
        ge.viewport = p[0];
        UpdateWide();
        break;
    case G3OP_BOX_TEST:
        BoxTest(p);
        break;
    case G3OP_POS_TEST: {
        s32 c[4];
        ge.vx = (s16)(p[0] & 0xffff);
        ge.vy = (s16)(p[0] >> 16);
        ge.vz = (s16)(p[1] & 0xffff);
        Transform(ClipMtx(), ge.vx, ge.vy, ge.vz, FX32_ONE, c);
        ge.posTestResult.x = c[0];
        ge.posTestResult.y = c[1];
        ge.posTestResult.z = c[2];
        ge.posTestW = c[3];
        break;
    }
    case G3OP_VEC_TEST: {
        s32 v[4];
        Transform(&ge.vec, Sext(p[0], 10) << 3, Sext(p[0] >> 10, 10) << 3, Sext(p[0] >> 20, 10) << 3, 0, v);
        for (int k = 0; k < 3; k++) {
            ge.vecTestResult[k] = (fx16)v[k];
        }
        break;
    }
    default:
        break;
    }
}

/* Command input: FIFO (packed) and register-style writes --------------------------- */

static struct {
    u32 ops; /* remaining packed opcodes, lowest byte next */
    u32 op; /* command collecting parameters */
    int need, have;
    u32 params[32];
} fifo;

static volatile u32 sFifoSlot;
static BOOL sFifoPending;
static volatile u32 sCmdSlot;
static u32 sCmdSlotOp;
static BOOL sCmdPending;

static void NextPackedOp(void)
{
    while (fifo.ops != 0 && fifo.need == 0) {
        u32 op = fifo.ops & 0xff;
        fifo.ops >>= 8;
        if (op == 0) {
            continue;
        }
        int n = ParamCount(op);
        if (n == 0) {
            NtrGe_Execute(op, NULL);
        } else {
            fifo.op = op;
            fifo.need = n;
            fifo.have = 0;
        }
    }
}

/* Command words into the FIFO: whole commands are run with their
 * parameters read in place; the word-at-a-time state machine
 * (NtrGe_FifoWrite) finishes a command split across blocks. */
/* Display lists outside the view ------------------------------------------------- */

/* The game draws whole map blocks around the player, most of them off screen.
 * A display list made only of primitives (BEGIN/END, vertices, normals,
 * colors, texture coordinates) has its vertices' bounding box worked out once,
 * for the whole list and for stretches of it (chunks, which start at a BEGIN,
 * or between two quads or triangles).
 * While a box is entirely beyond one side of the view, none of its polygons
 * could be drawn, so its vertices are skipped and only what it leaves behind
 * is set: its last vertex, color, normal lighting and texture coordinates
 * (replayed in their order) and its last BEGIN. */
#define DL_MIN_WORDS 128
#define DL_CACHE 64
#define DL_REPLAY 5
#define DL_MAX_CHUNKS 32
#define DL_CHUNK_WORDS 96 /* chunks are at least this long (but the last) */

typedef struct DlPart {
    s32 lo[3], hi[3]; /* bounding box of the vertices (fx12, object space) */
    s32 last[3]; /* the last vertex */
    BOOL any; /* has a vertex */
    int numReplay;
    u8 replayOp[DL_REPLAY]; /* G3OP_* (G3OP_NOP: the last vertex) */
    u32 replayArg[DL_REPLAY];
    int replayAt[DL_REPLAY]; /* command index, for the order */
} DlPart;

typedef struct DlChunk {
    u16 start, end; /* words of the block (the first chunk also has the list's first command word) */
    DlPart part;
} DlChunk;

typedef struct DlInfo {
    const u32 *ptr;
    u32 first; /* the list's first command word, when it came in an earlier transfer */
    u32 count, hash, lastUse;
    BOOL cullable;
    DlPart all;
    int numChunks;
    DlChunk chunks[DL_MAX_CHUNKS];
} DlInfo;

static DlInfo sDl[DL_CACHE];
static u32 sDlClock;
#ifdef NTR_DEBUG_HOOKS
u32 gNtrDlCulled, gNtrDlDrawn, gNtrDlState[4], gNtrDlChunksSkipped, gNtrDlChunksDrawn;
#endif

/* A check of a list's words (cheap: it runs on every list, every frame). */
static u32 HashList(const u32 *w, u32 count)
{
    u32 sum = count, x = 0;
    for (u32 i = 0; i < count; i++) {
        sum += w[i];
        x = (x << 5 | x >> 27) ^ w[i];
    }
    return sum ^ (x * 0x9e3779b9u);
}

static void PartInit(DlPart *d)
{
    for (int k = 0; k < 3; k++) {
        d->lo[k] = 0x7fffffff;
        d->hi[k] = -0x7fffffff;
    }
    d->any = FALSE;
    d->numReplay = 0;
}

static void NoteReplay(DlPart *d, u32 op, u32 arg, int at)
{
    for (int i = 0; i < d->numReplay; i++) {
        if (d->replayOp[i] == op) {
            d->replayArg[i] = arg;
            d->replayAt[i] = at;
            return;
        }
    }
    if (d->numReplay < DL_REPLAY) {
        d->replayOp[d->numReplay] = (u8)op;
        d->replayArg[d->numReplay] = arg;
        d->replayAt[d->numReplay] = at;
        d->numReplay++;
    }
}

static void NoteVertex(DlPart *d, const s32 v[3], int at)
{
    for (int k = 0; k < 3; k++) {
        d->lo[k] = v[k] < d->lo[k] ? v[k] : d->lo[k];
        d->hi[k] = v[k] > d->hi[k] ? v[k] : d->hi[k];
        d->last[k] = v[k];
    }
    d->any = TRUE;
    NoteReplay(d, G3OP_NOP, 0, at);
}

static void ScanList(DlInfo *d, u32 first, const u32 *w0, u32 count)
{
    const u32 *w = w0, *end = w0 + count;
    BOOL known = FALSE; /* the current vertex is set by this list */
    s32 v[3] = { 0, 0, 0 };
    int at = 0;
    int primType = -1, primVerts = 0; /* the primitive being drawn, and its vertices so far */
    d->cullable = FALSE;
    PartInit(&d->all);
    d->numChunks = 1;
    d->chunks[0].start = 0;
    PartInit(&d->chunks[0].part);
    for (u32 ops = first; ops != 0 || w < end; ops = 0) {
        if (ops == 0) {
            /* A new chunk where a command word starts with BEGIN, or comes
             * between two quads or triangles. */
            DlChunk *c = &d->chunks[d->numChunks - 1];
            BOOL boundary = (*w & 0xff) == G3OP_BEGIN || (primType == GX_BEGIN_QUADS && primVerts % 4 == 0)
                || (primType == GX_BEGIN_TRIANGLES && primVerts % 3 == 0);
            if (boundary && w - w0 - c->start >= DL_CHUNK_WORDS && d->numChunks < DL_MAX_CHUNKS) {
                c->end = (u16)(w - w0);
                c = &d->chunks[d->numChunks++];
                c->start = (u16)(w - w0);
                PartInit(&c->part);
            }
            ops = *w++;
        }
        DlPart *part = &d->chunks[d->numChunks - 1].part;
        for (; ops != 0; ops >>= 8) {
            u32 op = ops & 0xff;
            if (op == G3OP_NOP) {
                continue;
            }
            int n = ParamCount(op);
            if (end - w < n) {
                return; /* a command split off the end */
            }
            const u32 *p = w;
            w += n;
            at++;
            BOOL vertex = TRUE;
            switch (op) {
            case G3OP_VTX_16:
                v[0] = (s16)(p[0] & 0xffff), v[1] = (s16)(p[0] >> 16), v[2] = (s16)(p[1] & 0xffff);
                known = TRUE;
                break;
            case G3OP_VTX_10:
                v[0] = Sext(p[0], 10) << 6, v[1] = Sext(p[0] >> 10, 10) << 6, v[2] = Sext(p[0] >> 20, 10) << 6;
                known = TRUE;
                break;
            case G3OP_VTX_XY:
                v[0] = (s16)(p[0] & 0xffff), v[1] = (s16)(p[0] >> 16);
                break;
            case G3OP_VTX_XZ:
                v[0] = (s16)(p[0] & 0xffff), v[2] = (s16)(p[0] >> 16);
                break;
            case G3OP_VTX_YZ:
                v[1] = (s16)(p[0] & 0xffff), v[2] = (s16)(p[0] >> 16);
                break;
            case G3OP_VTX_DIFF:
                v[0] += Sext(p[0], 10), v[1] += Sext(p[0] >> 10, 10), v[2] += Sext(p[0] >> 20, 10);
                break;
            case G3OP_BEGIN:
                primType = p[0] & 3;
                primVerts = 0;
                /* fall through */
            case G3OP_COLOR:
            case G3OP_NORMAL:
            case G3OP_TEXCOORD:
                NoteReplay(&d->all, op, p[0], at);
                NoteReplay(part, op, p[0], at);
                vertex = FALSE;
                break;
            case G3OP_END:
                vertex = FALSE;
                break;
            default:
                return; /* anything else changes state the list leaves behind */
            }
            if (vertex) {
                if (!known) {
                    return; /* relative to a vertex from before the list */
                }
                NoteVertex(&d->all, v, at);
                NoteVertex(part, v, at);
                primVerts++;
            }
        }
    }
    d->chunks[d->numChunks - 1].end = (u16)count;
    d->cullable = d->all.any && count <= 0xffff;
#ifdef NTR_DEBUG_HOOKS
    if (getenv("PORT_DLSCAN")) {
        /* Where the BEGINs are: position in their command word, and gaps. */
        int pos[4] = { 0 }, begins = 0, prims[4] = { 0 };
        const u32 *q = w0, *qe = w0 + count;
        for (u32 ops = first; ops != 0 || q < qe; ops = 0) {
            if (ops == 0) {
                ops = *q++;
            }
            for (int b = 0; ops != 0; ops >>= 8, b++) {
                u32 op = ops & 0xff;
                if (op == G3OP_NOP) {
                    continue;
                }
                if (op == G3OP_BEGIN) {
                    pos[b]++;
                    begins++;
                    prims[*q & 3]++;
                }
                q += ParamCount(op);
            }
        }
        printf("list of %u words: %d BEGINs at byte 0/1/2/3: %d %d %d %d; tri/quad/tristrip/quadstrip %d %d %d %d; %d chunks\n", count,
            begins, pos[0], pos[1], pos[2], pos[3], prims[0], prims[1], prims[2], prims[3], d->numChunks);
    }
#endif
}

/* A part's bounding box is entirely beyond one side of the view (in the clip
 * space SubmitVertex makes, widescreen scale included). The corners follow
 * from one corner and the box's edges, as clip space is linear; a small
 * margin covers the rounding of that and of the fixed-point transform. */
static BOOL PartOutsideView(const DlPart *d)
{
    if (!d->any) {
        return TRUE;
    }
    const Mtx *m = ClipMtx();
    s32 c0[4];
    Transform(m, d->lo[0], d->lo[1], d->lo[2], FX32_ONE, c0);
    float base[4], edge[3][4];
    float scale[4] = { ge.wideX / 4096.0f, ge.wideY / 4096.0f, 1 / 4096.0f, 1 / 4096.0f };
    for (int j = 0; j < 4; j++) {
        base[j] = c0[j] * scale[j];
        for (int k = 0; k < 3; k++) {
            edge[k][j] = (float)(d->hi[k] - d->lo[k]) * (float)m->m[k][j] / 4096.0f * scale[j];
        }
    }
    u32 out = 0x3f;
    for (int i = 0; i < 8 && out != 0; i++) {
        float c[4];
        for (int j = 0; j < 4; j++) {
            c[j] = base[j] + ((i & 1) ? edge[0][j] : 0) + ((i & 2) ? edge[1][j] : 0) + ((i & 4) ? edge[2][j] : 0);
        }
        float w = c[3];
        float margin = 1e-4f * (fabsf(c[0]) + fabsf(c[1]) + fabsf(c[2]) + fabsf(w)) + 1e-3f;
        out &= (c[0] < -w - margin ? 1u : 0) | (c[0] > w + margin ? 2u : 0) | (c[1] < -w - margin ? 4u : 0) | (c[1] > w + margin ? 8u : 0)
            | (c[2] < -w - margin ? 16u : 0) | (c[2] > w + margin ? 32u : 0);
    }
    return out != 0;
}

/* Sets what a skipped part leaves behind, in its order. */
static void ReplayPart(const DlPart *d)
{
    int order[DL_REPLAY];
    for (int i = 0; i < d->numReplay; i++) {
        order[i] = i;
    }
    for (int i = 1; i < d->numReplay; i++) {
        for (int j = i; j > 0 && d->replayAt[order[j]] < d->replayAt[order[j - 1]]; j--) {
            int t = order[j];
            order[j] = order[j - 1];
            order[j - 1] = t;
        }
    }
    for (int i = 0; i < d->numReplay; i++) {
        int r = order[i];
        if (d->replayOp[r] == G3OP_NOP) {
            ge.vx = d->last[0];
            ge.vy = d->last[1];
            ge.vz = d->last[2];
            if ((ge.texImage >> 30) == GX_TEXGEN_VERTEX) {
                TexCoordFromVector(ge.vx, ge.vy, ge.vz);
            }
        } else {
            NtrGe_Execute(d->replayOp[r], &d->replayArg[r]);
        }
    }
    /* No primitive continues from the skipped vertices. */
    ge.primCount = 0;
    ge.stripLen = 0;
}

static void DecodeBlock(const u32 *w, u32 count);

/* Runs a display list, skipping what is outside the view; FALSE (nothing
 * run) if it cannot be skipped in part. `first` is the list's first command
 * word if that came at the end of an earlier transfer (NitroSystem sends it
 * so), with none of its parameters yet, else 0. */
static BOOL RunListInView(u32 first, const u32 *w, u32 count)
{
    u32 hash = HashList(w, count) ^ first;
    DlInfo *d = NULL, *oldest = &sDl[0];
    for (int i = 0; i < DL_CACHE; i++) {
        if (sDl[i].ptr == w && sDl[i].first == first && sDl[i].count == count && sDl[i].hash == hash) {
            d = &sDl[i];
            break;
        }
        if (sDl[i].lastUse < oldest->lastUse) {
            oldest = &sDl[i];
        }
    }
    if (d == NULL) {
        d = oldest;
        d->ptr = w;
        d->first = first;
        d->count = count;
        d->hash = hash;
        ScanList(d, first, w, count);
    }
    d->lastUse = ++sDlClock;
    if (!d->cullable) {
        return FALSE;
    }
    if (PartOutsideView(&d->all)) {
#ifdef NTR_DEBUG_HOOKS
        gNtrDlCulled++;
#endif
        if (first != 0) {
            fifo.need = 0; /* the pending first command is the list's */
            fifo.ops = 0;
        }
        ReplayPart(&d->all);
        return TRUE;
    }
#ifdef NTR_DEBUG_HOOKS
    gNtrDlDrawn++;
#endif
    if (d->numChunks < 2) {
        return FALSE;
    }
    for (int i = 0; i < d->numChunks; i++) {
        const DlChunk *c = &d->chunks[i];
        if (PartOutsideView(&c->part)) {
#ifdef NTR_DEBUG_HOOKS
            gNtrDlChunksSkipped++;
#endif
            if (i == 0 && first != 0) {
                fifo.need = 0;
                fifo.ops = 0;
            }
            ReplayPart(&c->part);
        } else {
#ifdef NTR_DEBUG_HOOKS
            gNtrDlChunksDrawn++;
#endif
            DecodeBlock(w + c->start, c->end - c->start);
        }
    }
    return TRUE;
}

#ifdef NTR_DEBUG_HOOKS
/* Debugging (PORT_GESTATS): FeedFifo blocks by size, and how many start
 * with a BEGIN command, per frame. */
u32 gNtrGeBlocks[8], gNtrGeBlockBegin, gNtrGeBlockWords;
#endif

/* Time in the geometry engine (FIFO blocks, SDK calls) and in the 3D
 * renderer at SWAP_BUFFERS, for profiling (NtrHost_Ticks units). */
u64 gNtrGeTicks;

static void FeedFifoTimed(const u32 *w, u32 count);

static void FeedFifo(const u32 *w, u32 count)
{
    if (gNtrDebugSkip & NTR_SKIP_GEOMETRY) {
        return;
    }
    u64 t0 = NtrHost_Ticks();
    FeedFifoTimed(w, count);
    gNtrGeTicks += NtrHost_Ticks() - t0;
}

static void FeedFifoTimed(const u32 *w, u32 count)
{
#ifdef NTR_DEBUG_HOOKS
    if (count >= DL_MIN_WORDS) {
        gNtrDlState[(fifo.need != 0) | (fifo.ops != 0) << 1]++;
    }
#endif
    if (count >= DL_MIN_WORDS) {
        if (fifo.need == 0 && fifo.ops == 0) {
            if (RunListInView(0, w, count)) {
                return;
            }
        } else if (fifo.need > 0 && fifo.have == 0) {
            /* The list's first command word came before, without parameters. */
            if (RunListInView(fifo.ops << 8 | fifo.op, w, count)) {
                return;
            }
        }
    }
    DecodeBlock(w, count);
}

/* Command words: whole commands are run with their parameters read in place;
 * the word-at-a-time state machine (NtrGe_FifoWrite) finishes a command split
 * across blocks. */
static void DecodeBlock(const u32 *w, u32 count)
{
    const u32 *end = w + count;
#ifdef NTR_DEBUG_HOOKS
    {
        int b = 0;
        for (u32 c = count; c > 4 && b < 7; c >>= 2) {
            b++;
        }
        gNtrGeBlocks[b]++;
        gNtrGeBlockWords += count;
        if (count > 0 && (w[0] & 0xff) == 0x40) {
            gNtrGeBlockBegin++;
        }
    }
#endif
    while (w < end) {
        if (fifo.need != 0 || fifo.ops != 0) {
#ifdef NTR_DEBUG_HOOKS
            gNtrGeFifoFromBlocks++;
#endif
            NtrGe_FifoWrite(*w++);
            continue;
        }
        u32 ops = *w++;
        while (ops != 0) {
            u32 op = ops & 0xff;
            ops >>= 8;
            if (op == 0) {
                continue;
            }
            /* The vertex data commands (most of a display list) directly. */
            if (w < end) {
                u32 p0 = *w;
                switch (op) {
                case G3OP_TEXCOORD:
                    GE_COUNT(op);
                    ge.s = (s16)(p0 & 0xffff);
                    ge.t = (s16)(p0 >> 16);
                    TexCoordFromTexCoord();
                    w++;
                    continue;
                case G3OP_VTX_XY:
                    GE_COUNT(op);
                    SubmitVertex((s16)(p0 & 0xffff), (s16)(p0 >> 16), ge.vz);
                    w++;
                    continue;
                case G3OP_VTX_XZ:
                    GE_COUNT(op);
                    SubmitVertex((s16)(p0 & 0xffff), ge.vy, (s16)(p0 >> 16));
                    w++;
                    continue;
                case G3OP_VTX_YZ:
                    GE_COUNT(op);
                    SubmitVertex(ge.vx, (s16)(p0 & 0xffff), (s16)(p0 >> 16));
                    w++;
                    continue;
                case G3OP_VTX_16:
                    if (end - w >= 2) {
                        GE_COUNT(op);
                        SubmitVertex((s16)(p0 & 0xffff), (s16)(p0 >> 16), (s16)(w[1] & 0xffff));
                        w += 2;
                        continue;
                    }
                    break;
                default:
                    break;
                }
            }
            int n = ParamCount(op);
            if (n == 0) {
                GE_PATH(0);
                NtrGe_Execute(op, NULL);
            } else if (end - w >= n) {
                GE_PATH(0);
                NtrGe_Execute(op, w);
                w += n;
            } else {
                fifo.ops = ops;
                fifo.op = op;
                fifo.need = n;
                fifo.have = 0;
                break;
            }
        }
    }
}

void NtrGe_FifoWrite(u32 word)
{
#ifdef NTR_DEBUG_HOOKS
    gNtrGeFifoWords++;
    if (fifo.need == 0) {
        for (u32 o = word; o != 0; o >>= 8) {
            gNtrGeFifoOps[o & 0xff]++;
        }
    }
#endif
    if (fifo.need > 0) {
        fifo.params[fifo.have++] = word;
        if (fifo.have == fifo.need) {
            fifo.need = 0;
            GE_PATH(1);
            NtrGe_Execute(fifo.op, fifo.params);
            NextPackedOp();
        }
        return;
    }
    fifo.ops = word;
    NextPackedOp();
}

/* Parameters written to a command port collect until the command has all of
 * them, as in the hardware FIFO. */
static struct {
    u32 op;
    int have;
    u32 params[32];
} sPort;

static void PortWrite(u32 op, u32 value)
{
    int need = ParamCount(op);
    if (need == 0) {
        sPort.have = 0;
        NtrGe_Execute(op, NULL);
        return;
    }
    if (sPort.op != op) {
        sPort.op = op;
        sPort.have = 0;
    }
    sPort.params[sPort.have++] = value;
    if (sPort.have == need) {
        sPort.have = 0;
        GE_PATH(2);
        NtrGe_Execute(op, sPort.params);
    }
}

void NtrGe_Flush(void)
{
    if (sFifoPending) {
        sFifoPending = FALSE;
        NtrGe_FifoWrite(sFifoSlot);
    }
    if (sCmdPending) {
        sCmdPending = FALSE;
        PortWrite(sCmdSlotOp, sCmdSlot);
    }
}

volatile u32 *NtrGe_FifoSlot(void)
{
    NtrGe_Flush();
    sFifoPending = TRUE;
    return &sFifoSlot;
}

volatile u32 *NtrGe_CmdSlot(u32 op)
{
    NtrGe_Flush();
    sCmdSlotOp = op;
    sCmdPending = TRUE;
    return &sCmdSlot;
}

void NtrGe_Command(u32 op, const u32 *params)
{
    NtrGe_Flush();
    sPort.have = 0;
    GE_PATH(3);
    NtrGe_Execute(op, params);
}

/* Display lists and CPU transfers into the FIFO. */
void MI_SendGXCommand(u32 dmaNo, const void *src, u32 commandLength)
{
    (void)dmaNo;
    NtrGe_Flush();
    FeedFifo(src, commandLength / 4);
}

void MI_SendGXCommandAsync(u32 dmaNo, const void *src, u32 commandLength, void (*callback)(void *), void *arg)
{
    MI_SendGXCommand(dmaNo, src, commandLength);
    if (callback) {
        callback(arg);
    }
}

void MI_SendGXCommandFast(u32 dmaNo, const void *src, u32 commandLength) { MI_SendGXCommand(dmaNo, src, commandLength); }

void MI_SendGXCommandAsyncFast(u32 dmaNo, const void *src, u32 commandLength, void (*callback)(void *), void *arg)
{
    MI_SendGXCommandAsync(dmaNo, src, commandLength, callback, arg);
}

/* MI_CpuSend32 (an SDK inline) lands here; sends into the FIFO feed the engine. */
void MIi_CpuSend32(const void *src, volatile void *dest, u32 size)
{
    const u32 *w = src;
    if (dest == &sFifoSlot) {
        sFifoPending = FALSE; /* the slot was taken just to form this address */
        FeedFifo(w, size / 4);
    } else if (dest == &sCmdSlot) {
        sCmdPending = FALSE;
        for (u32 i = 0; i < size / 4; i++) {
            PortWrite(sCmdSlotOp, w[i]);
        }
    } else if ((u8 *)dest >= gNtrIoRegs + 0x400 && (u8 *)dest < gNtrIoRegs + 0x600) {
        /* REG_*_ADDR of the FIFO (how NitroSystem sends its command buffers)
         * or a command port */
        u32 off = (u32)((volatile u8 *)dest - gNtrIoRegs);
        NtrGe_Flush();
        if (off < 0x440) {
            FeedFifo(w, size / 4);
        } else {
            for (u32 i = 0; i < size / 4; i++) {
                PortWrite((off - 0x400) / 4, w[i]);
            }
        }
    } else {
        for (u32 i = 0; i < size / 4; i++) {
            ((volatile u32 *)dest)[0] = w[i];
        }
    }
}

#ifdef NTR_DEBUG_HOOKS
u32 gNtrGeSyncs[16]; /* read-backs and resets, by kind (PORT_GESTATS) */
#define DEBUG_SYNC(k) (gNtrGeSyncs[k]++)
#else
#define DEBUG_SYNC(k) ((void)0)
#endif

/* SDK 3D API ---------------------------------------------------------------------- */




void G3_LoadMtx44(const MtxFx44 *m) { NtrGe_Command(G3OP_MTX_LOAD_4x4, (const u32 *)m->a); }
void G3_LoadMtx43(const MtxFx43 *m) { NtrGe_Command(G3OP_MTX_LOAD_4x3, (const u32 *)m->a); }
void G3_MultMtx44(const MtxFx44 *m) { NtrGe_Command(G3OP_MTX_MULT_4x4, (const u32 *)m->a); }
void G3_MultMtx43(const MtxFx43 *m) { NtrGe_Command(G3OP_MTX_MULT_4x3, (const u32 *)m->a); }
void G3_MultMtx33(const MtxFx33 *m) { NtrGe_Command(G3OP_MTX_MULT_3x3, (const u32 *)m->a); }

void G3_MultTransMtx33(const MtxFx33 *mtx, const VecFx32 *trans)
{
    u32 p[12];
    memcpy(p, mtx->a, sizeof(mtx->a));
    p[9] = (u32)trans->x;
    p[10] = (u32)trans->y;
    p[11] = (u32)trans->z;
    NtrGe_Command(G3OP_MTX_MULT_4x3, p);
}


/* G3_RotX/Y/Z and the texture matrix helpers come from the SDK (g3_util.c). */




void G3_Shininess(const u32 *table) { NtrGe_Command(G3OP_SHININESS, table); }




/* G3X: engine control ----------------------------------------------------------------- */

void G3X_InitMtxStack(void)
{
    DEBUG_SYNC(7);
    NtrGe_Flush();
    MtxIdentity(&ge.proj);
    MtxIdentity(&ge.pos);
    MtxIdentity(&ge.vec);
    MtxIdentity(&ge.tex);
    ge.posSp = ge.projSp = ge.texSp = 0;
    ge.clipDirty = TRUE;
    GXSTAT &= ~(1u << 15);
}

/* As the SDK's: the texture matrix becomes the identity, the projection
 * stack is popped (its matrix kept), the position/vector stack is popped and
 * its matrix becomes the identity; the mode ends as position/vector. */
void G3X_ResetMtxStack(void)
{
    DEBUG_SYNC(10);
    NtrGe_Flush();
    GXSTAT &= ~(1u << 15);
    u32 mode = GX_MTXMODE_TEXTURE;
    NtrGe_Execute(G3OP_MTX_MODE, &mode);
    NtrGe_Execute(G3OP_MTX_IDENTITY, NULL);
    mode = GX_MTXMODE_PROJECTION;
    NtrGe_Execute(G3OP_MTX_MODE, &mode);
    if (ge.projSp != 0) {
        u32 n = (u32)ge.projSp;
        NtrGe_Execute(G3OP_MTX_POP, &n);
    }
    mode = GX_MTXMODE_POSITION_VECTOR;
    NtrGe_Execute(G3OP_MTX_MODE, &mode);
    u32 n = (u32)ge.posSp & 0x3f;
    NtrGe_Execute(G3OP_MTX_POP, &n);
    NtrGe_Execute(G3OP_MTX_IDENTITY, NULL);
}

void G3X_Init(void)
{
    memset(&fifo, 0, sizeof(fifo));
    sFifoPending = sCmdPending = FALSE;
    G3X_InitMtxStack();
    DISP3DCNT = 0;
    G3X_InitTable();
    G3X_SetClearColor(0, 0, 0x7fff, 0, FALSE);
    G3_ViewPort(0, 0, 255, 191);
}

/* As the SDK's: error flags cleared, the matrix stacks reset
 * (G3X_ResetMtxStack), and the polygon and texture state back to defaults
 * (games call it every frame, and rely on it: the particle system leaves a
 * scaled texture matrix behind, which would distort the battle's Pokemon). */
void G3X_Reset(void)
{
    DEBUG_SYNC(8);
    NtrGe_Flush();
    DISP3DCNT &= ~(3u << 12);
    G3X_ResetMtxStack();
    /* GX_LIGHTMASK_NONE, GX_POLYGONMODE_MODULATE, GX_CULL_BACK, ID 0, alpha 31 */
    u32 attr = 0x80u | (31u << 16), zero = 0;
    NtrGe_Execute(G3OP_POLYGON_ATTR, &attr);
    NtrGe_Execute(G3OP_TEXIMAGE_PARAM, &zero);
    NtrGe_Execute(G3OP_TEXPLTT_BASE, &zero);
}

void G3X_ClearFifo(void)
{
    DEBUG_SYNC(9);
    memset(&fifo, 0, sizeof(fifo));
    sFifoPending = sCmdPending = FALSE;
}

void G3X_InitTable(void)
{
    static const u32 zero[32];
    G3X_SetEdgeColorTable((const GXRgb *)zero);
    G3X_SetFogTable(zero);
    G3X_SetToonTable((const GXRgb *)zero);
    G3_Shininess(zero);
}

int G3X_GetClipMtx(MtxFx44 *m)
{
    DEBUG_SYNC(0);
    NtrGe_Flush();
    const Mtx *c = ClipMtx();
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            m->m[i][j] = c->m[i][j];
        }
    }
    return 0;
}

int G3X_GetVectorMtx(MtxFx33 *m)
{
    DEBUG_SYNC(1);
    NtrGe_Flush();
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            m->m[i][j] = ge.vec.m[i][j];
        }
    }
    return 0;
}

int G3X_GetPositionTestResult(VecFx32 *vec, fx32 *w)
{
    DEBUG_SYNC(2);
    NtrGe_Flush();
    *vec = ge.posTestResult;
    *w = ge.posTestW;
    return 0;
}

int G3X_GetVectorTestResult(fx16 *vec)
{
    DEBUG_SYNC(3);
    NtrGe_Flush();
    memcpy(vec, ge.vecTestResult, sizeof(ge.vecTestResult));
    return 0;
}

int G3X_GetBoxTestResult(s32 *in)
{
    DEBUG_SYNC(4);
    NtrGe_Flush();
    *in = (GXSTAT >> 1) & 1;
    return 0;
}

int G3X_GetMtxStackLevelPV(s32 *level)
{
    DEBUG_SYNC(5);
    NtrGe_Flush();
    *level = ge.posSp;
    return 0;
}

int G3X_GetMtxStackLevelPJ(s32 *level)
{
    DEBUG_SYNC(6);
    NtrGe_Flush();
    *level = ge.projSp;
    return 0;
}
