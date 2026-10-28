/*
 * GPU composition of the DS screens on the 3DS.
 *
 * Each frame the CPU decodes the 2D layers of both engines into images
 * (Ntr_ComposeEngine, platform/src/ntr_render.c), the GPU's transfer engine
 * turns them into textures, and the composition passes are drawn straight
 * into the screen's framebuffer, with the GPU 3D frame (gpu3d_3ds.c) as one
 * more texture. The CPU does no per-pixel blending and the 3D frame is not
 * read back. Frames the passes do not cover are composed on the CPU
 * (Ntr_RenderEngine) and drawn as one picture.
 *
 * Textures keep their first rows at the top (t = 1), as the transfer engine
 * writes them. Layer images cover the screen (texel = output pixel) in the
 * top rows of 512x256 textures.
 */
#include <string.h>

#include <3ds.h>
#include <citro3d.h>

#include "gpu2d_3ds.h"
#include "gpu3d_3ds.h"
#include "ntr_shim.h"

extern const unsigned char gGpu2DShader[];
extern const unsigned gGpu2DShader_size;

#define LAYER_TEX_W 512 /* layer images, rows LAYER_TEX_W pixels apart */
#define LAYER_TEX_H 256
#define PICTURE_W 512 /* a CPU-composed screen */
#define PICTURE_H 256
#define TEX3D_W 512
#define TEX3D_H 256
#define MAX_VERTS ((2 * NTR_COMPOSE_MAX_RECTS + 16) * 6)

#define TRANSFER_FLAGS(fmt)                                                                         \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(1) | GX_TRANSFER_RAW_COPY(0)                  \
        | GX_TRANSFER_IN_FORMAT(fmt) | GX_TRANSFER_OUT_FORMAT(fmt) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

typedef struct Vertex {
    float x, y, u, v, u1, v1; /* the source's texel, the below image's */
} Vertex;

/* Where a source's texel for output pixel (x, y) is: (x + offX, y + offY) of
 * a texW x texH texture. */
typedef struct Mapping {
    C3D_Tex *tex;
    float offX, offY, texW, texH;
} Mapping;

typedef struct EngineImages {
    u16 *bg[4]; /* RGBA5551, LAYER_TEX_W x NTR_LAYER_H */
    u32 *obj; /* RGBA8 */
    u16 *picture; /* RGBA5551, PICTURE_W x PICTURE_H */
    u16 *objWin; /* the OBJ window's shape, RGBA5551 like the BG images */
    C3D_Tex bgTex[4], objTex, pictureTex, objWinTex;
    NtrComposition comp;
    bool software;
    bool bgDirty[4]; /* the image changed since its last transfer */
} EngineImages;

static EngineImages sEngines[2];
static shaderProgram_s sProgram;
static DVLB_s *sShader;
static int sProjLoc, sZLoc;

/* Depths of the composition's pixels (as zoffset): the OBJ window's pixels
 * are drawn at Z_OBJ_WINDOW in its depth pass, so passes for either part
 * of it test for their depth. */
#define Z_OUTSIDE 0.0f
#define Z_OBJ_WINDOW -0.25f
static Vertex *sVerts;
static int sNumVerts;

u64 gGpuFlushTicks;

void Gpu_FlushDataCache(const void *p, u32 size)
{
    u64 t = svcGetSystemTick();
    GSPGPU_FlushDataCache(p, size);
    gGpuFlushTicks += svcGetSystemTick() - t;
}

