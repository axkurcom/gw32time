#include "ntp_time.h"

#include <windows.h>

static uint64_t filetime_to_unix_100ns(const FILETIME *ft)
{
    ULARGE_INTEGER value;
    uint64_t ticks_100ns;
    const uint64_t epoch_delta_100ns = 116444736000000000ULL;

    value.LowPart = ft->dwLowDateTime;
    value.HighPart = ft->dwHighDateTime;
    ticks_100ns = value.QuadPart;
    if (ticks_100ns < epoch_delta_100ns) {
        return 0;
    }
    return ticks_100ns - epoch_delta_100ns;
}

static uint64_t unix_100ns_now(void)
{
    FILETIME ft;

    GetSystemTimeAsFileTime(&ft);
    return filetime_to_unix_100ns(&ft);
}

uint64_t gw_ntp_from_unix_seconds(double unix_seconds)
{
    double ntp = unix_seconds + (double)GW_NTP_UNIX_DELTA;
    uint32_t sec;
    uint32_t frac;
    double sec_floor;

    if (ntp < 0.0) {
        ntp = 0.0;
    }

    sec = (uint32_t)ntp;
    sec_floor = (double)sec;
    frac = (uint32_t)((ntp - sec_floor) * 4294967296.0);
    return ((uint64_t)sec << 32) | (uint64_t)frac;
}

double gw_ntp_to_unix_seconds(uint64_t ntp_timestamp)
{
    uint32_t sec = (uint32_t)(ntp_timestamp >> 32);
    uint32_t frac = (uint32_t)(ntp_timestamp & 0xffffffffULL);
    double fraction_seconds = (double)frac / 4294967296.0;

    return ((double)sec - (double)GW_NTP_UNIX_DELTA) + fraction_seconds;
}

double gw_ntp_to_ms(uint64_t ntp_timestamp)
{
    return gw_ntp_to_unix_seconds(ntp_timestamp) * 1000.0;
}

double gw_ntp_diff_ms(uint64_t newer, uint64_t older)
{
    return gw_ntp_to_ms(newer) - gw_ntp_to_ms(older);
}

uint64_t gw_ntp_now_timestamp(void)
{
    uint64_t unix_100ns = unix_100ns_now();
    double unix_seconds = (double)unix_100ns / 10000000.0;

    return gw_ntp_from_unix_seconds(unix_seconds);
}

int gw_clock_sample_now(gw_clock_sample_t *sample)
{
    LARGE_INTEGER counter;
    LARGE_INTEGER frequency;

    if (sample == NULL) {
        return -1;
    }
    if (!QueryPerformanceCounter(&counter) || !QueryPerformanceFrequency(&frequency) || frequency.QuadPart == 0) {
        return -1;
    }

    sample->ntp_wall_timestamp = gw_ntp_now_timestamp();
    sample->mono_ms = ((double)counter.QuadPart * 1000.0) / (double)frequency.QuadPart;
    return 0;
}

double gw_mono_diff_ms(double newer, double older)
{
    return newer - older;
}
