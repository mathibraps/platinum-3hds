/* GPU composition of the DS screens on the 3DS (gpu2d_3ds.c). */
#ifndef GPU2D_3DS_H
#define GPU2D_3DS_H

#include <citro3d.h>

/* Call after C3D_Init. */
void Gpu2D_Init(void);

/* Start of a citro3d frame, after C3D_FrameBegin. */
void Gpu2D_BeginFrame(void);

/* The CPU work for both 2D engines, in steps that spread it over two cores:
 * Gpu2D_Begin (citro3d thread) decides how each engine is composed;
 * Gpu2D_Work(0) and Gpu2D_Work(1) each decode half of both engines' lines (or
 * compose an engine on the CPU when the GPU passes do not cover its frame:
 * the sub engine in part 0, the main one in part 1, which must run on the
 * citro3d thread as it may read the 3D frame back); Gpu2D_End after both. */
void Gpu2D_Begin(void);
void Gpu2D_Work(int part);
/* Then each engine's composition is finished (on either thread, the two
 * engines at the same time), and Gpu2D_End (citro3d thread) composes on
 * the CPU what the passes could not cover. */
void Gpu2D_EndEngine(int engine);
/* Whether an engine has layers to finish this frame (most of EndEngine's work). */
bool Gpu2D_EngineHasNewLayers(int engine);
void Gpu2D_End(void);

/* Turns what Gpu2D_Work made into textures and draws the engine's screen
 * into `target` (outW x outH, a screen target with a stencil buffer). */
void Gpu2D_Draw(int engine, C3D_RenderTarget *target, int outW, int outH);

/* GSPGPU_FlushDataCache, timed (gGpuFlushTicks: system ticks so far). */
void Gpu_FlushDataCache(const void *p, u32 size);
extern u64 gGpuFlushTicks;

/* Whether the engine was composed on the CPU in the last frame. */
bool Gpu2D_UsedSoftware(int engine);

#endif
