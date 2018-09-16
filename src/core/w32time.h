#ifndef GW32TIME_W32TIME_H
#define GW32TIME_W32TIME_H

#include <stddef.h>
#include <windows.h>

#define NTP_MAX_PEERS 16

typedef struct {
    wchar_t host[256];
    DWORD flags;
    int enabled;
} ntp_peer_t;

typedef struct {
    ntp_peer_t peers[NTP_MAX_PEERS];
    int count;
} ntp_peer_list_t;

typedef struct {
    wchar_t type[32];
    wchar_t ntp_server[1024];
    DWORD special_poll_interval;
    DWORD ntp_client_enabled;
    DWORD min_poll_interval;
    DWORD max_poll_interval;
    int has_special_poll_interval;
    int has_ntp_client_enabled;
    int has_min_poll_interval;
    int has_max_poll_interval;
} w32time_config_t;

int w32time_read_config(w32time_config_t *cfg);
int w32time_write_manual_servers(const wchar_t *peerlist);
int ntp_parse_peer_list(const wchar_t *raw, ntp_peer_list_t *out);
int ntp_format_peer_list(const ntp_peer_list_t *list, wchar_t *buf, size_t chars);

#endif
