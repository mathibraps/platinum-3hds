/*
 * Prefix for the port's build of NitroSDK's ARM7 sound driver (SND7_SRCS in
 * sources.mk): the sequencer, envelopes and channel allocation that ran on the
 * DS's second CPU. The game's prefix header (ARM9 view of the SDK) comes
 * first; the build passes NTR_SND7, which leaves the ARM9 view's sound
 * prototypes out of it. Then:
 *   - the sources see SDK_ARM7 instead of SDK_ARM9;
 *   - every symbol they define gets an ARM7_ prefix (ntr_snd7_names.h,
 *     listed from a first compile), so the halves of the sources the ARM9
 *     and the ARM7 share link side by side;
 *   - the ARM7 sound registers are the port's ARM7 register block, which the
 *     mixer reads (ntr_spu.c);
 *   - the ARM7 view of the sound headers declares the driver's functions;
 *   - the OS services the driver uses on the ARM7 (PXI to the ARM9, the
 *     ARM7 clock and alarms) are the port's ARM7 versions (ntr_snd.c).
 * Message queues and the other OS calls are the same on both CPUs.
 */
#ifndef NTR_SND7_H
#define NTR_SND7_H

#undef SDK_ARM9
#define SDK_ARM7 1

#ifndef NTR_SND7_RAW
#include "ntr_snd7_names.h"
#endif

#undef HW_REG_BASE
#define HW_REG_BASE ((u32)gNtrSnd7Regs)

#define PXI_SetFifoRecvCallback ARM7_PXI_SetFifoRecvCallback
#define PXI_SendWordByFifo ARM7_PXI_SendWordByFifo
#define OS_GetTick ARM7_OS_GetTick
#define OS_CreateAlarm ARM7_OS_CreateAlarm
#define OS_SetAlarm ARM7_OS_SetAlarm
#define OS_SetPeriodicAlarm ARM7_OS_SetPeriodicAlarm
#define OS_CancelAlarm ARM7_OS_CancelAlarm
void PXI_SetFifoRecvCallback(int fifotag, PXIFifoCallback callback);
int PXI_SendWordByFifo(int fifotag, u32 data, BOOL err);
OSTick OS_GetTick(void);
void OS_CreateAlarm(OSAlarm *alarm);
void OS_SetAlarm(OSAlarm *alarm, OSTick tick, OSAlarmHandler handler, void *arg);
void OS_SetPeriodicAlarm(OSAlarm *alarm, OSTick start, OSTick period, OSAlarmHandler handler, void *arg);
void OS_CancelAlarm(OSAlarm *alarm);
/* ARM7-only services the driver's sleep functions call (unused here). */
void SVC_SetSoundBias(s32 stepLoops);
void SVC_ResetSoundBias(s32 stepLoops);
void PMi_ResetControl(u8 ctrl);
void PMi_SetControl(u8 ctrl);

/* Starting a channel or a capture restarts it on the sound hardware, even
 * when it is running: the SDK's inline helpers only set the start bit, which
 * a register in plain memory cannot tell from a rewrite of it. */
#define SND_StartChannel NtrSnd7_SdkStartChannel
#define SND_StartCapture NtrSnd7_SdkStartCapture
#define SND_StartCaptureBoth NtrSnd7_SdkStartCaptureBoth
#include "ntr_sdk_defs7.h"
#undef SND_StartChannel
#undef SND_StartCapture
#undef SND_StartCaptureBoth
#define SND_StartChannel(chNo) Ntr_SpuStartChannel(chNo)
#define SND_StartCapture(capture) Ntr_SpuStartCapture(1 << (capture))
#define SND_StartCaptureBoth() Ntr_SpuStartCapture(3)

#endif
