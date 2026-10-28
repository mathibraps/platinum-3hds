/*
 * DS 2D hardware support: the emulated memory map, VRAM bank mapping, and the
 * NitroSDK GX/G2 library functions that are not inline in the SDK headers
 * (bank setup, VRAM/palette/OAM transfers, BG pointer queries, affine and
 * blend register setters). Inline SDK helpers write the emulated registers
 * directly; ntr_render.c turns the register image into pixels.
 *
 * VRAM banks A-I are stored in LCDC order (gNtrLcdcVram). When banks are
 * mapped to a region (BG, OBJ, extended palettes, textures...), their
 * contents are copied into that region's view, and back out when unmapped,
 * so data survives remapping as on the hardware.
 */
#include <stdlib.h>

#include "ntr_shim.h"

/* Emulated memory -------------------------------------------------------------- */

u8 gNtrIoRegs[NTR_IOREG_SIZE] ATTRIBUTE_ALIGN(32);
u8 gNtrPltt[0x800] ATTRIBUTE_ALIGN(32);
u8 gNtrOam[0x800] ATTRIBUTE_ALIGN(32);
u8 gNtrBgVram[0x80000] ATTRIBUTE_ALIGN(32);
u8 gNtrObjVram[0x40000] ATTRIBUTE_ALIGN(32);
u8 gNtrDbBgVram[0x20000] ATTRIBUTE_ALIGN(32);
u8 gNtrDbObjVram[0x20000] ATTRIBUTE_ALIGN(32);
u8 gNtrLcdcVram[NTR_LCDC_VRAM_SIZE] ATTRIBUTE_ALIGN(32);
u8 gNtrSysArea[NTR_SYSAREA_SIZE] ATTRIBUTE_ALIGN(32);
u8 gNtrDtcm[0x4000] ATTRIBUTE_ALIGN(32);
u8 gNtrItcm[0x8000] ATTRIBUTE_ALIGN(32);
u8 gNtrWram[0x8000] ATTRIBUTE_ALIGN(32);
u8 gNtrCtrdgRom[0x10000] ATTRIBUTE_ALIGN(32);
u8 gNtrCtrdgRam[0x10000] ATTRIBUTE_ALIGN(32);

static u8 sBgExtPltt[2][0x8000];
static u8 sObjExtPltt[2][0x2000];
static u8 sTexVram[0x80000];
static u8 sTexPltt[0x18000];

u8 *gNtrTexVram = sTexVram;
u8 *gNtrTexPltt = sTexPltt;

NtrEngine gNtrEngines[2] = {
    { 0x0000, NULL, 0, NULL, 0, NULL, NULL, (u16 *)(gNtrPltt + 0x000), (u16 *)(gNtrPltt + 0x200), gNtrOam + 0x000 },
    { 0x1000, NULL, 0, NULL, 0, NULL, NULL, (u16 *)(gNtrPltt + 0x400), (u16 *)(gNtrPltt + 0x600), gNtrOam + 0x400 },
};

#define MAIN (&gNtrEngines[0])
#define SUB (&gNtrEngines[1])

u32 GXi_DmaId = GX_DMA_NOT_USE; /* no DMA: transfers are plain copies */

/* VRAM banks ------------------------------------------------------------------------ */

enum { BANK_A, BANK_B, BANK_C, BANK_D, BANK_E, BANK_F, BANK_G, BANK_H, BANK_I, NUM_BANKS };

static const u32 sBankSize[NUM_BANKS] = { 0x20000, 0x20000, 0x20000, 0x20000, 0x10000, 0x4000, 0x4000, 0x8000, 0x4000 };
static const u32 sBankLcdcOffset[NUM_BANKS] = { 0x00000, 0x20000, 0x40000, 0x60000, 0x80000, 0x90000, 0x94000, 0x98000, 0xA0000 };

/* Where each bank currently lives: a region view (NULL = LCDC/unmapped) and the
 * byte range in it. */
