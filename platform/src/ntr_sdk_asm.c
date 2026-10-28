/*
 * C versions of the NitroSDK matrix routines that the SDK writes in ARM
 * assembly (libraries/fx/src/fx_mtx*.c). tools/strip_asm.py removes the
 * assembly from the SDK sources; these produce the same matrices, element
 * for element.
 */
#include <stdarg.h>

#include "ntr_shim.h"

static void Set(fx32 *dst, int n, ...)
{
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) {
        dst[i] = va_arg(ap, fx32);
    }
    va_end(ap);
}

#define ONE FX32_ONE

/* 2x2 */

void MTX_Identity22_(MtxFx22 *pDst) { Set(pDst->a, 4, ONE, 0, 0, ONE); }
void MTX_Transpose22_(const MtxFx22 *pSrc, MtxFx22 *pDst) { Set(pDst->a, 4, pSrc->_00, pSrc->_10, pSrc->_01, pSrc->_11); }
void MTX_Scale22_(MtxFx22 *pDst, fx32 x, fx32 y) { Set(pDst->a, 4, x, 0, 0, y); }
void MTX_Rot22_(MtxFx22 *pDst, fx32 sinVal, fx32 cosVal) { Set(pDst->a, 4, cosVal, sinVal, -sinVal, cosVal); }

/* 3x3 */

void MTX_Identity33_(MtxFx33 *pDst) { Set(pDst->a, 9, ONE, 0, 0, 0, ONE, 0, 0, 0, ONE); }

void MTX_Copy33To43_(const MtxFx33 *pSrc, MtxFx43 *pDst)
{
    memcpy(pDst->a, pSrc->a, sizeof(pSrc->a));
    pDst->_30 = pDst->_31 = pDst->_32 = 0;
}

void MTX_Copy33To44_(const MtxFx33 *pSrc, MtxFx44 *pDst)
{
    Set(pDst->a, 16, pSrc->_00, pSrc->_01, pSrc->_02, 0, pSrc->_10, pSrc->_11, pSrc->_12, 0,
        pSrc->_20, pSrc->_21, pSrc->_22, 0, 0, 0, 0, ONE);
}

void MTX_Transpose33_(const MtxFx33 *pSrc, MtxFx33 *pDst)
{
    MtxFx33 t;
    Set(t.a, 9, pSrc->_00, pSrc->_10, pSrc->_20, pSrc->_01, pSrc->_11, pSrc->_21, pSrc->_02, pSrc->_12, pSrc->_22);
    *pDst = t;
}

void MTX_Scale33_(MtxFx33 *pDst, fx32 x, fx32 y, fx32 z) { Set(pDst->a, 9, x, 0, 0, 0, y, 0, 0, 0, z); }
void MTX_RotX33_(MtxFx33 *pDst, fx32 s, fx32 c) { Set(pDst->a, 9, ONE, 0, 0, 0, c, s, 0, -s, c); }
void MTX_RotY33_(MtxFx33 *pDst, fx32 s, fx32 c) { Set(pDst->a, 9, c, 0, -s, 0, ONE, 0, s, 0, c); }
void MTX_RotZ33_(MtxFx33 *pDst, fx32 s, fx32 c) { Set(pDst->a, 9, c, s, 0, -s, c, 0, 0, 0, ONE); }

/* 4x3 */

void MTX_Identity43_(MtxFx43 *pDst) { Set(pDst->a, 12, ONE, 0, 0, 0, ONE, 0, 0, 0, ONE, 0, 0, 0); }

void MTX_Copy43To44_(const MtxFx43 *pSrc, MtxFx44 *pDst)
{
    Set(pDst->a, 16, pSrc->_00, pSrc->_01, pSrc->_02, 0, pSrc->_10, pSrc->_11, pSrc->_12, 0,
        pSrc->_20, pSrc->_21, pSrc->_22, 0, pSrc->_30, pSrc->_31, pSrc->_32, ONE);
}

void MTX_Transpose43_(const MtxFx43 *pSrc, MtxFx43 *pDst)
{
    MtxFx43 t;
    Set(t.a, 12, pSrc->_00, pSrc->_10, pSrc->_20, pSrc->_01, pSrc->_11, pSrc->_21, pSrc->_02, pSrc->_12, pSrc->_22, 0, 0, 0);
    *pDst = t;
}

