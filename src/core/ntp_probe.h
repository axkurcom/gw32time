#ifndef GW32TIME_NTP_PROBE_H
#define GW32TIME_NTP_PROBE_H

#include <windows.h>

typedef struct {
    int ok;
    int dns_ok;
    DWORD rtt_ms;
    double offset_ms;
    int stratum;
    wchar_t error[256];
} ntp_probe_result_t;

int ntp_probe(const wchar_t *host, int timeout_ms, ntp_probe_result_t *out);

#endif