static void InitTex(C3D_Tex *tex, int w, int h, GPU_TEXCOLOR fmt)
{
    C3D_TexInit(tex, (u16)w, (u16)h, fmt);
    C3D_TexSetFilter(tex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
}

void Gpu2D_Init(void)
{
    for (int e = 0; e < 2; e++) {
        EngineImages *s = &sEngines[e];
        for (int i = 0; i < 4; i++) {
            s->bg[i] = linearAlloc(LAYER_TEX_W * NTR_LAYER_H * 2);
            InitTex(&s->bgTex[i], LAYER_TEX_W, LAYER_TEX_H, GPU_RGBA5551);
        }
        /* Rows of the OBJ image are only cleared after they had sprites. */
        s->obj = linearAlloc(LAYER_TEX_W * NTR_LAYER_H * 4);
        memset(s->obj, 0, LAYER_TEX_W * NTR_LAYER_H * 4);
        InitTex(&s->objTex, LAYER_TEX_W, LAYER_TEX_H, GPU_RGBA8);
        s->picture = linearAlloc(PICTURE_W * PICTURE_H * 2);
        InitTex(&s->pictureTex, PICTURE_W, PICTURE_H, GPU_RGBA5551);
        s->objWin = linearAlloc(LAYER_TEX_W * NTR_LAYER_H * 2);
        memset(s->objWin, 0, LAYER_TEX_W * NTR_LAYER_H * 2);
        InitTex(&s->objWinTex, LAYER_TEX_W, LAYER_TEX_H, GPU_RGBA5551);
    }
    sShader = DVLB_ParseFile((u32 *)gGpu2DShader, gGpu2DShader_size);
    shaderProgramInit(&sProgram);
    shaderProgramSetVsh(&sProgram, &sShader->DVLE[0]);
    sProjLoc = shaderInstanceGetUniformLocation(sProgram.vertexShader, "projection");
    sZLoc = shaderInstanceGetUniformLocation(sProgram.vertexShader, "zoffset");
    sVerts = linearAlloc(MAX_VERTS * sizeof(Vertex));
}

void Gpu2D_BeginFrame(void)
{
    sNumVerts = 0; /* the GPU is done with the last frame's vertices */
}

void Gpu2D_Begin(void)
{
    for (int e = 0; e < 2; e++) {
        sEngines[e].software = !Ntr_ComposeBegin(e, &sEngines[e].comp);
    }
}

void Gpu2D_Work(int part)
{
    /* Part 1 decodes its lines in the other order, so the two parts do not
     * fight over the same engine's data at the same time. */
    for (int k = 0; k < 2; k++) {
        int engine = part == 0 ? 1 - k : k;
        EngineImages *s = &sEngines[engine];
        if (s->software) {
            if (engine == 1 - part) {
                if (engine == 0) {
                    Gpu3D_ReadBack();
                }
                Ntr_RenderEngine(engine, s->picture, PICTURE_W);
            }
            continue;
        }
        NtrLayerImages images = { { s->bg[0], s->bg[1], s->bg[2], s->bg[3] }, s->obj, LAYER_TEX_W, s->objWin };
        Ntr_ComposeLines(engine, &images, part, 2);
    }
}

static bool sLateSoftware[2];

bool Gpu2D_EngineHasNewLayers(int engine)
{
    return !sEngines[engine].software && sEngines[engine].comp.changedBg != 0;
}

void Gpu2D_EndEngine(int engine)
{
    EngineImages *s = &sEngines[engine];
    sLateSoftware[engine] = false;
    if (!s->software) {
        NtrLayerImages images = { { s->bg[0], s->bg[1], s->bg[2], s->bg[3] }, s->obj, LAYER_TEX_W, s->objWin };
        s->software = sLateSoftware[engine] = !Ntr_ComposeEnd(engine, &images, &s->comp);
    }
}

void Gpu2D_End(void)
{
    for (int e = 0; e < 2; e++) {
        if (sLateSoftware[e]) {
            /* Too many passes: compose on the CPU after all. */
            if (e == 0) {
                Gpu3D_ReadBack();
            }
            Ntr_RenderEngine(e, sEngines[e].picture, PICTURE_W);
        }
    }
}

bool Gpu2D_UsedSoftware(int engine)
{
    return sEngines[engine].software;
}

/* Queued in the GPU's command queue, so it runs before the draws that follow. */
static void Transfer(void *src, int bytesPerPixel, int w, int h, C3D_Tex *tex, u32 fmt)
{
    Gpu_FlushDataCache(src, (u32)(w * h * bytesPerPixel));
    GX_DisplayTransfer((u32 *)src, GX_BUFFER_DIM(w, h), (u32 *)tex->data, GX_BUFFER_DIM(w, h), TRANSFER_FLAGS(fmt));
}

static GPU_TESTFUNC TestFunc(int test)
{
    switch (test) {
    case NTR_TEST_EQUAL:
        return GPU_EQUAL;
    case NTR_TEST_NOTEQUAL:
        return GPU_NOTEQUAL;
    case NTR_TEST_LESS:
        return GPU_LESS;
    case NTR_TEST_GREATER:
        return GPU_GREATER;
    default:
        return GPU_ALWAYS;
    }
}

/* 0xRRGGBBAA -> citro3d's 0xAABBGGRR. */
static u32 Abgr(u32 c)
{
    return (c & 0xff) << 24 | ((c >> 8) & 0xff) << 16 | ((c >> 16) & 0xff) << 8 | c >> 24;
}

static void SetSource(C3D_Tex *tex, u32 color)
{
    C3D_TexEnv *env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    if (tex != NULL) {
        C3D_TexBind(0, tex);
        C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    } else {
        C3D_TexEnvSrc(env, C3D_Both, GPU_CONSTANT, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
        C3D_TexEnvColor(env, Abgr(color));
    }
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    C3D_TexEnvInit(C3D_GetTexEnv(1));
}

/* The blend done in the texture combiner with the below image on unit 1
 * (texture 0 is the source): source * eva + below * evb, or source over
 * below by the source's alpha. Alpha stays the source's, for the alpha test. */
static void SetSourceOverImage(C3D_Tex *tex, C3D_Tex *below, int blend, int eva, int evb)
{
    C3D_TexBind(0, tex);
    C3D_TexBind(1, below);
    C3D_TexEnv *env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnv *env1 = C3D_GetTexEnv(1);
    C3D_TexEnvInit(env1);
    C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
    if (blend == NTR_BLEND_EVA_EVB) {
        u32 a = (u32)(eva * 255 + 8) / 16, b = (u32)(evb * 255 + 8) / 16;
        C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_CONSTANT, GPU_PRIMARY_COLOR);
        C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
        C3D_TexEnvColor(env, 0xff000000u | a << 16 | a << 8 | a);
        C3D_TexEnvSrc(env1, C3D_RGB, GPU_TEXTURE1, GPU_CONSTANT, GPU_PREVIOUS);
        C3D_TexEnvFunc(env1, C3D_RGB, GPU_MULTIPLY_ADD);
        C3D_TexEnvColor(env1, 0xff000000u | b << 16 | b << 8 | b);
    } else {
        C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_TEXTURE1, GPU_TEXTURE0);
        C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA);
        C3D_TexEnvFunc(env, C3D_RGB, GPU_INTERPOLATE);
        C3D_TexEnvSrc(env1, C3D_RGB, GPU_PREVIOUS, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
        C3D_TexEnvFunc(env1, C3D_RGB, GPU_REPLACE);
    }
    C3D_TexEnvSrc(env1, C3D_Alpha, GPU_PREVIOUS, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env1, C3D_Alpha, GPU_REPLACE);
}

