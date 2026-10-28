/*
 * Scripted input for tests: the desktop harness (--script FILE) and the 3DS
 * test build (sdmc:/pokeplat_script.txt) read the same scripts, so both play
 * the same frames and their snapshots can be compared.
 *
 * One command per line, frames counted from 1 (# starts a comment):
 *   press KEY FRAME [LENGTH]          hold KEY (A B X Y L R START SELECT UP
 *                                     DOWN LEFT RIGHT) for LENGTH frames (2)
 *   mash KEY FROM TO PERIOD [LENGTH]  press KEY every PERIOD frames
 *   touch X Y FRAME [LENGTH]          touch the DS touch screen at X,Y
 *   dump FRAME [FRAME...]             snapshot these frames
 *   dumpevery N                       snapshot every N frames
 *   frames N                          stop after N frames
 *   setting NAME VALUE                a port option (game/port_settings.c),
 *                                     e.g. setting 3d_edges 1
 *   audio realtime                    3DS: sound on its own thread, as in
 *                                     normal play (for timing runs; runs
 *                                     that wait on sound may then differ)
 *   heap quarantine KB                hold freed game memory back, filled
 *                                     with 0xdd, up to KB (to catch reads
 *                                     of freed memory)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "input_script.h"
#include "port_settings.h"
#include "ntr_shim.h"

#define MAX_EVENTS 1024
#define MAX_DUMPS 256

typedef struct Event {
    u16 key; /* 0 = touch */
    u16 x, y;
    int from, to, period, length;
} Event;

static Event sEvents[MAX_EVENTS];
static int sNumEvents;
static int sDumps[MAX_DUMPS], sNumDumps, sDumpEvery, sFrames;
static BOOL sRealtimeAudio;
static BOOL sFsBench;

static u16 KeyByName(const char *name)
{
    static const struct {
        const char *name;
        u16 bit;
    } keys[] = {
        { "A", PAD_BUTTON_A }, { "B", PAD_BUTTON_B }, { "SELECT", PAD_BUTTON_SELECT }, { "START", PAD_BUTTON_START },
        { "RIGHT", PAD_KEY_RIGHT }, { "LEFT", PAD_KEY_LEFT }, { "UP", PAD_KEY_UP }, { "DOWN", PAD_KEY_DOWN },
        { "R", PAD_BUTTON_R }, { "L", PAD_BUTTON_L }, { "X", PAD_BUTTON_X }, { "Y", PAD_BUTTON_Y },
    };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (strcmp(keys[i].name, name) == 0) {
            return keys[i].bit;
        }
    }
    return 0;
}

static void AddEvent(Event e)
{
    if (sNumEvents < MAX_EVENTS) {
        sEvents[sNumEvents++] = e;
    }
}

BOOL InputScript_Parse(const char *text)
{
    sNumEvents = sNumDumps = sDumpEvery = sFrames = 0;
    char line[512];
    BOOL ok = TRUE;
    while (*text) {
        size_t n = strcspn(text, "\n");
        if (n >= sizeof(line)) {
            n = sizeof(line) - 1;
        }
        memcpy(line, text, n);
        line[n] = 0;
        text += n + (text[n] == '\n');
        char *hash = strchr(line, '#');
        if (hash) {
            *hash = 0;
        }

        char cmd[16], name[16];
        int a, b, c, d, len;
        if (sscanf(line, "%15s", cmd) != 1) {
            continue;
        }
        if (strcmp(cmd, "press") == 0 && sscanf(line, "%*s %15s %d %d", name, &a, &len) >= 2) {
            if (sscanf(line, "%*s %*s %*d %d", &len) != 1) {
                len = 2;
            }
            AddEvent((Event){ KeyByName(name), 0, 0, a, a, 1, len });
        } else if (strcmp(cmd, "mash") == 0 && sscanf(line, "%*s %15s %d %d %d", name, &a, &b, &c) == 4) {
            if (sscanf(line, "%*s %*s %*d %*d %*d %d", &len) != 1) {
                len = 2;
            }
            AddEvent((Event){ KeyByName(name), 0, 0, a, b, c > 0 ? c : 1, len });
        } else if (strcmp(cmd, "touch") == 0 && sscanf(line, "%*s %d %d %d", &a, &b, &c) == 3) {
            if (sscanf(line, "%*s %*d %*d %*d %d", &len) != 1) {
                len = 2;
            }
            AddEvent((Event){ 0, (u16)a, (u16)b, c, c, 1, len });
        } else if (strcmp(cmd, "dump") == 0) {
            char *p = line + 4;
            while (sNumDumps < MAX_DUMPS && sscanf(p, "%d%n", &d, &n) == 1) {
                sDumps[sNumDumps++] = d;
                p += n;
            }
        } else if (strcmp(cmd, "dumpevery") == 0 && sscanf(line, "%*s %d", &a) == 1) {
            sDumpEvery = a;
        } else if (strcmp(cmd, "frames") == 0 && sscanf(line, "%*s %d", &a) == 1) {
            sFrames = a;
        } else if (strcmp(cmd, "audio") == 0 && sscanf(line, "%*s %15s", name) == 1 && strcmp(name, "realtime") == 0) {
            sRealtimeAudio = TRUE;
        } else if (strcmp(cmd, "skip") == 0 && sscanf(line, "%*s %15s", name) == 1) {
            gNtrDebugSkip |= strcmp(name, "mix") == 0 ? NTR_SKIP_MIX : strcmp(name, "geometry") == 0 ? NTR_SKIP_GEOMETRY
                : strcmp(name, "2d") == 0 ? NTR_SKIP_2D : strcmp(name, "prefetch") == 0 ? NTR_SKIP_PREFETCH
                : strcmp(name, "savewriter") == 0 ? NTR_SKIP_SAVE_WRITER : 0;
        } else if (strcmp(cmd, "heap") == 0 && sscanf(line, "%*s %15s %d", name, &a) == 2 && strcmp(name, "quarantine") == 0) {
            PortHeap_SetQuarantine((u32)a * 1024);
        } else if (strcmp(cmd, "fsbench") == 0) {
            sFsBench = TRUE;
        } else if (strcmp(cmd, "setting") == 0 && sscanf(line, "%*s %15s %d", name, &a) == 2 && Port_SettingByName(name) >= 0) {
            Port_SetSetting(Port_SettingByName(name), a);
        } else {
            fprintf(stderr, "input script: bad line: %s\n", line);
            ok = FALSE;
        }
    }
    return ok;
}

void InputScript_Input(int frame, u16 *keys, BOOL *touching, u16 *x, u16 *y)
{
    for (int i = 0; i < sNumEvents; i++) {
        const Event *e = &sEvents[i];
        if (frame < e->from || frame >= e->to + e->length) {
            continue;
        }
        int k = (frame - e->from) / e->period, last = (e->to - e->from) / e->period;
        int start = e->from + (k < last ? k : last) * e->period;
        if (frame >= start + e->length) {
            continue;
        }
        if (e->key) {
            *keys |= e->key;
        } else {
            *touching = TRUE;
            *x = e->x;
            *y = e->y;
        }
    }
}

BOOL InputScript_Dump(int frame)
{
    if (sDumpEvery > 0 && frame % sDumpEvery == 0) {
        return TRUE;
    }
    for (int i = 0; i < sNumDumps; i++) {
        if (sDumps[i] == frame) {
            return TRUE;
        }
    }
    return FALSE;
}

int InputScript_Frames(void)
{
    return sFrames;
}

BOOL InputScript_RealtimeAudio(void)
{
    return sRealtimeAudio;
}

BOOL InputScript_FsBench(void)
{
    return sFsBench;
}
