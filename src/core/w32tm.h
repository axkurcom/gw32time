#ifndef GW32TIME_W32TM_H
#define GW32TIME_W32TM_H

#include <stddef.h>
#include <windows.h>

typedef struct {
    wchar_t raw[8192];
    DWORD exit_code;
} w32tm_raw_result_t;

typedef struct {
    wchar_t source[256];
    wchar_t stratum[128];
    wchar_t last_sync[128];
    wchar_t poll_interval[128];
    int has_source;
    int has_stratum;
    int has_last_sync;
    int has_poll_interval;
} w32tm_status_summary_t;

int run_process_capture(const wchar_t *cmdline, wchar_t *stdout_buf, size_t stdout_chars, DWORD *exit_code);
int w32tm_query_status_raw(w32tm_raw_result_t *out);
int w32tm_query_peers_raw(w32tm_raw_result_t *out);
int w32tm_query_configuration_raw(w32tm_raw_result_t *out);
int w32tm_resync_raw(w32tm_raw_result_t *out);
int w32tm_config_manual_peers_raw(const wchar_t *peerlist, w32tm_raw_result_t *out);
int w32tm_config_update_raw(w32tm_raw_result_t *out);
int w32tm_parse_status_summary(const wchar_t *raw, w32tm_status_summary_t *out);

#endif
