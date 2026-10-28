/*
 * OS services for the game's own main loop: interrupts, the VBlank wait,
 * ticks, arenas, locks, and the other OS calls the game makes.
 *
 * The game runs as on the DS: NitroMain never returns and waits for VBlank
 * with OS_WaitIrq. Each wait ends the frame: the front end presents it and
 * reads input (NtrHost_VBlank), then the registered VBlank handler runs, as
 * the interrupt would. There are no threads or real interrupts; everything
 * happens on the game's thread, in that order. (Only the sound driver, the
 * DS's other CPU, may run on a thread of its own: ntr_snd.c.)
 */
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ntr_shim.h"

/* ----------------------------------------------------------------- input */

#define REG_KEYINPUT NTR_IO16(0x130)
#define BUTTON_XY_BUF (*(vu16 *)HW_BUTTON_XY_BUF)

void Ntr_SetInput(u16 keys, BOOL touching, u16 touchX, u16 touchY)
{
    /* Key bits are active-low, as the hardware reports them. A, B, select,
     * start, the d-pad, R and L are in KEYINPUT; X and Y come from the ARM7. */
    REG_KEYINPUT = (u16)(~keys & (PAD_PLUS_KEY_MASK | 0x030f));
    BUTTON_XY_BUF = (u16)(~keys & (PAD_BUTTON_X | PAD_BUTTON_Y));
    Ntr_SetTouch(touching, touchX, touchY);
}

/* ------------------------------------------------------------- interrupts */

#define REG_IME NTR_IO16(0x208)
#define REG_IE NTR_IO32(0x210)
#define REG_IF NTR_IO32(0x214)
#define INTR_CHECK (*(vu32 *)HW_INTR_CHECK_BUF)

static OSIrqFunction sIrqTable[32];

void OS_Init(void)
{
    Ntr_SetInput(0, FALSE, 0, 0);
    OS_InitTick();
}

static int IrqIndex(OSIrqMask bit)
{
    return bit ? __builtin_ctz(bit) : 0;
}

void OS_SetIrqFunction(OSIrqMask intrBit, OSIrqFunction function)
{
    for (int i = 0; i < 32; i++) {
        if (intrBit & (1u << i)) {
            sIrqTable[i] = function;
        }
    }
}

OSIrqFunction OS_GetIrqFunction(OSIrqMask intrBit)
{
    return sIrqTable[IrqIndex(intrBit)];
}

OSIrqMask OS_SetIrqMask(OSIrqMask intr)
{
    OSIrqMask prev = REG_IE;
    REG_IE = intr;
    return prev;
}

OSIrqMask OS_EnableIrqMask(OSIrqMask intr)
{
    OSIrqMask prev = REG_IE;
    REG_IE = prev | intr;
    return prev;
}

OSIrqMask OS_DisableIrqMask(OSIrqMask intr)
{
    OSIrqMask prev = REG_IE;
    REG_IE = prev & ~intr;
    return prev;
}

OSIrqMask OS_ResetRequestIrqMask(OSIrqMask intr)
{
    OSIrqMask prev = REG_IF;
    REG_IF = prev & ~intr;
    return prev;
}

/* The game never runs inside an interrupt, so the CPU interrupt state is a no-op. */
OSIntrMode OS_DisableInterrupts(void) { return 0; }
OSIntrMode OS_EnableInterrupts(void) { return 0; }
OSIntrMode OS_RestoreInterrupts(OSIntrMode state) { return state; }

/* Raises an interrupt: runs its handler if the game enabled it. */
static void RaiseIrq(OSIrqMask bit)
{
    OSIrqFunction handler = sIrqTable[IrqIndex(bit)];
    if (REG_IME && (REG_IE & bit) && handler != NULL) {
        handler();
    } else {
        /* No handler: still let OS_WaitIrq see the interrupt happen. */
        INTR_CHECK |= bit;
    }
}

/* Frames ended so far (for the deterministic clock). */
static u32 sFramesEnded;
static BOOL sDeterministic;

