/*
 * The DS memory map, emulated.
 *
 * SDK and game code address hardware through fixed addresses (I/O registers,
 * palette RAM, OAM, VRAM, system areas). Here those base addresses point at
 * host memory, so every SDK register macro and inline helper works as
 * written: registers are plain memory that the renderer and the 3D engine
 * read, the way the DS hardware reads them.
 *
 * Registers with side effects override the SDK's definitions:
 *   - 3D command ports (0x04000400-0x040005FF) feed the geometry engine;
 *   - divider/square root result registers compute their result when read.
 *
 * Included by ntr_shim.h before the generated SDK definitions, so these
 * definitions take precedence (the generated ones are #ifndef-guarded).
 * Assumes 32-bit pointers (3DS and the wasm32 test build).
 */
#ifndef NTR_MEMMAP_H
#define NTR_MEMMAP_H

/* Backing memory ------------------------------------------------------------- */

#define NTR_IOREG_SIZE 0x101000 /* up to 0x04101000 (IPC/card ports) */
#define NTR_SYSAREA_SIZE 0x10000 /* last 64KB of the 8MB main RAM range */
#define NTR_LCDC_VRAM_SIZE 0xA4000 /* VRAM banks A-I in LCDC order */

extern u8 gNtrIoRegs[NTR_IOREG_SIZE];
extern u8 gNtrPltt[0x800];
extern u8 gNtrOam[0x800];
extern u8 gNtrBgVram[0x80000];
extern u8 gNtrObjVram[0x40000];
extern u8 gNtrDbBgVram[0x20000];
extern u8 gNtrDbObjVram[0x20000];
extern u8 gNtrLcdcVram[NTR_LCDC_VRAM_SIZE];
extern u8 gNtrSysArea[NTR_SYSAREA_SIZE];
extern u8 gNtrDtcm[0x4000];
extern u8 gNtrItcm[0x8000];
extern u8 gNtrWram[0x8000];
extern u8 gNtrCtrdgRom[0x10000];
extern u8 gNtrCtrdgRam[0x10000];

/* Typed access to the I/O register image (offset = DS address - 0x04000000). */
#define NTR_IO8(off) (*(volatile u8 *)&gNtrIoRegs[(off)])
#define NTR_IO16(off) (*(volatile u16 *)&gNtrIoRegs[(off)])
#define NTR_IO32(off) (*(volatile u32 *)&gNtrIoRegs[(off)])

/* Base addresses (SDK names; everything else in the SDK memory map derives from these) */

#define HW_IOREG ((u32)gNtrIoRegs)
#define HW_REG_BASE HW_IOREG

#define HW_BG_PLTT ((u32)gNtrPltt)
#define HW_BG_PLTT_END (HW_BG_PLTT + 0x200)
#define HW_OBJ_PLTT (HW_BG_PLTT + 0x200)
#define HW_OBJ_PLTT_END (HW_BG_PLTT + 0x400)
#define HW_DB_BG_PLTT (HW_BG_PLTT + 0x400)
#define HW_DB_BG_PLTT_END (HW_BG_PLTT + 0x600)
#define HW_DB_OBJ_PLTT (HW_BG_PLTT + 0x600)
#define HW_DB_OBJ_PLTT_END (HW_BG_PLTT + 0x800)

#define HW_BG_VRAM ((u32)gNtrBgVram)
#define HW_BG_VRAM_END (HW_BG_VRAM + 0x80000)
#define HW_DB_BG_VRAM ((u32)gNtrDbBgVram)
#define HW_DB_BG_VRAM_END (HW_DB_BG_VRAM + 0x20000)
#define HW_OBJ_VRAM ((u32)gNtrObjVram)
#define HW_OBJ_VRAM_END (HW_OBJ_VRAM + 0x40000)
#define HW_DB_OBJ_VRAM ((u32)gNtrDbObjVram)
#define HW_DB_OBJ_VRAM_END (HW_DB_OBJ_VRAM + 0x20000)
#define HW_LCDC_VRAM ((u32)gNtrLcdcVram)
#define HW_LCDC_VRAM_END (HW_LCDC_VRAM + NTR_LCDC_VRAM_SIZE)

#define HW_OAM ((u32)gNtrOam)
#define HW_OAM_END (HW_OAM + 0x400)
#define HW_DB_OAM (HW_OAM + 0x400)
#define HW_DB_OAM_END (HW_OAM + 0x800)

/* Only the system area at the top of main RAM (shared buffers, ROM header
 * copy, ...) is backed; the rest of main RAM is the host heap. */
#define HW_MAIN_MEM ((u32)gNtrSysArea + NTR_SYSAREA_SIZE - 0x800000)
#define HW_DTCM ((u32)gNtrDtcm)
#define HW_ITCM ((u32)gNtrItcm)
#define HW_WRAM ((u32)gNtrWram)
#define HW_WRAM_END (HW_WRAM + 0x8000)
#define HW_CTRDG_ROM ((u32)gNtrCtrdgRom)
#define HW_CTRDG_RAM ((u32)gNtrCtrdgRam)
#define HW_CTRDG_RAM_END (HW_CTRDG_RAM + 0x10000)

