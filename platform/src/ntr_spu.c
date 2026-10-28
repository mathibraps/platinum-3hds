/*
 * The DS sound hardware, as the ARM7 sound driver programs it through its
 * registers (gNtrSnd7Regs, DS addresses 0x04000000 + offset):
 *   - 16 channels playing PCM8, PCM16 or IMA-ADPCM samples from memory,
 *     square waves (channels 8-13) or noise (14, 15), each at its own rate
 *     (a timer on the 16.76 MHz sound clock), with volume, shift and pan;
 *   - the mixer and its output selection, and the master volume;
 *   - the two capture units, which record the mix (left, right) into memory
 *     while channels 1 and 3 play it back: the game's reverb.
 * Output is 16-bit stereo at the DS rate (one sample every 512 sound clocks,
 * 32728.5 Hz), each channel's current sample without interpolation, as the
 * hardware. Formulas follow the hardware's integer mixing, without the DAC's
 * reduction to 10 bits.
 *
 * Runs on the ARM7 side only (ntr_snd.c), between the driver's steps, so the
 * registers do not change while a batch of samples is mixed.
 */
#include <string.h>

#include "ntr_shim.h"

u8 gNtrSnd7Regs[NTR_SND7_REGS_SIZE] ATTRIBUTE_ALIGN(32);

#define CH_CNT(ch) (*(volatile u32 *)&gNtrSnd7Regs[0x400 + 16 * (ch)])
#define CH_SAD(ch) (*(volatile u32 *)&gNtrSnd7Regs[0x404 + 16 * (ch)])
#define CH_TMR(ch) (*(volatile u16 *)&gNtrSnd7Regs[0x408 + 16 * (ch)])
#define CH_PNT(ch) (*(volatile u16 *)&gNtrSnd7Regs[0x40A + 16 * (ch)])
#define CH_LEN(ch) (*(volatile u32 *)&gNtrSnd7Regs[0x40C + 16 * (ch)])
#define SOUNDCNT_VOL (*(volatile u8 *)&gNtrSnd7Regs[0x500])
#define SOUNDCNT_8 (*(volatile u8 *)&gNtrSnd7Regs[0x501])
#define CAP_CNT(cap) (*(volatile u8 *)&gNtrSnd7Regs[0x508 + (cap)])
#define CAP_DAD(cap) (*(volatile u32 *)&gNtrSnd7Regs[0x510 + 8 * (cap)])
#define CAP_LEN(cap) (*(volatile u16 *)&gNtrSnd7Regs[0x514 + 8 * (cap)])

#define CNT_START 0x80000000u
#define CNT_HOLD 0x8000u
#define CAP_START 0x80

/* Sound clocks per output sample. */
#define CLOCKS_PER_SAMPLE 512

enum { FMT_PCM8, FMT_PCM16, FMT_ADPCM, FMT_PSG };
enum { REPEAT_MANUAL, REPEAT_LOOP, REPEAT_ONESHOT };

typedef struct {
    const u8 *data;
    u32 acc; /* sound clocks toward the next sample */
    u32 pos; /* next sample (PCM), nibble (ADPCM) or waveform step (PSG) */
    u32 loop, end; /* same units */
    u32 delay; /* timer periods before the first sample */
    s32 cur; /* the sample the channel outputs */
    s32 adpcm, adpcmLoop;
    int index, indexLoop;
    u32 noise;
    u8 format, repeat, duty;
} Channel;

typedef struct {
    u8 *dest;
    u32 acc, size; /* size in bytes */
    u32 taken; /* samples captured since the start */
    s16 fifo[8]; /* the last CAPTURE_LAG samples, not yet in memory */
    BOOL on;
} Capture;

static Channel sCh[16];
static u32 sActive; /* channels running, one bit each */
static Capture sCap[2];