void Ntr_SetDeterministicTime(BOOL enable)
{
    sDeterministic = enable;
}

BOOL Ntr_DeterministicTime(void)
{
    return sDeterministic;
}

u32 Ntr_FramesEnded(void)
{
    return sFramesEnded;
}

static void EndFrame(void)
{
    sFramesEnded++;
    Ntr_SoundPoll();
    NtrHost_VBlank();
    Ntr_EndFrameDma();
    RaiseIrq(OS_IE_V_BLANK);
}

void OS_WaitIrq(BOOL clear, OSIrqMask irqFlags)
{
    if (clear) {
        INTR_CHECK &= ~irqFlags;
    }
    /* VBlank is the only interrupt source the port generates. */
    while (!(INTR_CHECK & irqFlags)) {
        EndFrame();
        if (!(irqFlags & OS_IE_V_BLANK)) {
            INTR_CHECK |= irqFlags;
        }
    }
}

/* A VBlank interrupt without a frame: what the DS does when a frame runs long
 * (the handler runs, nothing is presented). */
void Ntr_RaiseVBlankIrq(void)
{
    RaiseIrq(OS_IE_V_BLANK);
}

void OS_WaitVBlankIntr(void)
{
    OS_WaitIrq(TRUE, OS_IE_V_BLANK);
}

void OS_WaitAnyIrq(void)
{
    EndFrame();
}

/* ------------------------------------------------------------------ ticks */

static u64 HostMicroseconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000u + (u64)ts.tv_nsec / 1000u;
}

static u64 sTickBase;

void OS_InitTick(void)
{
    if (sTickBase == 0) {
        sTickBase = HostMicroseconds();
    }
}

BOOL OS_IsTickAvailable(void)
{
    return TRUE;
}

OSTick OS_GetTick(void)
{
    /* The DS tick runs at the system clock / 64; deterministic time advances
     * one 60th of a second per frame. */
    if (sDeterministic) {
        return (u64)sFramesEnded * (HW_SYSTEM_CLOCK / 64) / 60;
    }
    return (HostMicroseconds() - sTickBase) * (HW_SYSTEM_CLOCK / 64) / 1000000u;
}

u16 OS_GetTickLo(void)
{
    return (u16)OS_GetTick();
}

void OS_InitAlarm(void) {}

/* Code waits like this for the other CPU (the sound driver), so the driver
 * gets the chance to catch up. */
void OS_SpinWait(u32 cycle)
{
    (void)cycle;
    Ntr_SoundWait();
}

BOOL OS_IsRunOnEmulator(void)
{
    return FALSE;
}

/* --------------------------------------------------------- message queues */

/* Without threads, nothing can wait for a queue: a blocking send to a full
 * queue or receive from an empty one fails as a non-blocking one does. */
void OS_InitMessageQueue(OSMessageQueue *mq, OSMessage *msgArray, s32 msgCount)
{
    memset(mq, 0, sizeof(*mq));
    mq->msgArray = msgArray;
    mq->msgCount = msgCount;
}

BOOL OS_SendMessage(OSMessageQueue *mq, OSMessage msg, s32 flags)
{
    (void)flags;
    if (mq->usedCount >= mq->msgCount) {
        return FALSE;
    }
    mq->msgArray[(mq->firstIndex + mq->usedCount) % mq->msgCount] = msg;
    mq->usedCount++;
    return TRUE;
}

BOOL OS_ReceiveMessage(OSMessageQueue *mq, OSMessage *msg, s32 flags)
{
    (void)flags;
    if (mq->usedCount == 0) {
        return FALSE;
    }
    if (msg != NULL) {
        *msg = mq->msgArray[mq->firstIndex];
    }
    mq->firstIndex = (mq->firstIndex + 1) % mq->msgCount;
    mq->usedCount--;
    return TRUE;
}

void OS_Sleep(u32 msec)
{
    (void)msec;
}