/* NitroSystem sanity-checks pointers against this range: accept everything. */
#define HW_ITCM_IMAGE 0x00000000u

#define HW_LCD_WIDTH 256
#define HW_LCD_HEIGHT 192

/* Divider and square root: results are computed when the result registers are read. */
u32 NtrCp_DivResultAddr(void);
u32 NtrCp_DivRemResultAddr(void);
u32 NtrCp_SqrtResultAddr(void);
#define REG_DIV_RESULT_ADDR NtrCp_DivResultAddr()
#define REG_DIVREM_RESULT_ADDR NtrCp_DivRemResultAddr()
#define REG_SQRT_RESULT_ADDR NtrCp_SqrtResultAddr()

/* 3D geometry engine command ports ------------------------------------------- */

void NtrGe_Command(u32 op, const u32 *params);
void NtrGe_FifoWrite(u32 word);
void NtrGe_Flush(void);
volatile u32 *NtrGe_FifoSlot(void);
volatile u32 *NtrGe_CmdSlot(u32 op);

#define reg_G3X_GXFIFO (*NtrGe_FifoSlot())
#define reg_G3_MTX_MODE (*NtrGe_CmdSlot(0x10))
#define reg_G3_MTX_PUSH (*NtrGe_CmdSlot(0x11))
#define reg_G3_MTX_POP (*NtrGe_CmdSlot(0x12))
#define reg_G3_MTX_STORE (*NtrGe_CmdSlot(0x13))
#define reg_G3_MTX_RESTORE (*NtrGe_CmdSlot(0x14))
#define reg_G3_MTX_IDENTITY (*NtrGe_CmdSlot(0x15))
#define reg_G3_MTX_LOAD_4x4 (*NtrGe_CmdSlot(0x16))
#define reg_G3_MTX_LOAD_4x3 (*NtrGe_CmdSlot(0x17))
#define reg_G3_MTX_MULT_4x4 (*NtrGe_CmdSlot(0x18))
#define reg_G3_MTX_MULT_4x3 (*NtrGe_CmdSlot(0x19))
#define reg_G3_MTX_MULT_3x3 (*NtrGe_CmdSlot(0x1a))
#define reg_G3_MTX_SCALE (*NtrGe_CmdSlot(0x1b))
#define reg_G3_MTX_TRANS (*NtrGe_CmdSlot(0x1c))
#define reg_G3_COLOR (*NtrGe_CmdSlot(0x20))
#define reg_G3_NORMAL (*NtrGe_CmdSlot(0x21))
#define reg_G3_TEXCOORD (*NtrGe_CmdSlot(0x22))
#define reg_G3_VTX_16 (*NtrGe_CmdSlot(0x23))
#define reg_G3_VTX_10 (*NtrGe_CmdSlot(0x24))
#define reg_G3_VTX_XY (*NtrGe_CmdSlot(0x25))
#define reg_G3_VTX_XZ (*NtrGe_CmdSlot(0x26))
#define reg_G3_VTX_YZ (*NtrGe_CmdSlot(0x27))
#define reg_G3_VTX_DIFF (*NtrGe_CmdSlot(0x28))
#define reg_G3_POLYGON_ATTR (*NtrGe_CmdSlot(0x29))
#define reg_G3_TEXIMAGE_PARAM (*NtrGe_CmdSlot(0x2a))
#define reg_G3_TEXPLTT_BASE (*NtrGe_CmdSlot(0x2b))
#define reg_G3_DIF_AMB (*NtrGe_CmdSlot(0x30))
#define reg_G3_SPE_EMI (*NtrGe_CmdSlot(0x31))
#define reg_G3_LIGHT_VECTOR (*NtrGe_CmdSlot(0x32))
#define reg_G3_LIGHT_COLOR (*NtrGe_CmdSlot(0x33))
#define reg_G3_SHININESS (*NtrGe_CmdSlot(0x34))
#define reg_G3_BEGIN_VTXS (*NtrGe_CmdSlot(0x40))
#define reg_G3_END_VTXS (*NtrGe_CmdSlot(0x41))
#define reg_G3_SWAP_BUFFERS (*NtrGe_CmdSlot(0x50))
#define reg_G3_VIEWPORT (*NtrGe_CmdSlot(0x60))
#define reg_G3_BOX_TEST (*NtrGe_CmdSlot(0x70))
#define reg_G3_POS_TEST (*NtrGe_CmdSlot(0x71))
#define reg_G3_VEC_TEST (*NtrGe_CmdSlot(0x72))

#endif /* NTR_MEMMAP_H */
