/* Real-time clock: the host's local time (or a fixed clock for tests).
 * Requests complete immediately. */
#include <time.h>

#include "ntr_shim.h"

void RTC_Init(void) {}

/* Real time, or for tests 2009-03-22 (the US release) 12:00:00 plus one
 * second per 60 frames. */
static void ReadClock(RTCDate *date, RTCTime *rtcTime)
{
    struct tm fixed = { 0 };
    struct tm *t;
    if (Ntr_DeterministicTime()) {
        fixed.tm_year = 109, fixed.tm_mon = 2, fixed.tm_mday = 22, fixed.tm_hour = 12;
        time_t base = mktime(&fixed) + Ntr_FramesEnded() / 60;
        t = localtime(&base);
    } else {
        time_t now = time(NULL);
        t = localtime(&now);
    }
    if (date != NULL) {
        date->year = (u32)(t->tm_year % 100);
        date->month = (u32)(t->tm_mon + 1);
        date->day = (u32)t->tm_mday;
        date->week = (RTCWeek)t->tm_wday;
    }
    if (rtcTime != NULL) {
        rtcTime->hour = (u32)t->tm_hour;
        rtcTime->minute = (u32)t->tm_min;
        rtcTime->second = (u32)(t->tm_sec > 59 ? 59 : t->tm_sec);
    }
}

RTCResult RTC_GetDateTimeAsync(RTCDate *date, RTCTime *time, RTCCallback callback, void *arg)
{
    ReadClock(date, time);
    if (callback != NULL) {
        callback(RTC_RESULT_SUCCESS, arg);
    }
    return RTC_RESULT_SUCCESS;
}

RTCResult RTC_GetDateAsync(RTCDate *date, RTCCallback callback, void *arg)
{
    return RTC_GetDateTimeAsync(date, NULL, callback, arg);
}

RTCResult RTC_GetTimeAsync(RTCTime *time, RTCCallback callback, void *arg)
{
    return RTC_GetDateTimeAsync(NULL, time, callback, arg);
}

RTCResult RTC_GetDateTime(RTCDate *date, RTCTime *time)
{
    ReadClock(date, time);
    return RTC_RESULT_SUCCESS;
}

RTCResult RTC_GetDate(RTCDate *date)
{
    ReadClock(date, NULL);
    return RTC_RESULT_SUCCESS;
}

RTCResult RTC_GetTime(RTCTime *time)
{
    ReadClock(NULL, time);
    return RTC_RESULT_SUCCESS;
}