void OS_GetLowEntropyData(u32 buffer[8])
{
    u64 t = sDeterministic ? 12345 : HostMicroseconds();
    for (int i = 0; i < 8; i++) {
        t = t * 6364136223846793005ull + 1442695040888963407ull;
        buffer[i] = (u32)(t >> 32);
    }
}

/* ------------------------------------------------------------ arenas, heaps */

/* Arena allocations are never freed by the game; the host heap backs them. */
void *OS_AllocFromArenaLo(OSArenaId id, u32 size, u32 align)
{
    (void)id;
    if (align < sizeof(void *)) {
        align = sizeof(void *);
    }
    void *p = aligned_alloc(align, (size + align - 1) / align * align);
    if (p != NULL) {
        memset(p, 0, size);
    }
    return p;
}

void *OS_AllocFromArenaHi(OSArenaId id, u32 size, u32 align)
{
    return OS_AllocFromArenaLo(id, size, align);
}

/* Only the wireless code creates OS heaps; it gets a fixed host buffer. */
static u8 sArena[0x40000] ATTRIBUTE_ALIGN(32);

void *OS_GetArenaLo(OSArenaId id) { (void)id; return sArena; }
void *OS_GetArenaHi(OSArenaId id) { (void)id; return sArena + sizeof(sArena); }
void *OS_GetInitArenaLo(OSArenaId id) { return OS_GetArenaLo(id); }
void *OS_GetInitArenaHi(OSArenaId id) { return OS_GetArenaHi(id); }
void OS_SetArenaLo(OSArenaId id, void *newLo) { (void)id; (void)newLo; }
void OS_SetArenaHi(OSArenaId id, void *newHi) { (void)id; (void)newHi; }

void *OS_InitAlloc(OSArenaId id, void *arenaStart, void *arenaEnd, int maxHeaps)
{
    (void)id;
    (void)arenaEnd;
    (void)maxHeaps;
    return arenaStart;
}

void OS_ClearAlloc(OSArenaId id) { (void)id; }

OSHeapHandle OS_CreateHeap(OSArenaId id, void *start, void *end)
{
    (void)id;
    (void)start;
    (void)end;
    return 0;
}

OSHeapHandle OS_SetCurrentHeap(OSArenaId id, OSHeapHandle heap)
{
    (void)id;
    (void)heap;
    return 0;
}

/* ---------------------------------------------------- threads, locks, misc */

/* Only the wireless and voice chat code starts threads; they never run. */
void OS_CreateThread(OSThread *thread, void (*func)(void *), void *arg, void *stack, u32 stackSize, u32 prio)
{
    (void)thread;
    (void)func;
    (void)arg;
    (void)stack;
    (void)stackSize;
    (void)prio;
}

void OS_WakeupThreadDirect(OSThread *thread) { (void)thread; }
void OS_InitMutex(OSMutex *mutex) { (void)mutex; }
void OS_LockMutex(OSMutex *mutex) { (void)mutex; }
void OS_UnlockMutex(OSMutex *mutex) { (void)mutex; }

static u32 sLockIDs;

s32 OS_GetLockID(void)
{
    for (int i = 0; i < 32; i++) {
        if (!(sLockIDs & (1u << i))) {
            sLockIDs |= 1u << i;
            return OS_MAINP_LOCK_ID_START + i;
        }
    }
    return OS_LOCK_ID_ERROR;
}

void OS_ReleaseLockID(u16 lockID)
{
    int i = lockID - OS_MAINP_LOCK_ID_START;
    if (i >= 0 && i < 32) {
        sLockIDs &= ~(1u << i);
    }
}

OSProcMode OS_GetProcMode(void)
{
    return OS_PROCMODE_SYS;
}

/* Soft reset: the front end restarts or quits the game. */
void OS_ResetSystem(u32 parameter)
{
    NtrHost_Reset(parameter);
}

void OS_GetMacAddress(u8 *macAddress)
{
    static const u8 mac[6] = { 0x00, 0x09, 0xbf, 0x12, 0x34, 0x56 };
    memcpy(macAddress, mac, sizeof(mac));
}

