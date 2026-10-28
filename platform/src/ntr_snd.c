/*
 * The DS sound system. The game calls NitroSDK's sound interface (built for
 * the ARM9), which sends command lists to the sound driver that ran on the
 * DS's second CPU. Here that driver is NitroSDK's own ARM7 build (its
 * symbols prefixed ARM7_, see ntr_snd7.h): it runs beside the game, talks to
 * it through the same PXI messages and shared memory, and plays through the
 * sound hardware in ntr_spu.c.
 *
 * Time on the ARM7 side is the sound output: the driver's 5.2 ms steps and
 * its alarms happen at their place among the samples the front end asks for
 * (16 ARM7 timer ticks per sample). The front end either asks from an audio
 * thread (the 3DS: the driver then runs on that thread only, and the game's
 * messages wait in a queue for it) or from the game's thread, once a frame
 * (the desktop harness and tests: then the driver takes messages the moment
 * they are sent, like an ARM7 infinitely fast, and runs are repeatable).
 */
#include <string.h>

#include "ntr_shim.h"

/* The driver (NitroSDK's ARM7 sound library). */
void ARM7_SND_CommandInit(void);
void ARM7_SND_ExChannelInit(void);
void ARM7_SND_SeqInit(void);
void ARM7_SND_AlarmInit(void);
void ARM7_SND_Enable(void);
void ARM7_SND_SetOutputSelector(SNDOutput left, SNDOutput right, SNDChannelOut channel1, SNDChannelOut channel3);
void ARM7_SND_SetMasterVolume(int volume);
void ARM7_SND_UpdateExChannel(void);
void ARM7_SND_CommandProc(void);
void ARM7_SND_SeqMain(BOOL doPeriodicProc);
void ARM7_SND_ExChannelMain(BOOL doPeriodicProc);
void ARM7_SND_UpdateSharedWork(void);
u16 ARM7_SND_CalcRandom(void);

/* ARM7 timer ticks (33.51 MHz / 64) per output sample (512 sound clocks of
 * 16.76 MHz). */
#define TICKS_PER_SAMPLE 16
/* The driver's first step comes this long after it starts (snd_main.c). */
#define FIRST_STEP 0x10000

/* PXI: a queue each way, one writer and one reader each. */
#define QUEUE_SIZE 64

typedef struct {
    u32 data[QUEUE_SIZE];
    u32 head, tail;
} Queue;

static BOOL Push(Queue *q, u32 v)
{
    u32 head = __atomic_load_n(&q->head, __ATOMIC_RELAXED);
    if (head - __atomic_load_n(&q->tail, __ATOMIC_ACQUIRE) >= QUEUE_SIZE) {
        return FALSE;
    }
    q->data[head % QUEUE_SIZE] = v;
    __atomic_store_n(&q->head, head + 1, __ATOMIC_RELEASE);
    return TRUE;
}

static BOOL Pop(Queue *q, u32 *v)
{
    u32 tail = __atomic_load_n(&q->tail, __ATOMIC_RELAXED);
    if (tail == __atomic_load_n(&q->head, __ATOMIC_ACQUIRE)) {
        return FALSE;
    }
    *v = q->data[tail % QUEUE_SIZE];
    __atomic_store_n(&q->tail, tail + 1, __ATOMIC_RELEASE);
    return TRUE;
}

static Queue sToArm7, sToArm9;
static PXIFifoCallback sArm9Callback, sArm7Callback;
static BOOL sArm7Ready;
static BOOL sThreaded;
static void (*sWake)(void);
static void (*sIdle)(void);

/* ARM7 side: its clock, the driver's next step and the alarms it set. */
static u64 sNow;
static u64 sNextStep;
#define MAX_ALARMS 16
static OSAlarm *sAlarms[MAX_ALARMS];
static int sNumAlarms;

/* One pass of the driver thread (SndThread in snd_main.c): a periodic step
 * advances sequences and envelopes; a wake-up only takes commands. */
static void DriverStep(BOOL periodic)
{
    ARM7_SND_UpdateExChannel();
    ARM7_SND_CommandProc();
    ARM7_SND_SeqMain(periodic);
    ARM7_SND_ExChannelMain(periodic);
    ARM7_SND_UpdateSharedWork();
    (void)ARM7_SND_CalcRandom();
}