static struct {
    u8 *view;
    u32 offset;
    u32 size;
} sBankMap[NUM_BANKS];

enum Region { REGION_BG, REGION_BGEXT, REGION_OBJ, REGION_OBJEXT, REGION_TEX, REGION_TEXPLTT, REGION_SUBBG, REGION_SUBBGEXT, REGION_SUBOBJ, REGION_SUBOBJEXT, REGION_LCDC, REGION_ARM7, NUM_REGIONS };

static int sRegionBanks[NUM_REGIONS];

static void UnmapBank(int b)
{
    if (sBankMap[b].view) {
        memcpy(gNtrLcdcVram + sBankLcdcOffset[b], sBankMap[b].view + sBankMap[b].offset, sBankMap[b].size);
        sBankMap[b].view = NULL;
    }
}

u32 gNtrTexWrites;
u32 gNtrTexVramPage[0x80000 >> NTR_TEX_PAGE_SHIFT];
u32 gNtrTexPlttPage[0x18000 >> NTR_TEX_PAGE_SHIFT];

void Ntr_TexMemoryWritten(BOOL palette, u32 addr, u32 size)
{
    u32 *pages = palette ? gNtrTexPlttPage : gNtrTexVramPage;
    u32 count = palette ? 0x18000 >> NTR_TEX_PAGE_SHIFT : 0x80000 >> NTR_TEX_PAGE_SHIFT;
    if (size == 0) {
        return;
    }
    u32 stamp = ++gNtrTexWrites;
    for (u32 p = addr >> NTR_TEX_PAGE_SHIFT; p <= (addr + size - 1) >> NTR_TEX_PAGE_SHIFT && p < count; p++) {
        pages[p] = stamp;
    }
}

static void MapBank(int b, u8 *view, u32 offset, u32 size)
{
    UnmapBank(b);
    if (size > sBankSize[b]) {
        size = sBankSize[b];
    }
    sBankMap[b].view = view;
    sBankMap[b].offset = offset;
    sBankMap[b].size = size;
    memcpy(view + offset, gNtrLcdcVram + sBankLcdcOffset[b], size);
    if (view == sTexVram || view == sTexPltt) {
        Ntr_TexMemoryWritten(view == sTexPltt, offset, size);
    }
}

static void UnmapRegion(enum Region r)
{
    for (int b = 0; b < NUM_BANKS; b++) {
        if (sRegionBanks[r] & (1 << b)) {
            UnmapBank(b);
        }
    }
    sRegionBanks[r] = 0;
}

/* Maps banks to a region consecutively in letter order (how the DS lays out
 * multi-bank BG/OBJ/texture configurations). Returns the mapped size. */
static u32 MapConsecutive(enum Region r, int banks, u8 *view, u32 viewSize)
{
    UnmapRegion(r);
    sRegionBanks[r] = banks;
    u32 offset = 0;
    for (int b = 0; b < NUM_BANKS; b++) {
        if (banks & (1 << b)) {
            if (offset < viewSize) {
                MapBank(b, view, offset, viewSize - offset < sBankSize[b] ? viewSize - offset : sBankSize[b]);
            }
            offset += sBankSize[b];
        }
    }
    return offset < viewSize ? offset : viewSize;
}

void GX_SetBankForBG(GXVRamBG bg)
{
    MAIN->bgVramSize = MapConsecutive(REGION_BG, bg, gNtrBgVram, sizeof(gNtrBgVram));
    MAIN->bgVram = bg ? gNtrBgVram : NULL;
}

void GX_SetBankForOBJ(GXVRamOBJ obj)
{
    MAIN->objVramSize = MapConsecutive(REGION_OBJ, obj, gNtrObjVram, sizeof(gNtrObjVram));
    MAIN->objVram = obj ? gNtrObjVram : NULL;
}

void GX_SetBankForSubBG(GXVRamSubBG bg)
{
    SUB->bgVramSize = MapConsecutive(REGION_SUBBG, bg, gNtrDbBgVram, sizeof(gNtrDbBgVram));
    SUB->bgVram = bg ? gNtrDbBgVram : NULL;
}

