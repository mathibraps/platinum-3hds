/* Software stand-in for the GPU in GPU composition (compose_ref.c). */
#ifndef COMPOSE_REF_H
#define COMPOSE_REF_H

#include "ntr_shim.h"

/* Runs the passes of `c` into `out` (RGBA8 0xRRGGBBAA, rows outStride pixels
 * apart), reading the layer images and the 3D image (RGBA8, w3d x h3d). */
void ComposeRef_Run(const NtrComposition *c, const NtrLayerImages *img, const u32 *img3d, int w3d, int h3d, int stride3d, u32 *out, int outStride);

#endif