/* The ARM7 takes the game's messages: command lists go to the driver's
 * queue (its PXI interrupt), a wake-up request runs the driver at once. */
static void Arm7Receive(void)
{
    u32 data;
    BOOL wake = FALSE;
    while (Pop(&sToArm7, &data)) {
        if (data == 0) { /* SND_MSG_REQUEST_COMMAND_PROC */
            wake = TRUE;
        } else if (sArm7Callback != NULL) {
            sArm7Callback(PXI_FIFO_TAG_SOUND, data, FALSE);
        }
    }
    if (wake) {
        DriverStep(FALSE);
    }
}

void NtrSound_Start(BOOL threaded, void (*wake)(void), void (*idle)(void))
{
    sThreaded = threaded;
    sWake = wake;
    sIdle = idle;
    Ntr_SpuReset();
    sNow = 0;
    sNextStep = FIRST_STEP;
    /* What the ARM7's SND_Init and sound thread do before their loop. */
    ARM7_SND_CommandInit();
    ARM7_SND_ExChannelInit();
    ARM7_SND_SeqInit();
    ARM7_SND_AlarmInit();
    ARM7_SND_Enable();
    ARM7_SND_SetOutputSelector(SND_OUTPUT_MIXER, SND_OUTPUT_MIXER, SND_CHANNEL_OUT_MIXER, SND_CHANNEL_OUT_MIXER);
    ARM7_SND_SetMasterVolume(SND_MASTER_VOLUME_MAX);
    __atomic_store_n(&sArm7Ready, TRUE, __ATOMIC_RELEASE);
}

void NtrSound_Service(void)
{
    Arm7Receive();
}

static void RunAlarms(void)
{
    for (int i = 0; i < sNumAlarms; i++) {
        OSAlarm *a = sAlarms[i];
        if (a->fire > sNow) {
            continue;
        }
        OSAlarmHandler handler = a->handler;
        void *arg = a->arg;
        if (a->period != 0) {
            a->fire += a->period;
        } else {
            sAlarms[i--] = sAlarms[--sNumAlarms];
        }
        if (handler != NULL) {
            handler(arg);
        }
    }
}

void NtrSound_Render(s16 *out, int frames)
{
    while (frames > 0) {
        Arm7Receive();
        if (sNextStep <= sNow) {
            DriverStep(TRUE);
            sNextStep += SND_PROC_INTERVAL;
        }
        RunAlarms();
        u64 next = sNextStep;
        for (int i = 0; i < sNumAlarms; i++) {
            if (sAlarms[i]->fire < next) {
                next = sAlarms[i]->fire;
            }
        }
        u64 n = next > sNow ? (next - sNow + TICKS_PER_SAMPLE - 1) / TICKS_PER_SAMPLE : 1;
        if (n > (u64)frames) {
            n = (u64)frames;
        }
        Ntr_SpuMix(out, (int)n);
        out += 2 * n;
        frames -= (int)n;
        sNow += n * TICKS_PER_SAMPLE;
    }
}

/* ------------------------------------------------- the ARM9 (game) side */

void PXI_SetFifoRecvCallback(int fifotag, PXIFifoCallback callback)
{
    if (fifotag == PXI_FIFO_TAG_SOUND) {
        sArm9Callback = callback;
    }
}

BOOL PXI_IsCallbackReady(int fifotag, PXIProc proc)
{
    if (fifotag != PXI_FIFO_TAG_SOUND || proc != PXI_PROC_ARM7) {
        return FALSE;
    }
    /* A front end that never started the driver (the tests) gets one on the
     * game's thread, so the game does not wait for it forever. */
    if (!__atomic_load_n(&sArm7Ready, __ATOMIC_ACQUIRE)) {
        NtrSound_Start(FALSE, NULL, NULL);
    }
    return TRUE;
}