/* The DS user settings: an English-language console with no nickname. */
void OS_GetOwnerInfo(OSOwnerInfo *info)
{
    memset(info, 0, sizeof(*info));
    info->language = OS_LANGUAGE_ENGLISH;
    info->birthday.month = 1;
    info->birthday.day = 1;
}

s64 OS_GetOwnerRtcOffset(void)
{
    return 0;
}

/* -------------------------------------------------------------- power */

u32 PM_GetBackLight(PMBackLightSwitch *top, PMBackLightSwitch *bottom)
{
    if (top != NULL) {
        *top = PM_BACKLIGHT_ON;
    }
    if (bottom != NULL) {
        *bottom = PM_BACKLIGHT_ON;
    }
    return PM_RESULT_SUCCESS;
}

u32 PM_SetBackLight(PMLCDTarget target, PMBackLightSwitch status)
{
    (void)target;
    (void)status;
    return PM_RESULT_SUCCESS;
}

void PM_GoSleepMode(PMWakeUpTrigger trigger, PMLogic logic, u16 keyPattern)
{
    (void)trigger;
    (void)logic;
    (void)keyPattern;
}

u32 PM_ForceToPowerOff(void)
{
    NtrHost_Reset(0);
    return PM_RESULT_SUCCESS;
}

/* ------------------------------------------------------- cache, BIOS calls */

void DC_WaitWriteBufferEmpty(void) {}

u16 SVC_GetCRC16(u32 start, const void *datap, u32 size)
{
    static const u16 table[16] = { 0x0000, 0xCC01, 0xD801, 0x1400, 0xF001, 0x3C00, 0x2800, 0xE401,
                                   0xA001, 0x6C00, 0x7800, 0xB401, 0x5000, 0x9C01, 0x8801, 0x4400 };
    const u8 *p = datap;
    u16 crc = (u16)start;
    for (u32 i = 0; i < size; i++) {
        crc = (u16)((crc >> 4) ^ table[crc & 0xf] ^ table[p[i] & 0xf]);
        crc = (u16)((crc >> 4) ^ table[crc & 0xf] ^ table[p[i] >> 4]);
    }
    return crc;
}

u16 SVC_Sqrt(u32 src)
{
    u32 r = 0;
    for (u32 bit = 1u << 30; bit != 0; bit >>= 2) {
        if (src >= r + bit) {
            src -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
    }
    return (u16)r;
}

/* ------------------------------------------------------------------- DMA */

/* DMA copies are done immediately, so there is never anything to wait for. */
void MI_WaitDma(u32 dmaNo) { (void)dmaNo; }

void MI_DmaFill32(u32 dmaNo, void *dest, u32 data, u32 size)
{
    (void)dmaNo;
    MIi_CpuClear32(data, dest, size);
}

/* HBlank DMA copies one unit to a register after each scanline; the port
 * renders whole frames, so the transfer stays armed and the first unit is
 * applied at the end of each frame. */
static struct {
    const void *src;
    void *dest;
    u32 size;
} sHBlankDma[4];

void MI_HBlankDmaCopy32(u32 dmaNo, const void *src, void *dest, u32 size)
{
    sHBlankDma[dmaNo & 3].src = src;
    sHBlankDma[dmaNo & 3].dest = dest;
    sHBlankDma[dmaNo & 3].size = size;
}

void MI_HBlankDmaCopy16(u32 dmaNo, const void *src, void *dest, u32 size)
{
    MI_HBlankDmaCopy32(dmaNo, src, dest, size);
}

void MI_StopDma(u32 dmaNo)
{
    sHBlankDma[dmaNo & 3].src = NULL;
}

void Ntr_EndFrameDma(void)
{
    for (int i = 0; i < 4; i++) {
        if (sHBlankDma[i].src != NULL) {
            memcpy(sHBlankDma[i].dest, sHBlankDma[i].src, sHBlankDma[i].size);
        }
    }
}