void GX_SetBankForSubOBJ(GXVRamSubOBJ obj)
{
    SUB->objVramSize = MapConsecutive(REGION_SUBOBJ, obj, gNtrDbObjVram, sizeof(gNtrDbObjVram));
    SUB->objVram = obj ? gNtrDbObjVram : NULL;
}

/* Extended palettes: E holds slots 0-3, F slots 0-1, G slots 2-3. */
void GX_SetBankForBGExtPltt(GXVRamBGExtPltt ext)
{
    UnmapRegion(REGION_BGEXT);
    sRegionBanks[REGION_BGEXT] = ext;
    if (ext & GX_VRAM_E) {
        MapBank(BANK_E, sBgExtPltt[0], 0, 0x8000);
    }
    if (ext & GX_VRAM_F) {
        MapBank(BANK_F, sBgExtPltt[0], 0, 0x4000);
    }
    if (ext & GX_VRAM_G) {
        MapBank(BANK_G, sBgExtPltt[0], 0x4000, 0x4000);
    }
    MAIN->bgExtPltt = ext ? sBgExtPltt[0] : NULL;
    if (ext) {
        NTR_IO32(0x000) |= REG_GX_DISPCNT_BG_MASK;
    } else {
        NTR_IO32(0x000) &= ~REG_GX_DISPCNT_BG_MASK;
    }
}

void GX_SetBankForOBJExtPltt(GXVRamOBJExtPltt ext)
{
    UnmapRegion(REGION_OBJEXT);
    sRegionBanks[REGION_OBJEXT] = ext;
    if (ext & GX_VRAM_F) {
        MapBank(BANK_F, sObjExtPltt[0], 0, 0x2000);
    }
    if (ext & GX_VRAM_G) {
        MapBank(BANK_G, sObjExtPltt[0], 0, 0x2000);
    }
    MAIN->objExtPltt = ext ? sObjExtPltt[0] : NULL;
    if (ext) {
        NTR_IO32(0x000) |= REG_GX_DISPCNT_O_MASK;
    } else {
        NTR_IO32(0x000) &= ~REG_GX_DISPCNT_O_MASK;
    }
}

void GX_SetBankForSubBGExtPltt(GXVRamSubBGExtPltt ext)
{
    UnmapRegion(REGION_SUBBGEXT);
    sRegionBanks[REGION_SUBBGEXT] = ext;
    if (ext & GX_VRAM_H) {
        MapBank(BANK_H, sBgExtPltt[1], 0, 0x8000);
    }
    SUB->bgExtPltt = ext ? sBgExtPltt[1] : NULL;
    if (ext) {
        NTR_IO32(0x1000) |= REG_GXS_DB_DISPCNT_BG_MASK;
    } else {
        NTR_IO32(0x1000) &= ~REG_GXS_DB_DISPCNT_BG_MASK;
    }
}

void GX_SetBankForSubOBJExtPltt(GXVRamSubOBJExtPltt ext)
{
    UnmapRegion(REGION_SUBOBJEXT);
    sRegionBanks[REGION_SUBOBJEXT] = ext;
    if (ext & GX_VRAM_I) {
        MapBank(BANK_I, sObjExtPltt[1], 0, 0x2000);
    }
    SUB->objExtPltt = ext ? sObjExtPltt[1] : NULL;
    if (ext) {
        NTR_IO32(0x1000) |= REG_GXS_DB_DISPCNT_O_MASK;
    } else {
        NTR_IO32(0x1000) &= ~REG_GXS_DB_DISPCNT_O_MASK;
    }
}