int PXI_SendWordByFifo(int fifotag, u32 data, BOOL err)
{
    (void)err;
    if (fifotag != PXI_FIFO_TAG_SOUND) {
        return 0; /* nothing listens on the ARM7 for the other subsystems */
    }
    if (!sThreaded) {
        if (!Push(&sToArm7, data)) {
            return -1;
        }
        Arm7Receive();
        return 0;
    }
    BOOL sent = Push(&sToArm7, data);
    sWake();
    return sent ? 0 : -1;
}

/* Delivers the driver's messages (alarms) to the game: the ARM9's PXI
 * interrupt. Called once a frame and while the game waits on the driver. */
void Ntr_SoundPoll(void)
{
    u32 data;
    while (Pop(&sToArm9, &data)) {
        if (sArm9Callback != NULL) {
            sArm9Callback(PXI_FIFO_TAG_SOUND, data, FALSE);
        }
    }
}

/* Time the game spent waiting for the driver (NtrHost_Ticks units). */
u64 gNtrSoundWaitTicks;

static void SoundWait(void);

/* The game is waiting for the driver (OS_SpinWait). */
void Ntr_SoundWait(void)
{
    u64 t0 = NtrHost_Ticks();
    SoundWait();
    gNtrSoundWaitTicks += NtrHost_Ticks() - t0;
}

static void SoundWait(void)
{
    Ntr_SoundPoll();
    if (!__atomic_load_n(&sArm7Ready, __ATOMIC_ACQUIRE)) {
        return;
    }
    if (sThreaded) {
        sWake();
        sIdle();
    } else {
        DriverStep(FALSE);
    }
}

/* ------------------------------------- the ARM7's OS, as the driver uses it */

void ARM7_PXI_SetFifoRecvCallback(int fifotag, PXIFifoCallback callback)
{
    if (fifotag == PXI_FIFO_TAG_SOUND) {
        sArm7Callback = callback;
    }
}

int ARM7_PXI_SendWordByFifo(int fifotag, u32 data, BOOL err)
{
    (void)err;
    /* Never fails: the driver retries forever, and in the single-threaded
     * mode the game cannot run in between to make room. */
    if (fifotag == PXI_FIFO_TAG_SOUND) {
        (void)Push(&sToArm9, data);
    }
    return 0;
}

OSTick ARM7_OS_GetTick(void)
{
    return sNow;
}

static void RemoveAlarm(OSAlarm *alarm)
{
    for (int i = 0; i < sNumAlarms; i++) {
        if (sAlarms[i] == alarm) {
            sAlarms[i] = sAlarms[--sNumAlarms];
            return;
        }
    }
}

static void AddAlarm(OSAlarm *alarm)
{
    RemoveAlarm(alarm);
    if (sNumAlarms < MAX_ALARMS) {
        sAlarms[sNumAlarms++] = alarm;
    }
}

void ARM7_OS_CreateAlarm(OSAlarm *alarm)
{
    RemoveAlarm(alarm);
    alarm->handler = NULL;
    alarm->tag = 0;
}

void ARM7_OS_SetAlarm(OSAlarm *alarm, OSTick tick, OSAlarmHandler handler, void *arg)
{
    alarm->handler = handler;
    alarm->arg = arg;
    alarm->fire = sNow + tick;
    alarm->period = 0;
    alarm->start = 0;
    AddAlarm(alarm);
}

void ARM7_OS_SetPeriodicAlarm(OSAlarm *alarm, OSTick start, OSTick period, OSAlarmHandler handler, void *arg)
{
    alarm->handler = handler;
    alarm->arg = arg;
    alarm->fire = start;
    alarm->start = start;
    alarm->period = period;
    AddAlarm(alarm);
}

void ARM7_OS_CancelAlarm(OSAlarm *alarm)
{
    RemoveAlarm(alarm);
    alarm->handler = NULL;
}

/* The driver's sleep functions (SND_BeginSleep, SND_EndSleep) ramp the
 * speaker bias and power; the port never sleeps the sound hardware. */
void SVC_SetSoundBias(s32 stepLoops) { (void)stepLoops; }
void SVC_ResetSoundBias(s32 stepLoops) { (void)stepLoops; }
void PMi_ResetControl(u8 ctrl) { (void)ctrl; }
void PMi_SetControl(u8 ctrl) { (void)ctrl; }