static const s16 sAdpcmStep[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
    5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767,
};
static const s8 sAdpcmIndex[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

/* Per step index and nibble: the signed change and the next index. */
static s32 sAdpcmDiff[89][16];
static u8 sAdpcmNext[89][16];

static void InitAdpcmTables(void)
{
    for (int index = 0; index < 89; index++) {
        for (int n = 0; n < 16; n++) {
            int step = sAdpcmStep[index];
            int diff = step >> 3;
            if (n & 1) {
                diff += step >> 2;
            }
            if (n & 2) {
                diff += step >> 1;
            }
            if (n & 4) {
                diff += step;
            }
            sAdpcmDiff[index][n] = (n & 8) ? -diff : diff;
            int next = index + sAdpcmIndex[n & 7];
            sAdpcmNext[index][n] = (u8)(next < 0 ? 0 : next > 88 ? 88 : next);
        }
    }
}

void Ntr_SpuReset(void)
{
    InitAdpcmTables();
    memset(sCh, 0, sizeof(sCh));
    memset(sCap, 0, sizeof(sCap));
    sActive = 0;
}

static void Stop(int ch)
{
    CH_CNT(ch) &= ~CNT_START;
    sActive &= ~(1u << ch);
    if (!(CH_CNT(ch) & CNT_HOLD)) {
        sCh[ch].cur = 0;
    }
}

void Ntr_SpuStartChannel(int ch)
{
    Channel *c = &sCh[ch];
    u32 cnt = CH_CNT(ch) | CNT_START;
    CH_CNT(ch) = cnt;

    memset(c, 0, sizeof(*c));
    c->format = (u8)((cnt >> 29) & 3);
    c->repeat = (u8)((cnt >> 27) & 3);
    c->duty = (u8)((cnt >> 24) & 7);
    c->data = (const u8 *)(uintptr_t)CH_SAD(ch);
    u32 pnt = CH_PNT(ch), len = CH_LEN(ch) & 0x3fffff;
    /* PCM and ADPCM start three timer periods late, square waves and noise
     * one (as measured on hardware by emulator authors). */
    c->delay = 3;
    switch (c->format) {
    case FMT_PCM8:
        c->loop = pnt * 4;
        c->end = (pnt + len) * 4;
        break;
    case FMT_PCM16:
        c->loop = pnt * 2;
        c->end = (pnt + len) * 2;
        break;
    case FMT_ADPCM:
        /* A 32-bit header (first sample, step index), then 4-bit samples;
         * the loop start counts the header's word. The decoder's state at
         * the loop start is restored at each loop. */
        if (c->data != NULL) {
            u32 header = c->data[0] | c->data[1] << 8 | c->data[2] << 16 | (u32)c->data[3] << 24;
            c->adpcm = (s16)header;
            c->index = (header >> 16) & 0x7f;
            if (c->index > 88) {
                c->index = 88;
            }
        }
        c->adpcmLoop = c->adpcm;
        c->indexLoop = c->index;
        c->pos = 8;
        c->loop = pnt < 1 ? 8 : pnt * 8;
        c->end = (pnt + len) * 8;
        break;
    default:
        c->delay = 1;
        c->noise = 0x7fff;
        break;
    }
    if (c->format != FMT_PSG && (c->data == NULL || c->end <= c->pos)) {
        Stop(ch);
        return;
    }
    sActive |= 1u << ch;
}

void Ntr_SpuStartCapture(int mask)
{
    for (int cap = 0; cap < 2; cap++) {
        if (!(mask & (1 << cap))) {
            continue;
        }
        Capture *k = &sCap[cap];
        CAP_CNT(cap) |= CAP_START;
        k->dest = (u8 *)(uintptr_t)CAP_DAD(cap);
        k->size = (CAP_LEN(cap) ? CAP_LEN(cap) : 1) * 4u;
        k->acc = 0;
        k->taken = 0;
        k->on = k->dest != NULL;
    }
}

/* At the end of the sample data: loops (manual mode loops as well), or a
 * one-shot sound ends. FALSE when the channel stopped. */
static BOOL Wrap(Channel *c, int ch)
{
    if (c->repeat == REPEAT_ONESHOT || c->loop >= c->end) {
        Stop(ch);
        return FALSE;
    }
    c->pos = c->loop;
    if (c->format == FMT_ADPCM) {
        c->adpcm = c->adpcmLoop;
        c->index = c->indexLoop;
    }
    return TRUE;
}

/* One timer period: the channel moves to its next sample. FALSE when it
 * stopped. `format` is a constant where this is inlined, so each format gets
 * its own loop (RenderChannel). */
static inline __attribute__((always_inline)) BOOL Advance(Channel *c, int ch, int format)
{
    if (__builtin_expect(c->delay != 0, 0)) {
        c->delay--;
        return TRUE;
    }
    switch (format) {
    case FMT_PCM8:
        if (c->pos >= c->end && !Wrap(c, ch)) {
            return FALSE;
        }
        c->cur = (s8)c->data[c->pos++] << 8;
        break;
    case FMT_PCM16:
        if (c->pos >= c->end && !Wrap(c, ch)) {
            return FALSE;
        }
        c->cur = (s16)(c->data[c->pos * 2] | c->data[c->pos * 2 + 1] << 8);
        c->pos++;
        break;
    case FMT_ADPCM: {
        if (c->pos >= c->end && !Wrap(c, ch)) {
            return FALSE;
        }
        if (c->pos == c->loop) {
            c->adpcmLoop = c->adpcm;
            c->indexLoop = c->index;
        }
        int n = (c->data[c->pos >> 1] >> ((c->pos & 1) * 4)) & 0xf;
        s32 v = c->adpcm + sAdpcmDiff[c->index][n];
        c->adpcm = v < -0x7fff ? -0x7fff : v > 0x7fff ? 0x7fff : v;
        c->index = sAdpcmNext[c->index][n];
        c->pos++;
        c->cur = c->adpcm;
        break;
    }
    default:
        if (ch >= 14) {
            if (c->noise & 1) {
                c->noise = (c->noise >> 1) ^ 0x6000;
                c->cur = -0x7fff;
            } else {
                c->noise >>= 1;
                c->cur = 0x7fff;
            }
        } else if (ch >= 8) {
            /* Duty d: the last d + 1 of 8 steps are high (7: always low). */
            c->pos = (c->pos + 1) & 7;
            c->cur = (c->duty < 7 && c->pos >= 7u - c->duty) ? 0x7fff : -0x7fff;
        } else {
            c->cur = 0; /* no square wave or noise on channels 0-7 */
        }
        break;
    }
    return TRUE;
}

static s32 Clamp16(s32 v)
{
    return v < -0x8000 ? -0x8000 : v > 0x7fff ? 0x7fff : v;
}

/* Adds n samples of a channel to mix (interleaved left, right): its current
 * sample times volume and pan, with the data shift (1, 1/2, 1/4, 1/16), so
 * that a 16-bit sample at full volume, panned hard, mixes at 2^8 times its
 * value. */
static inline __attribute__((always_inline)) void Run(Channel *c, int ch, int format, int n, s32 *restrict mix)
{
    static const u8 kShift[4] = { 4, 3, 2, 0 };
    u32 cnt = CH_CNT(ch);
    u32 period = 0x10000u - CH_TMR(ch);
    s32 vol = cnt & 0x7f, pan = (cnt >> 16) & 0x7f;
    vol += vol == 127;
    pan += pan == 127;
    s32 gainL = vol * (128 - pan), gainR = vol * pan;
    int shift = 10 - kShift[(cnt >> 8) & 3];
    u32 acc = c->acc;
    s32 l = (c->cur * gainL) >> shift, r = (c->cur * gainR) >> shift;
    s32 *end = mix + 2 * n;
    for (; mix < end; mix += 2) {
        acc += CLOCKS_PER_SAMPLE;
        if (acc >= period) {
            BOOL stopped = FALSE;
            do {
                acc -= period;
                if (!Advance(c, ch, format)) {
                    acc = 0;
                    stopped = TRUE;
                    break;
                }
            } while (acc >= period);
            l = (c->cur * gainL) >> shift;
            r = (c->cur * gainR) >> shift;
            if (stopped) {
                /* Stopped: a held last sample (or silence) to the end. */
                for (; mix < end; mix += 2) {
                    mix[0] += l;
                    mix[1] += r;
                }
                break;
            }
        }
        mix[0] += l;
        mix[1] += r;
    }
    c->acc = acc;
}

static void RenderChannel(int ch, int n, s32 *mix)
{
    Channel *c = &sCh[ch];
    switch (c->format) {
    case FMT_PCM8:
        Run(c, ch, FMT_PCM8, n, mix);
        break;
    case FMT_PCM16:
        Run(c, ch, FMT_PCM16, n, mix);
        break;
    case FMT_ADPCM:
        Run(c, ch, FMT_ADPCM, n, mix);
        break;
    default:
        Run(c, ch, FMT_PSG, n, mix);
        break;
    }
}

/* Capture writes reach memory CAPTURE_LAG samples after they are taken (the
 * hardware's capture FIFO), and channels play captured data from before that
 * (their read FIFO): channels 1 and 3, playing the capture buffers back, hear
 * the mix a whole buffer later, the game's reverb. */
#define CAPTURE_LAG 8

static void CaptureWrite(Capture *k, BOOL pcm8, u32 slot, s32 v)
{
    if (pcm8) {
        k->dest[slot] = (u8)(v >> 8);
    } else {
        k->dest[slot * 2] = (u8)v;
        k->dest[slot * 2 + 1] = (u8)(v >> 8);
    }
}

static void RunCapture(int cap, int n, const s32 *src, int stride)
{
    Capture *k = &sCap[cap];
    u8 cnt = CAP_CNT(cap);
    BOOL pcm8 = (cnt & 8) != 0;
    u32 slots = pcm8 ? k->size : k->size / 2;
    u32 period = 0x10000u - CH_TMR(cap ? 3 : 1);
    for (int i = 0; i < n; i++) {
        k->acc += CLOCKS_PER_SAMPLE;
        while (k->acc >= period) {
            k->acc -= period;
            s32 v = Clamp16(src[i * stride] >> 8);
            u32 slot = k->taken % slots;
            if (k->taken >= CAPTURE_LAG) {
                CaptureWrite(k, pcm8, (k->taken - CAPTURE_LAG) % slots, k->fifo[k->taken % CAPTURE_LAG]);
            }
            k->fifo[k->taken % CAPTURE_LAG] = (s16)v;
            k->taken++;
            if ((cnt & 4) && slot == slots - 1) { /* one-shot: ends with the buffer */
                for (u32 t = k->taken > CAPTURE_LAG ? k->taken - CAPTURE_LAG : 0; t < k->taken; t++) {
                    CaptureWrite(k, pcm8, t % slots, k->fifo[t % CAPTURE_LAG]);
                }
                CAP_CNT(cap) = cnt & ~CAP_START;
                k->on = FALSE;
                return;
            }
        }
    }
}

#define BATCH 128

static void MixBatch(s16 *out, int n)
{
    /* Interleaved left, right: the mix, and channels 0-3 on their own (they
     * also feed capture and the output selection). */
    static s32 mix[2 * BATCH], low[4][2 * BATCH];
    memset(mix, 0, sizeof(s32) * 2 * n);
    u8 ctl = SOUNDCNT_8;
    BOOL ch1Mixed = !(ctl & 0x10), ch3Mixed = !(ctl & 0x20);
    u32 lowUsed = 0;
    for (u32 m = sActive; m != 0; m &= m - 1) {
        int ch = __builtin_ctz(m);
        if (!(CH_CNT(ch) & CNT_START)) { /* stopped by the driver */
            sActive &= ~(1u << ch);
            sCh[ch].cur = 0;
            continue;
        }
        if (ch >= 4) {
            RenderChannel(ch, n, mix);
            continue;
        }
        memset(low[ch], 0, sizeof(s32) * 2 * n);
        lowUsed |= 1u << ch;
        RenderChannel(ch, n, low[ch]);
        if ((ch == 1 && !ch1Mixed) || (ch == 3 && !ch3Mixed)) {
            continue;
        }
        for (int i = 0; i < 2 * n; i++) {
            mix[i] += low[ch][i];
        }
    }
    for (int ch = 0; ch < 4; ch++) {
        if (!(lowUsed & (1u << ch))) {
            memset(low[ch], 0, sizeof(s32) * 2 * n);
        }
    }

    /* Capture records the mix (or channel 0 / 2) before the master volume,
     * at the rate of channel 1 / 3's timer. */
    for (int cap = 0; cap < 2; cap++) {
        if (!sCap[cap].on) {
            continue;
        }
        if (!(CAP_CNT(cap) & CAP_START)) {
            sCap[cap].on = FALSE;
            continue;
        }
        if (CAP_CNT(cap) & 2) {
            static s32 chan[BATCH];
            for (int i = 0; i < n; i++) {
                chan[i] = low[cap * 2][2 * i] + low[cap * 2][2 * i + 1];
            }
            RunCapture(cap, n, chan, 1);
        } else {
            RunCapture(cap, n, mix + cap, 2);
        }
    }

    s32 master = SOUNDCNT_VOL & 0x7f;
    master += master == 127;
    if (!(ctl & 0x80)) {
        master = 0;
    }
    /* The output selection: each side from the mix, channel 1, 3, or both. */
    int selL = ctl & 3, selR = (ctl >> 2) & 3;
    const s32 *l0 = selL == 0 ? mix : selL == 2 ? low[3] : low[1];
    const s32 *r0 = selR == 0 ? mix : selR == 2 ? low[3] : low[1];
    BOOL l3 = selL == 3, r3 = selR == 3;
    for (int i = 0; i < n; i++) {
        s32 l = l0[2 * i] + (l3 ? low[3][2 * i] : 0);
        s32 r = r0[2 * i + 1] + (r3 ? low[3][2 * i + 1] : 0);
        out[2 * i] = (s16)Clamp16((s32)(((s64)l * master) >> 15));
        out[2 * i + 1] = (s16)Clamp16((s32)(((s64)r * master) >> 15));
    }
}

/* Time spent mixing (NtrHost_Ticks units), for profiling. */
u64 gNtrSpuTicks;

u32 gNtrDebugSkip;

void Ntr_SpuMix(s16 *out, int frames)
{
    if (gNtrDebugSkip & NTR_SKIP_MIX) {
        memset(out, 0, (size_t)frames * 4);
        return;
    }
    u64 start = NtrHost_Ticks();
    while (frames > 0) {
        int n = frames < BATCH ? frames : BATCH;
        MixBatch(out, n);
        out += 2 * n;
        frames -= n;
    }
    gNtrSpuTicks += NtrHost_Ticks() - start;
}

#ifdef NTR_DEBUG_HOOKS
#include <stdio.h>

/* Debugging (desktop harness, PORT_SND): the channels and captures. */
void Ntr_SpuDebugPrint(void)
{
    printf("  soundcnt %02x vol %d cap %02x %02x\n", SOUNDCNT_8, SOUNDCNT_VOL, CAP_CNT(0), CAP_CNT(1));
    for (int ch = 0; ch < 16; ch++) {
        if (!(sActive & (1u << ch))) {
            continue;
        }
        const Channel *c = &sCh[ch];
        u32 cnt = CH_CNT(ch);
        printf("  ch%-2d fmt %d rep %d vol %3d sh %d pan %3d tmr %04x pos %6u/%6u loop %6u cur %6d\n", ch, c->format, c->repeat,
            cnt & 0x7f, (cnt >> 8) & 3, (cnt >> 16) & 0x7f, CH_TMR(ch), c->pos, c->end, c->loop, c->cur);
    }
}
#endif
