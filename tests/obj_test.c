/*
 * Sprite renderer test with real ROM data: Pokemon icons are loaded through
 * the decomp's own graphics.c, then OAM entries exercise each OBJ feature.
 * Writes obj_test.png (main engine) for visual checking.
 *
 * Row 1: plain, h-flip, v-flip, affine 45 degrees, affine double-size 1.5x
 * Row 2: OBJ priority vs. a BG (priority 1 bar across the middle):
 *        prio 0 sprite (in front of the bar), prio 2 sprite (behind it),
 *        semi-transparent sprite over the bar, and two overlapping sprites
 *        (lower OAM index wins).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "constants/graphics.h"
#include "constants/heap.h"

#include "graphics.h"
#include "ntr_shim.h"

#define ICON_FIRST_MEMBER 7
#define SPECIES_PIKACHU 25
#define SPECIES_PIPLUP 393
#define SPECIES_TURTWIG 387
#define SPECIES_CHIMCHAR 390

static u8 sOam[1024];

static void SetObj(int i, int x, int y, int shape, int size, int tile, int pal, int prio, u16 extra0, u16 extra1)
{
    u16 a0 = (u16)((y & 0xff) | (shape << 14) | extra0);
    u16 a1 = (u16)((x & 0x1ff) | (size << 14) | extra1);
    u16 a2 = (u16)(tile | (prio << 10) | (pal << 12));
    sOam[i * 8 + 0] = (u8)a0;
    sOam[i * 8 + 1] = (u8)(a0 >> 8);
    sOam[i * 8 + 2] = (u8)a1;
    sOam[i * 8 + 3] = (u8)(a1 >> 8);
    sOam[i * 8 + 4] = (u8)a2;
    sOam[i * 8 + 5] = (u8)(a2 >> 8);
}

static void SetAffine(int idx, double degrees, double scale)
{
    double r = degrees * 3.14159265358979 / 180.0;
    s16 pa = (s16)(cos(r) / scale * 256), pb = (s16)(-sin(r) / scale * 256);
    s16 pc = (s16)(sin(r) / scale * 256), pd = (s16)(cos(r) / scale * 256);
    s16 v[4] = { pa, pb, pc, pd };
    for (int k = 0; k < 4; k++) {
        sOam[idx * 32 + 6 + k * 8] = (u8)v[k];
        sOam[idx * 32 + 7 + k * 8] = (u8)(v[k] >> 8);
    }
}

int main(int argc, char **argv)
{
    NtrFS_SetRoot(argc > 1 ? argv[1] : "romfs");
    const char *out = argc > 2 ? argv[2] : "obj_test.png";

    GX_Init();
    GX_SetDispSelect(GX_DISP_SELECT_MAIN_SUB);
    GX_SetBankForBG(GX_VRAM_BG_128_A);
    GX_SetBankForOBJ(GX_VRAM_OBJ_128_B);
    GX_SetGraphicsMode(GX_DISPMODE_GRAPHICS, GX_BGMODE_0, GX_BG0_AS_2D);
    GX_SetOBJVRamModeChar(GX_OBJVRAMMODE_CHAR_1D_32K);
    GX_SetVisiblePlane(GX_PLANEMASK_BG0 | GX_PLANEMASK_OBJ);

    /* Four icons, 1KB each (two 32x32 frames), back to back in OBJ VRAM. */
    const int species[4] = { SPECIES_PIPLUP, SPECIES_PIKACHU, SPECIES_TURTWIG, SPECIES_CHIMCHAR };
    for (int i = 0; i < 4; i++) {
        Graphics_LoadObjectTiles(NARC_INDEX_POKETOOL__ICONGRA__PL_POKE_ICON, ICON_FIRST_MEMBER + species[i], DS_SCREEN_MAIN, i * 0x400, 0, FALSE, HEAP_ID_SYSTEM);
    }
    Graphics_LoadPalette(NARC_INDEX_POKETOOL__ICONGRA__PL_POKE_ICON, 0, PAL_LOAD_MAIN_OBJ, 0, 0, HEAP_ID_SYSTEM);

    /* Backdrop and a priority-1 BG bar (solid tile) across y = 112..144. */
    u16 bgPal[16] = { GX_RGB(6, 10, 16), GX_RGB(28, 8, 8) };
    GX_LoadBGPltt(bgPal, 0, sizeof(bgPal));
    G2_SetBG0Control(GX_BG_SCRSIZE_TEXT_256x256, GX_BG_COLORMODE_16, GX_BG_SCRBASE_0x0000, GX_BG_CHARBASE_0x04000, GX_BG_EXTPLTT_01);
    G2_SetBG0Priority(1);
    u8 solid[64] = { 0 };
    memset(solid + 32, 0x11, 32); /* tile 1: color index 1 */
    GX_LoadBG0Char(solid, 0, sizeof(solid));
    static u16 map[32 * 32];
    for (int ty = 14; ty < 18; ty++) {
        for (int tx = 0; tx < 32; tx++) {
            map[ty * 32 + tx] = 1;
        }
    }
    GX_LoadBG0Scr(map, 0, sizeof(map));

    /* Hide all sprites, then place the test ones. 1D 32K: tile index = byte offset / 32. */
    for (int i = 0; i < 128; i++) {
        SetObj(i, 0, 192, 0, 0, 0, 0, 0, 0x200, 0);
    }
    const int SQ = 0, S32 = 2;
    SetObj(0, 8, 16, SQ, S32, 0x00, 0, 0, 0, 0);
    SetObj(1, 56, 16, SQ, S32, 0x20, 1, 0, 0, 0x1000);
    SetObj(2, 104, 16, SQ, S32, 0x40, 1, 0, 0, 0x2000);
    SetAffine(0, 45, 1.0);
    SetObj(3, 152, 16, SQ, S32, 0x60, 0, 0, 0x100, 0 << 9);
    SetAffine(1, 0, 1.5);
    SetObj(4, 192, 0, SQ, S32, 0x00, 0, 0, 0x300, 1 << 9);

    SetObj(5, 8, 112, SQ, S32, 0x20, 1, 0, 0, 0);
    SetObj(6, 56, 112, SQ, S32, 0x40, 1, 2, 0, 0);
    SetObj(7, 104, 112, SQ, S32, 0x60, 0, 0, 0x400, 0);
    SetObj(8, 160, 96, SQ, S32, 0x00, 0, 0, 0, 0);
    SetObj(9, 176, 104, SQ, S32, 0x20, 1, 0, 0, 0);
    G2_SetBlendAlpha(GX_BLEND_PLANEMASK_NONE, GX_BLEND_PLANEMASK_BG0 | GX_BLEND_PLANEMASK_BD, 8, 8);

    GX_LoadOAM(sOam, 0, sizeof(sOam));

    static u32 top[256 * 192], bottom[256 * 192];
    Ntr_RenderScreens(top, bottom);

    extern void WritePngFile(const char *path, const u32 *rgba, int w, int h);
    WritePngFile(out, top, 256, 192);
    printf("wrote %s\n", out);
    return 0;
}

/* The test never waits for VBlank; the platform still needs the hooks. */
void NtrHost_VBlank(void) {}
void NtrHost_Reset(u32 parameter) { (void)parameter; }

u64 NtrHost_Ticks(void) { return 0; }
u64 NtrHost_TicksPerSecond(void) { return 1; }