/* As in the SDK, mapping texture banks also switches texture mapping on/off. */
void GX_SetBankForTex(GXVRamTex tex)
{
    MapConsecutive(REGION_TEX, tex, sTexVram, sizeof(sTexVram));
    if (tex) {
        NTR_IO16(0x060) = (u16)((NTR_IO16(0x060) & ~(REG_G3X_DISP3DCNT_RO_MASK | REG_G3X_DISP3DCNT_GO_MASK)) | REG_G3X_DISP3DCNT_TME_MASK);
    } else {
        NTR_IO16(0x060) &= (u16) ~(REG_G3X_DISP3DCNT_TME_MASK | REG_G3X_DISP3DCNT_RO_MASK | REG_G3X_DISP3DCNT_GO_MASK);
    }
}

/* Texture palette slots are 16KB; E covers slots 0-3, F/G one slot each. */
void GX_SetBankForTexPltt(GXVRamTexPltt texPltt)
{
    MapConsecutive(REGION_TEXPLTT, texPltt, sTexPltt, sizeof(sTexPltt));
}

void GX_SetBankForLCDC(int lcdc)
{
    for (int b = 0; b < NUM_BANKS; b++) {
        if (lcdc & (1 << b)) {
            for (int r = 0; r < NUM_REGIONS; r++) {
                if (r != REGION_LCDC && (sRegionBanks[r] & (1 << b))) {
                    UnmapBank(b);
                    sRegionBanks[r] &= ~(1 << b);
                }
            }
        }
    }
    sRegionBanks[REGION_LCDC] |= lcdc;
}

void GX_SetBankForARM7(GXVRamARM7 arm7)
{
    UnmapRegion(REGION_ARM7);
    sRegionBanks[REGION_ARM7] = arm7;
}

#define DEFINE_BANK_QUERIES(name, region, type, setter) \
    type GX_ResetBankFor##name(void)                    \
    {                                                   \
        type old = (type)sRegionBanks[region];          \
        setter((type)0);                                \
        return old;                                     \
    }                                                   \
    type GX_DisableBankFor##name(void)                  \
    {                                                   \
        return GX_ResetBankFor##name();                 \
    }                                                   \
    type GX_GetBankFor##name(void)                      \
    {                                                   \
        return (type)sRegionBanks[region];              \
    }

DEFINE_BANK_QUERIES(BG, REGION_BG, GXVRamBG, GX_SetBankForBG)
DEFINE_BANK_QUERIES(BGExtPltt, REGION_BGEXT, GXVRamBGExtPltt, GX_SetBankForBGExtPltt)
DEFINE_BANK_QUERIES(OBJ, REGION_OBJ, GXVRamOBJ, GX_SetBankForOBJ)
DEFINE_BANK_QUERIES(OBJExtPltt, REGION_OBJEXT, GXVRamOBJExtPltt, GX_SetBankForOBJExtPltt)
DEFINE_BANK_QUERIES(Tex, REGION_TEX, GXVRamTex, GX_SetBankForTex)
DEFINE_BANK_QUERIES(TexPltt, REGION_TEXPLTT, GXVRamTexPltt, GX_SetBankForTexPltt)
DEFINE_BANK_QUERIES(SubBG, REGION_SUBBG, GXVRamSubBG, GX_SetBankForSubBG)
DEFINE_BANK_QUERIES(SubBGExtPltt, REGION_SUBBGEXT, GXVRamSubBGExtPltt, GX_SetBankForSubBGExtPltt)
DEFINE_BANK_QUERIES(SubOBJ, REGION_SUBOBJ, GXVRamSubOBJ, GX_SetBankForSubOBJ)
DEFINE_BANK_QUERIES(SubOBJExtPltt, REGION_SUBOBJEXT, GXVRamSubOBJExtPltt, GX_SetBankForSubOBJExtPltt)
DEFINE_BANK_QUERIES(ARM7, REGION_ARM7, GXVRamARM7, GX_SetBankForARM7)

GXVRamLCDC GX_ResetBankForLCDC(void)
{
    GXVRamLCDC old = (GXVRamLCDC)sRegionBanks[REGION_LCDC];
    sRegionBanks[REGION_LCDC] = 0;
    return old;
}

