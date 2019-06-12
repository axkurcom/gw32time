#ifndef GW32TIME_NTP_SOCKET_H
#define GW32TIME_NTP_SOCKET_H

#include <stdint.h>

#include "ntp_packet.h"
#include "ntp_time.h"

typedef enum gw_ntp_socket_error {
    GW_NTP_SOCKET_OK = 0,
    GW_NTP_SOCKET_DNS,
    GW_NTP_SOCKET_CREATE,
    GW_NTP_SOCKET_TIMEOUT,
    GW_NTP_SOCKET_IO
} gw_ntp_socket_error_t;

int gw_ntp_socket_send_checker(
    const char *host,
    uint16_t port,
    int timeout_ms,
    gw_ntp_packet_t *response,
    gw_clock_sample_t *t1,
    gw_clock_sample_t *t4,
    int *response_bytes,
    gw_ntp_socket_error_t *error_out);

#endif
