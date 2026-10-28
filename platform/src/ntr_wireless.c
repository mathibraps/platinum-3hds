/*
 * Local wireless: the driver starts and stops as on a DS whose wireless
 * chip works, but nothing is ever found nearby (the WM functions are
 * stubs, see game/stubs/net_stubs.c). Requests complete immediately.
 */
#include "ntr_shim.h"

WVRResult WVR_StartUpAsync(GXVRamARM7 vram, WVRCallbackFunc callback, void *arg)
{
    (void)vram;
    if (callback != NULL) {
        callback(arg, WVR_RESULT_SUCCESS);
    }
    return WVR_RESULT_OPERATING;
}

WVRResult WVR_TerminateAsync(WVRCallbackFunc callback, void *arg)
{
    if (callback != NULL) {
        callback(arg, WVR_RESULT_SUCCESS);
    }
    return WVR_RESULT_OPERATING;
}