GXVRamLCDC GX_DisableBankForLCDC(void) { return GX_ResetBankForLCDC(); }
GXVRamLCDC GX_GetBankForLCDC(void) { return (GXVRamLCDC)sRegionBanks[REGION_LCDC]; }

/* Display control (gx.c) ------------------------------------------------------ */

void GX_Init(void)
{
    NTR_IO16(0x304) |= 0x8000; /* POWCNT: main engine on the top screen */
    NTR_IO32(0x000) = 0;
    NTR_IO32(0x1000) = 0x10000; /* sub engine: graphics display */
}

void GX_SetGraphicsMode(GXDispMode dispMode, GXBGMode bgMode, GXBG0As bg0_2d3d)
{
    u32 v = NTR_IO32(0x000);
    v &= ~(REG_GX_DISPCNT_BGMODE_MASK | REG_GX_DISPCNT_BG02D3D_MASK | REG_GX_DISPCNT_MODE_MASK);
    v |= ((u32)dispMode << REG_GX_DISPCNT_MODE_SHIFT) | ((u32)bgMode << REG_GX_DISPCNT_BGMODE_SHIFT) | ((u32)bg0_2d3d << REG_GX_DISPCNT_BG02D3D_SHIFT);
    NTR_IO32(0x000) = v;
}

void GXS_SetGraphicsMode(GXBGMode bgMode)
{
    NTR_IO32(0x1000) = (NTR_IO32(0x1000) & ~REG_GXS_DB_DISPCNT_BGMODE_MASK) | ((u32)bgMode << REG_GXS_DB_DISPCNT_BGMODE_SHIFT) | 0x10000;
}

void GX_DispOff(void) { NTR_IO32(0x000) &= ~REG_GX_DISPCNT_MODE_MASK; }

void GX_DispOn(void)
{
    if (!(NTR_IO32(0x000) & REG_GX_DISPCNT_MODE_MASK)) {
        NTR_IO32(0x000) |= (u32)GX_DISPMODE_GRAPHICS << REG_GX_DISPCNT_MODE_SHIFT;
    }
}

/* VRAM / palette / OAM transfers (gx_load.c) ------------------------------------- */

static void CopyIn(u8 *base, u32 limit, u32 offset, const void *src, u32 size)
{
    if (base == NULL || offset >= limit) {
        return;
    }
    if (offset + size > limit) {
        size = limit - offset;
    }
    memcpy(base + offset, src, size);
}

static u32 CharBase(const NtrEngine *e, int bg)
{
    u32 base = ((NTR_IO16(e->io + 0x008 + bg * 2) >> 2) & 0xf) * 0x4000;
    if (e == MAIN) {
        base += ((NTR_IO32(0x000) >> 24) & 7) * 0x10000;
    }
    return base;
}

static u32 ScreenBase(const NtrEngine *e, int bg)
{
    u32 base = ((NTR_IO16(e->io + 0x008 + bg * 2) >> 8) & 0x1f) * 0x800;
    if (e == MAIN) {
        base += ((NTR_IO32(0x000) >> 27) & 7) * 0x10000;
    }
    return base;
}

