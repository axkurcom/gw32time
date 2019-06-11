#ifndef GW32TIME_NTP_TIME_H
#define GW32TIME_NTP_TIME_H

#include <stdint.h>

#define GW_NTP_UNIX_DELTA 2208988800UL

typedef struct gw_clock_sample {
    uint64_t ntp_wall_timestamp;
    double mono_ms;
} gw_clock_sample_t;

uint64_t gw_ntp_now_timestamp(void);
uint64_t gw_ntp_from_unix_seconds(double unix_seconds);
double gw_ntp_to_unix_seconds(uint64_t ntp_timestamp);
double gw_ntp_to_ms(uint64_t ntp_timestamp);
double gw_ntp_diff_ms(uint64_t newer, uint64_t older);

int gw_clock_sample_now(gw_clock_sample_t *sample);
double gw_mono_diff_ms(double newer, double older);

#endif
