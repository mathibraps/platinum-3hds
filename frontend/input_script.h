/* Scripted input for tests (input_script.c). */
#ifndef INPUT_SCRIPT_H
#define INPUT_SCRIPT_H

#include "ntr_shim.h"

/* Replaces the current script; FALSE if a line was not understood. */
BOOL InputScript_Parse(const char *text);

/* Adds the script's input for a frame (frames count from 1). */
void InputScript_Input(int frame, u16 *keys, BOOL *touching, u16 *x, u16 *y);

/* Whether the script asks for a snapshot of this frame. */
BOOL InputScript_Dump(int frame);

/* The script's frame count, 0 if it sets none. */
int InputScript_Frames(void);

/* The script asked for sound on its own thread (audio realtime). */
BOOL InputScript_RealtimeAudio(void);

/* The script asked for a measurement of ROM file reads first (fsbench). */
BOOL InputScript_FsBench(void);

#endif