#define DEFINE_LOADS(P, G, E, IDX)                                                                                                   \
    void P##_LoadBG0Char(const void *s, u32 o, u32 n) { CopyIn((E)->bgVram, (E)->bgVramSize, CharBase(E, 0) + o, s, n); }           \
    void P##_LoadBG1Char(const void *s, u32 o, u32 n) { CopyIn((E)->bgVram, (E)->bgVramSize, CharBase(E, 1) + o, s, n); }           \
    void P##_LoadBG2Char(const void *s, u32 o, u32 n) { CopyIn((E)->bgVram, (E)->bgVramSize, CharBase(E, 2) + o, s, n); }           \
    void P##_LoadBG3Char(const void *s, u32 o, u32 n) { CopyIn((E)->bgVram, (E)->bgVramSize, CharBase(E, 3) + o, s, n); }           \
    void P##_LoadBG0Scr(const void *s, u32 o, u32 n) { CopyIn((E)->bgVram, (E)->bgVramSize, ScreenBase(E, 0) + o, s, n); }          \
    void P##_LoadBG1Scr(const void *s, u32 o, u32 n) { CopyIn((E)->bgVram, (E)->bgVramSize, ScreenBase(E, 1) + o, s, n); }          \
    void P##_LoadBG2Scr(const void *s, u32 o, u32 n) { CopyIn((E)->bgVram, (E)->bgVramSize, ScreenBase(E, 2) + o, s, n); }          \
    void P##_LoadBG3Scr(const void *s, u32 o, u32 n) { CopyIn((E)->bgVram, (E)->bgVramSize, ScreenBase(E, 3) + o, s, n); }          \
    void P##_LoadBGPltt(const void *s, u32 o, u32 n) { CopyIn((u8 *)(E)->bgPltt, 0x200, o, s, n); }                                \
    void P##_LoadOBJPltt(const void *s, u32 o, u32 n) { CopyIn((u8 *)(E)->objPltt, 0x200, o, s, n); }                              \
    void P##_LoadOBJ(const void *s, u32 o, u32 n) { CopyIn((E)->objVram, (E)->objVramSize, o, s, n); }                             \
    void P##_LoadOAM(const void *s, u32 o, u32 n) { CopyIn((E)->oam, 0x400, o, s, n); }                                            \
    void P##_BeginLoadBGExtPltt(void) {}                                                                                            \
    void P##_LoadBGExtPltt(const void *s, u32 o, u32 n) { CopyIn(sBgExtPltt[IDX], 0x8000, o, s, n); }                              \
    void P##_EndLoadBGExtPltt(void) {}                                                                                              \
    void P##_BeginLoadOBJExtPltt(void) {}                                                                                           \
    void P##_LoadOBJExtPltt(const void *s, u32 o, u32 n) { CopyIn(sObjExtPltt[IDX], 0x2000, o, s, n); }                            \
    void P##_EndLoadOBJExtPltt(void) {}                                                                                             \
    void *G##_GetBG0CharPtr(void) { return (E)->bgVram ? (E)->bgVram + CharBase(E, 0) : NULL; }                                     \
    void *G##_GetBG1CharPtr(void) { return (E)->bgVram ? (E)->bgVram + CharBase(E, 1) : NULL; }                                     \
    void *G##_GetBG2CharPtr(void) { return (E)->bgVram ? (E)->bgVram + CharBase(E, 2) : NULL; }                                     \
    void *G##_GetBG3CharPtr(void) { return (E)->bgVram ? (E)->bgVram + CharBase(E, 3) : NULL; }                                     \
    void *G##_GetBG0ScrPtr(void) { return (E)->bgVram ? (E)->bgVram + ScreenBase(E, 0) : NULL; }                                    \
    void *G##_GetBG1ScrPtr(void) { return (E)->bgVram ? (E)->bgVram + ScreenBase(E, 1) : NULL; }                                    \
    void *G##_GetBG2ScrPtr(void) { return (E)->bgVram ? (E)->bgVram + ScreenBase(E, 2) : NULL; }                                    \
    void *G##_GetBG3ScrPtr(void) { return (E)->bgVram ? (E)->bgVram + ScreenBase(E, 3) : NULL; }

DEFINE_LOADS(GX, G2, MAIN, 0)
DEFINE_LOADS(GXS, G2S, SUB, 1)

/* Register setters the SDK implements out of line (g2.c) ------------------------- */

void G2x_SetBGyAffine_(u32 addr, const MtxFx22 *mtx, int centerX, int centerY, int x1, int y1)
{
    s32 dx = x1 - centerX, dy = y1 - centerY;
    fx32 x2 = mtx->_00 * dx + mtx->_01 * dy + (centerX << FX32_SHIFT);
    fx32 y2 = mtx->_10 * dx + mtx->_11 * dy + (centerY << FX32_SHIFT);
    ((vu32 *)addr)[0] = (u32)((u16)(s16)(mtx->_00 >> 4) | (u16)(s16)(mtx->_01 >> 4) << 16);
    ((vu32 *)addr)[1] = (u32)((u16)(s16)(mtx->_10 >> 4) | (u16)(s16)(mtx->_11 >> 4) << 16);
    ((vu32 *)addr)[2] = (u32)(x2 >> 4);
    ((vu32 *)addr)[3] = (u32)(y2 >> 4);
}