void MTX_Scale43_(MtxFx43 *pDst, fx32 x, fx32 y, fx32 z) { Set(pDst->a, 12, x, 0, 0, 0, y, 0, 0, 0, z, 0, 0, 0); }
void MTX_RotX43_(MtxFx43 *pDst, fx32 s, fx32 c) { Set(pDst->a, 12, ONE, 0, 0, 0, c, s, 0, -s, c, 0, 0, 0); }
void MTX_RotY43_(MtxFx43 *pDst, fx32 s, fx32 c) { Set(pDst->a, 12, c, 0, -s, 0, ONE, 0, s, 0, c, 0, 0, 0); }
void MTX_RotZ43_(MtxFx43 *pDst, fx32 s, fx32 c) { Set(pDst->a, 12, c, s, 0, -s, c, 0, 0, 0, ONE, 0, 0, 0); }

/* 4x4 */

void MTX_Identity44_(MtxFx44 *pDst) { Set(pDst->a, 16, ONE, 0, 0, 0, 0, ONE, 0, 0, 0, 0, ONE, 0, 0, 0, 0, ONE); }

void MTX_Copy44To33_(const MtxFx44 *pSrc, MtxFx33 *pDst)
{
    Set(pDst->a, 9, pSrc->_00, pSrc->_01, pSrc->_02, pSrc->_10, pSrc->_11, pSrc->_12, pSrc->_20, pSrc->_21, pSrc->_22);
}

void MTX_Copy44To43_(const MtxFx44 *pSrc, MtxFx43 *pDst)
{
    Set(pDst->a, 12, pSrc->_00, pSrc->_01, pSrc->_02, pSrc->_10, pSrc->_11, pSrc->_12,
        pSrc->_20, pSrc->_21, pSrc->_22, pSrc->_30, pSrc->_31, pSrc->_32);
}

void MTX_Transpose44_(const MtxFx44 *pSrc, MtxFx44 *pDst)
{
    MtxFx44 t;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            t.m[i][j] = pSrc->m[j][i];
        }
    }
    *pDst = t;
}

void MTX_Scale44_(MtxFx44 *pDst, fx32 x, fx32 y, fx32 z) { Set(pDst->a, 16, x, 0, 0, 0, 0, y, 0, 0, 0, 0, z, 0, 0, 0, 0, ONE); }
void MTX_RotX44_(MtxFx44 *pDst, fx32 s, fx32 c) { Set(pDst->a, 16, ONE, 0, 0, 0, 0, c, s, 0, 0, -s, c, 0, 0, 0, 0, ONE); }
void MTX_RotY44_(MtxFx44 *pDst, fx32 s, fx32 c) { Set(pDst->a, 16, c, 0, -s, 0, 0, ONE, 0, 0, s, 0, c, 0, 0, 0, 0, ONE); }
void MTX_RotZ44_(MtxFx44 *pDst, fx32 s, fx32 c) { Set(pDst->a, 16, c, s, 0, 0, -s, c, 0, 0, 0, 0, ONE, 0, 0, 0, 0, ONE); }

/* MATH_QSort (math/src/qsort.c): the SDK's non-recursive quicksort, ported
 * from its assembly step by step so elements comparing equal end up in the
 * same order as on the DS. */
static void SwapBytes(u8 *a, u8 *b, u32 width)
{
    for (u32 i = 0; i < width; i++) {
        u8 t = a[i];
        a[i] = b[i];
        b[i] = t;
    }
}

void MATH_QSort(void *head, u32 num, u32 width, MATHCompareFunc comp, void *stackBuf)
{
    (void)stackBuf;
    if (num <= 1) {
        return;
    }

    u8 *stack[2 * 64];
    int sp = 0;
    u32 shift = 32 - (u32)__builtin_clz(width);

    stack[sp++] = head;
    stack[sp++] = (u8 *)head + (num - 1) * width;

    while (sp > 0) {
        u8 *right = stack[--sp];
        u8 *left = stack[--sp];

        if ((u32)(right - left) == width) {
            if (comp(left, right) > 0) {
                SwapBytes(left, right, width);
            }
            continue;
        }

        u8 *mid = left + (((u32)(right - left)) >> shift) * width;
        SwapBytes(mid, left, width);

        u8 *l = left + width;
        u8 *r = right;
        for (;;) {
            while (l < right && comp(l, left) < 0) {
                l += width;
            }
            while (comp(r, left) > 0) {
                r -= width;
            }
            if (l >= r) {
                break;
            }
            SwapBytes(l, r, width);
            l += width;
            r -= width;
            if (l > r) {
                break;
            }
        }
        SwapBytes(left, r, width);

        if (r - left > right - r) {
            if (left < r - width) {
                stack[sp++] = left;
                stack[sp++] = r - width;
            }
            if (r + width < right) {
                stack[sp++] = r + width;
                stack[sp++] = right;
            }
        } else {
            if (r + width < right) {
                stack[sp++] = r + width;
                stack[sp++] = right;
            }
            if (left < r - width) {
                stack[sp++] = left;
                stack[sp++] = r - width;
            }
        }
    }
}