/* Two triangles over r, with the texels of source mapping a and (for a
 * blend with a below image) b. */
static void AddRect(const NtrRect *r, const Mapping *a, const Mapping *b)
{
    if (sNumVerts + 6 > MAX_VERTS) {
        return;
    }
    float x0 = r->x0, y0 = r->y0, x1 = r->x1, y1 = r->y1;
    float u0 = (x0 + a->offX) / a->texW, u1 = (x1 + a->offX) / a->texW;
    float v0 = 1.0f - (y0 + a->offY) / a->texH, v1 = 1.0f - (y1 + a->offY) / a->texH;
    float bu0 = 0, bu1 = 0, bv0 = 0, bv1 = 0;
    if (b != NULL) {
        bu0 = (x0 + b->offX) / b->texW, bu1 = (x1 + b->offX) / b->texW;
        bv0 = 1.0f - (y0 + b->offY) / b->texH, bv1 = 1.0f - (y1 + b->offY) / b->texH;
    }
    Vertex *v = &sVerts[sNumVerts];
    v[0] = (Vertex){ x0, y0, u0, v0, bu0, bv0 };
    v[1] = (Vertex){ x0, y1, u0, v1, bu0, bv1 };
    v[2] = (Vertex){ x1, y0, u1, v0, bu1, bv0 };
    v[3] = (Vertex){ x1, y0, u1, v0, bu1, bv0 };
    v[4] = (Vertex){ x0, y1, u0, v1, bu0, bv1 };
    v[5] = (Vertex){ x1, y1, u1, v1, bu1, bv1 };
    sNumVerts += 6;
}