/* addr points at BLDCNT; BLDALPHA and BLDY follow it. */
void G2x_SetBlendAlpha_(u32 addr, int plane1, int plane2, int ev1, int ev2)
{
    *(vu32 *)addr = (u32)((plane1 | (1 << 6) | (plane2 << 8)) | ((ev1 | (ev2 << 8)) << 16));
}

void G2x_SetBlendBrightness_(u32 addr, int plane, int brightness)
{
    if (brightness < 0) {
        *(vu16 *)addr = (u16)(plane | (3 << 6));
        ((vu16 *)addr)[2] = (u16)(-brightness);
    } else {
        *(vu16 *)addr = (u16)(plane | (2 << 6));
        ((vu16 *)addr)[2] = (u16)brightness;
    }
}

void G2x_SetBlendBrightnessExt_(u32 addr, int plane1, int plane2, int ev1, int ev2, int brightness)
{
    ((vu16 *)addr)[1] = (u16)(ev1 | (ev2 << 8));
    if (brightness < 0) {
        *(vu16 *)addr = (u16)(plane1 | (3 << 6) | (plane2 << 8));
        ((vu16 *)addr)[2] = (u16)(-brightness);
    } else {
        *(vu16 *)addr = (u16)(plane1 | (2 << 6) | (plane2 << 8));
        ((vu16 *)addr)[2] = (u16)brightness;
    }
}

void G2x_ChangeBlendBrightness_(u32 addr, int brightness)
{
    u16 cnt = *(vu16 *)addr;
    if (brightness < 0) {
        if ((cnt & (3 << 6)) == (2 << 6)) {
            *(vu16 *)addr = (u16)((cnt & ~(3 << 6)) | (3 << 6));
        }
        ((vu16 *)addr)[2] = (u16)(-brightness);
    } else {
        if ((cnt & (3 << 6)) == (3 << 6)) {
            *(vu16 *)addr = (u16)((cnt & ~(3 << 6)) | (2 << 6));
        }
        ((vu16 *)addr)[2] = (u16)brightness;
    }
}

/* Reads an emulated 2D register by its I/O offset (used by a NitroSystem patch). */
u16 Ntr_ReadIoReg16(u32 offset)
{
    return NTR_IO16(offset);
}

/* Master brightness (MASTER_BRIGHT at +0x6c of each engine's registers). */
void GXx_SetMasterBrightness_(vu16 *reg, int brightness)
{
    if (brightness == 0) {
        *reg = 0;
    } else if (brightness > 0) {
        *reg = (u16)((1 << 14) | brightness);
    } else {
        *reg = (u16)((2 << 14) | -brightness);
    }
}

int GXx_GetMasterBrightness_(vu16 *reg)
{
    u16 v = *reg;
    if ((v >> 14) == 1) {
        return v & 0x1f;
    }
    if ((v >> 14) == 2) {
        return -(v & 0x1f);
    }
    return 0;
}

/* VBlank/HBlank interrupt requests (DISPSTAT bits 3 and 4). */
static s32 SetDispStatBit(u16 bit, BOOL enable)
{
    u16 prev = NTR_IO16(0x004);
    NTR_IO16(0x004) = enable ? (u16)(prev | bit) : (u16)(prev & ~bit);
    return (prev & bit) != 0;
}

s32 GX_VBlankIntr(BOOL enable)
{
    return SetDispStatBit(0x0008, enable);
}

s32 GX_HBlankIntr(BOOL enable)
{
    return SetDispStatBit(0x0010, enable);
}
