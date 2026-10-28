/*
 * Checks the tile-at-a-time text BG renderer against a plain per-pixel
 * reference on real game frames (title screen states, every BG of both
 * engines, all 192 lines, with several scroll offsets), in both line formats
 * (RGB555 for the software compositor, RGBA5551 for GPU composition).
 */
#include <stdio.h>

#include "../platform/src/ntr_render.c"
#include <stdlib.h>

static void ReferenceTextLine(const Eng *e, int bg, BOOL isMain, int y, u16 *out)
{
    u16 cnt = e->bgcnt[bg];
    int size = cnt >> 14;
    int width = (size & 1) ? 512 : 256;
    int height = (size & 2) ? 512 : 256;
    BOOL is256 = (cnt >> 7) & 1;
    u32 scr = ((cnt >> 8) & 0x1f) * 0x800;
    u32 chr = ((cnt >> 2) & 0xf) * 0x4000;
    if (isMain) {
        scr += ((e->dispcnt >> 27) & 7) * 0x10000;
        chr += ((e->dispcnt >> 24) & 7) * 0x10000;
    }
    BOOL extPltt = is256 && (e->dispcnt & (1u << 30));
    int slot = bg;
    if (bg < 2 && (cnt & (1 << 13))) {
        slot += 2;
    }

    int sy = (y + e->bgvofs[bg]) & (height - 1);
    for (int x = 0; x < W; x++) {
        int sx = (x + e->bghofs[bg]) & (width - 1);
        int block = (sx >> 8) + (sy >> 8) * (width >> 8);
        u16 entry = VramHalf(e, scr + block * 0x800 + ((sy & 255) >> 3) * 64 + ((sx & 255) >> 3) * 2);
        int tile = entry & 0x3ff;
        int px = (entry & 0x400) ? 7 - (sx & 7) : (sx & 7);
        int py = (entry & 0x800) ? 7 - (sy & 7) : (sy & 7);
        int pal = entry >> 12;
        int idx;
        if (is256) {
            idx = VramByte(e, chr + tile * 64 + py * 8 + px);
            out[x] = idx == 0 ? TRANSPARENT : (extPltt ? ExtColor(e, slot, pal, idx) : e->bgPltt[idx]) & 0x7fff;
        } else {
            u8 b = VramByte(e, chr + tile * 32 + py * 4 + (px >> 1));
            idx = (px & 1) ? (b >> 4) : (b & 0xf);
            out[x] = idx == 0 ? TRANSPARENT : e->bgPltt[pal * 16 + idx] & 0x7fff;
        }
    }
}

static int CompareAll(const char *label)
{
    static const u16 scrolls[][2] = { { 0, 0 }, { 3, 5 }, { 255, 1 }, { 300, 200 }, { 511, 511 } };
    int failures = 0;
    for (int eng = 0; eng < 2; eng++) {
        static Eng snapshot;
        static LineFormat formats[2];
        Snapshot(&gNtrEngines[eng], &snapshot);
        Eng *e = &snapshot;
        BuildLineFormat(e, &formats[0], FALSE);
        BuildLineFormat(e, &formats[1], TRUE);
        for (int bg = 0; bg < 4; bg++) {
            u16 h = e->bghofs[bg], v = e->bgvofs[bg];
            for (size_t s = 0; s < sizeof(scrolls) / sizeof(scrolls[0]); s++) {
                e->bghofs[bg] = scrolls[s][0];
                e->bgvofs[bg] = scrolls[s][1];
                for (int y = 0; y < H; y++) {
                    u16 fast[W], fast5551[W], ref[W], ref5551[W];
                    RenderTextLine(e, bg, eng == 0, y, fast, &formats[0], 0, W);
                    RenderTextLine(e, bg, eng == 0, y, fast5551, &formats[1], 0, W);
                    ReferenceTextLine(e, bg, eng == 0, y, ref);
                    for (int x = 0; x < W; x++) {
                        ref5551[x] = (ref[x] & TRANSPARENT) ? 0 : To5551(ref[x]);
                    }
                    if (memcmp(fast, ref, sizeof(fast)) != 0 || memcmp(fast5551, ref5551, sizeof(fast5551)) != 0) {
                        if (failures++ < 5) {
                            printf("%s: engine %d BG%d scroll (%d,%d) line %d differs\n", label, eng, bg, scrolls[s][0], scrolls[s][1], y);
                        }
                    }
                }
            }
            e->bghofs[bg] = h;
            e->bgvofs[bg] = v;
        }
    }
    return failures;
}

/* Runs the real game from boot (opening, then the title after START) and
 * checks the text BGs at a few frames. */
static int sFrame, sFailures;

void NtrHost_VBlank(void)
{
    sFrame++;
    if (sFrame == 450 || sFrame == 900 || sFrame == 1100 || sFrame == 1250) {
        char label[32];
        snprintf(label, sizeof(label), "frame %d", sFrame);
        sFailures += CompareAll(label);
    }
    if (sFrame == 1250) {
        printf(sFailures ? "text BG test: %d mismatching lines\n" : "text BG test: all lines match\n", sFailures);
        exit(sFailures != 0);
    }
    u16 keys = (sFrame >= 400 && sFrame < 402) || (sFrame >= 1000 && sFrame < 1002) ? PAD_BUTTON_START : 0;
    Ntr_SetInput(keys, FALSE, 0, 0);
}

void NtrHost_Reset(u32 parameter)
{
    (void)parameter;
    exit(1);
}

int main(int argc, char **argv)
{
    NtrFS_SetRoot(argc > 1 ? argv[1] : "romfs");
    Ntr_SetBackupFile("");
    NitroMain();
    return 1;
}

u64 NtrHost_Ticks(void) { return 0; }
u64 NtrHost_TicksPerSecond(void) { return 1; }