/* The texture and texel mapping of a pass source; false if there is none. */
static bool SourceMapping(EngineImages *s, int source, C3D_Tex *tex3d, Mapping *m)
{
    const NtrComposition *c = &s->comp;
    *m = (Mapping){ NULL, 0.0f, 0.0f, LAYER_TEX_W, LAYER_TEX_H };
    switch (source) {
    case NTR_SRC_BG0:
    case NTR_SRC_BG1:
    case NTR_SRC_BG2:
    case NTR_SRC_BG3:
        m->tex = &s->bgTex[source - NTR_SRC_BG0];
        return true;
    case NTR_SRC_OBJ:
        m->tex = &s->objTex;
        return true;
    case NTR_SRC_3D:
        *m = (Mapping){ tex3d, (float)c->ofs3D, 0.0f, TEX3D_W, TEX3D_H };
        return tex3d != NULL;
    default:
        return true; /* a color */
    }
}

static void SetBlend(int blend, int eva, int evb)
{
    switch (blend) {
    case NTR_BLEND_EVA_EVB: {
        u32 a = (u32)(eva * 255 + 8) / 16, b = (u32)(evb * 255 + 8) / 16;
        C3D_BlendingColor(b << 24 | a << 16 | a << 8 | a);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_CONSTANT_COLOR, GPU_CONSTANT_ALPHA, GPU_ONE, GPU_ZERO);
        break;
    }
    case NTR_BLEND_SRC_ALPHA:
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_ONE, GPU_ZERO);
        break;
    default:
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
        break;
    }
}

