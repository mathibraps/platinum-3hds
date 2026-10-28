/*
 * Touch panel. The front end reports touches in screen pixels, so "raw"
 * samples already are screen coordinates and calibration is the identity.
 */
#include <string.h>

#include "ntr_shim.h"

static TPData sPoint = { 0, 0, TP_TOUCH_OFF, TP_VALIDITY_VALID };
static TPData *sAutoBufs;
static u16 sAutoBufSize, sAutoIndex;

void Ntr_SetTouch(BOOL touching, u16 x, u16 y)
{
    sPoint.touch = touching ? TP_TOUCH_ON : TP_TOUCH_OFF;
    sPoint.validity = TP_VALIDITY_VALID;
    if (touching) {
        sPoint.x = x;
        sPoint.y = y;
    }
    if (sAutoBufs != NULL && sAutoBufSize != 0) {
        sAutoIndex = (u16)((sAutoIndex + 1) % sAutoBufSize);
        sAutoBufs[sAutoIndex] = sPoint;
    }
}

void TP_Init(void) {}
BOOL TP_GetUserInfo(TPCalibrateParam *calibrate)
{
    memset(calibrate, 0, sizeof(*calibrate));
    return FALSE;
}
void TP_SetCalibrateParam(const TPCalibrateParam *param) { (void)param; }
void TP_RequestSamplingAsync(void) {}

u32 TP_WaitRawResult(TPData *result)
{
    *result = sPoint;
    return 0;
}

u32 TP_WaitCalibratedResult(TPData *result)
{
    return TP_WaitRawResult(result);
}

u32 TP_GetCalibratedResult(TPData *result)
{
    return TP_WaitRawResult(result);
}

void TP_GetCalibratedPoint(TPData *disp, const TPData *raw)
{
    *disp = *raw;
}

void TP_RequestAutoSamplingStartAsync(u16 vcount, u16 frequence, TPData samplingBufs[], u16 bufSize)
{
    (void)vcount;
    (void)frequence;
    sAutoBufs = samplingBufs;
    sAutoBufSize = bufSize;
    sAutoIndex = 0;
    for (u16 i = 0; i < bufSize; i++) {
        samplingBufs[i] = sPoint;
    }
}

void TP_RequestAutoSamplingStopAsync(void)
{
    sAutoBufs = NULL;
}

void TP_WaitBusy(TPRequestCommandFlag command_flgs) { (void)command_flgs; }
u32 TP_CheckError(TPRequestCommandFlag command) { (void)command; return 0; }
u16 TP_GetLatestIndexInAuto(void) { return sAutoIndex; }

void TP_GetLatestRawPointInAuto(TPData *result)
{
    *result = sPoint;
}

void TP_GetLatestCalibratedPointInAuto(TPData *result)
{
    *result = sPoint;
}
