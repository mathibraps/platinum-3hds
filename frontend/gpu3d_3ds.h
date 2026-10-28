/* The DS 3D engine's pixel pipeline on the 3DS GPU (gpu3d_3ds.c). */
#ifndef GPU3D_3DS_H
#define GPU3D_3DS_H

#include <stdbool.h>

#include <citro3d.h>

/* Takes over 3D rendering from the software rasterizer. Call after C3D_Init. */
void Gpu3D_Init(void);

/* Draws the 3D frame submitted at the last SWAP_BUFFERS, if there is a new
 * one. Call inside a citro3d frame; returns true if it drew (other citro3d
 * users then set their state again). */
bool Gpu3D_Render(void);

/* Supersampling: from the next frame on, draw 3D at twice the size (if VRAM
 * allows), averaged down where the frame is used. */
void Gpu3D_SetSupersample(bool on);

/* The last 3D frame: a 512x256 RGBA8 texture with the image (the main
 * engine's output size) in its top rows, or 1024x512 with the image at twice
 * the size when supersampled (sample it at the same coordinates, with the
 * texture's linear filtering); NULL before the first frame. */
C3D_Tex *Gpu3D_Texture(void);

/* Copies the last 3D frame into gNtr3DColor/gNtr3DAlpha for the software
 * compositor. Waits for the GPU, which takes ending the citro3d frame and
 * starting another: call it before drawing to any screen. */
void Gpu3D_ReadBack(void);

#endif