void Gpu2D_Draw(int engine, C3D_RenderTarget *target, int outW, int outH)
{
    EngineImages *s = &sEngines[engine];
    const NtrComposition *c = &s->comp;
    C3D_Tex *tex3d = Gpu3D_Texture();

    /* The textures first: the transfers run ahead of these draws. */
    if (s->software) {
        Transfer(s->picture, 2, PICTURE_W, outH, &s->pictureTex, GX_TRANSFER_FMT_RGB5A1);
    } else {
        bool used[NTR_SRC_3D + 1] = { false };
        for (int i = 0; i < c->numPasses; i++) {
            used[c->passes[i].source] = true;
            used[c->passes[i].below] = true;
        }
        for (int i = 0; i < 4; i++) {
            s->bgDirty[i] |= (c->changedBg >> i) & 1;
            if (used[NTR_SRC_BG0 + i] && s->bgDirty[i]) {
                Transfer(s->bg[i], 2, LAYER_TEX_W, outH, &s->bgTex[i], GX_TRANSFER_FMT_RGB5A1);
                s->bgDirty[i] = false;
            }
        }
        /* Only the rows of the OBJ image that changed (the transfer engine
         * works on 8-row strips, which the tiled texture keeps whole). */
        int r0 = c->objRow0 & ~7, r1 = (c->objRow1 + 7) & ~7;
        if (r1 > outH) {
            r1 = outH;
        }
        if (r1 > r0) {
            u32 *src = s->obj + r0 * LAYER_TEX_W;
            Gpu_FlushDataCache(src, (u32)((r1 - r0) * LAYER_TEX_W * 4));
            GX_DisplayTransfer(src, GX_BUFFER_DIM(LAYER_TEX_W, r1 - r0), (u32 *)s->objTex.data + r0 * LAYER_TEX_W,
                GX_BUFFER_DIM(LAYER_TEX_W, r1 - r0), TRANSFER_FLAGS(GX_TRANSFER_FMT_RGBA8));
        }
        if (c->objWindow) {
            Transfer(s->objWin, 2, LAYER_TEX_W, outH, &s->objWinTex, GX_TRANSFER_FMT_RGB5A1);
        }
    }

    C3D_FrameDrawOn(target);
    C3D_BindProgram(&sProgram);
    C3D_Mtx proj;
    Mtx_OrthoTilt(&proj, 0.0f, (float)outW, (float)outH, 0.0f, 1.0f, -1.0f, true);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, sProjLoc, &proj);
    C3D_AttrInfo *attr = C3D_GetAttrInfo();
    AttrInfo_Init(attr);
    AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 2); /* x, y */
    AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 2); /* u, v */
    AttrInfo_AddLoader(attr, 2, GPU_FLOAT, 2); /* u1, v1 */
    C3D_BufInfo *buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, sVerts, sizeof(Vertex), 3, 0x210);
    C3D_CullFace(GPU_CULL_NONE);
    for (int i = 2; i < 6; i++) {
        C3D_TexEnvInit(C3D_GetTexEnv(i));
    }
    C3D_StencilOp(GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_REPLACE);

    int first = sNumVerts;
    C3D_FVUnifSet(GPU_VERTEX_SHADER, sZLoc, Z_OUTSIDE, 0.0f, 0.0f, 0.0f);
    if (!s->software && c->objWindow) {
        /* The OBJ window's pixels at their own depth: everything at
         * Z_OUTSIDE first, then the window's shape (by its alpha). */
        NtrRect all = { 0, 0, (s16)outW, (s16)outH };
        Mapping shape = { &s->objWinTex, 0.0f, 0.0f, LAYER_TEX_W, LAYER_TEX_H };
        SetSource(&s->objWinTex, 0);
        SetBlend(NTR_BLEND_NONE, 0, 0);
        C3D_StencilTest(false, GPU_ALWAYS, 0, 0, 0);
        C3D_DepthTest(true, GPU_ALWAYS, GPU_WRITE_DEPTH);
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
        int start = sNumVerts;
        AddRect(&all, &shape, NULL);
        C3D_DrawArrays(GPU_TRIANGLES, start, sNumVerts - start);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, sZLoc, Z_OBJ_WINDOW, 0.0f, 0.0f, 0.0f);
        C3D_AlphaTest(true, GPU_GREATER, 0);
        start = sNumVerts;
        AddRect(&all, &shape, NULL);
        C3D_DrawArrays(GPU_TRIANGLES, start, sNumVerts - start);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, sZLoc, Z_OUTSIDE, 0.0f, 0.0f, 0.0f);
    }
    if (s->software) {
        NtrRect all = { 0, 0, (s16)outW, (s16)outH };
        SetSource(&s->pictureTex, 0);
        SetBlend(NTR_BLEND_NONE, 0, 0);
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
        C3D_StencilTest(false, GPU_ALWAYS, 0, 0, 0);
        C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
        Mapping whole = { &s->pictureTex, 0.0f, 0.0f, PICTURE_W, PICTURE_H };
        AddRect(&all, &whole, NULL);
        C3D_DrawArrays(GPU_TRIANGLES, first, sNumVerts - first);
    } else {
        for (int i = 0; i < c->numPasses; i++) {
            const NtrPass *p = &c->passes[i];
            Mapping src, below;
            if (!SourceMapping(s, p->source, tex3d, &src)) {
                continue; /* no 3D frame yet */
            }
            bool overImage = p->below != NTR_SRC_COLOR;
            if (overImage && (!SourceMapping(s, p->below, tex3d, &below) || src.tex == NULL)) {
                continue;
            }
            if (overImage) {
                SetSourceOverImage(src.tex, below.tex, p->blend, p->eva, p->evb);
                SetBlend(NTR_BLEND_NONE, 0, 0);
            } else {
                SetSource(src.tex, p->color);
                SetBlend(p->blend, p->eva, p->evb);
            }
            C3D_AlphaTest(p->alphaTest != NTR_TEST_ALWAYS, TestFunc(p->alphaTest), p->alphaRef);
            C3D_StencilTest(p->stencilTest != NTR_TEST_ALWAYS || p->stencilWrite != 0, TestFunc(p->stencilTest), p->stencilRef,
                p->stencilMask, p->stencilWrite);
            if (p->objWindow) {
                /* Only inside or outside the OBJ window: its depth. */
                C3D_FVUnifSet(GPU_VERTEX_SHADER, sZLoc, p->objWindow == 1 ? Z_OBJ_WINDOW : Z_OUTSIDE, 0.0f, 0.0f, 0.0f);
                C3D_DepthTest(true, GPU_EQUAL, p->colorWrite ? GPU_WRITE_COLOR : 0);
            } else {
                C3D_FVUnifSet(GPU_VERTEX_SHADER, sZLoc, Z_OUTSIDE, 0.0f, 0.0f, 0.0f);
                C3D_DepthTest(false, GPU_ALWAYS, p->colorWrite ? GPU_WRITE_COLOR : 0);
            }
            int start = sNumVerts;
            for (int k = 0; k < p->numRects; k++) {
                AddRect(&c->rects[p->firstRect + k], &src, overImage ? &below : NULL);
            }
            if (sNumVerts > start) {
                C3D_DrawArrays(GPU_TRIANGLES, start, sNumVerts - start);
            }
        }
    }
    Gpu_FlushDataCache(&sVerts[first], (u32)(sNumVerts - first) * sizeof(Vertex));

    /* Leave the state as other citro3d users expect it. */
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_StencilTest(false, GPU_ALWAYS, 0, 0, 0);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    SetBlend(NTR_BLEND_SRC_ALPHA, 0, 0);
}
