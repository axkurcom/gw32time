#include "w32time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "registry.h"

static const wchar_t *W32TIME_PARAMETERS =
    L"SYSTEM\\CurrentControlSet\\Services\\W32Time\\Parameters";
static const wchar_t *W32TIME_NTP_CLIENT =
    L"SYSTEM\\CurrentControlSet\\Services\\W32Time\\TimeProviders\\NtpClient";
static const wchar_t *W32TIME_CONFIG =
    L"SYSTEM\\CurrentControlSet\\Services\\W32Time\\Config";

static int is_space(wchar_t ch)
{
    return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n';
}

static int copy_range(wchar_t *dst, size_t dst_chars, const wchar_t *first, const wchar_t *last)
{
    size_t len;

    if (dst == NULL || first == NULL || last == NULL || last < first || dst_chars == 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }

    len = (size_t)(last - first);
    if (len >= dst_chars) {
        SetLastError(ERROR_BUFFER_OVERFLOW);
        return -1;
    }

    if (len > 0) {
        wmemcpy(dst, first, len);
    }
    dst[len] = L'\0';
    return 0;
}

int w32time_read_config(w32time_config_t *cfg)
{
    DWORD value;

    if (cfg == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }

    memset(cfg, 0, sizeof(*cfg));

    if (reg_read_string(
            HKEY_LOCAL_MACHINE,
            W32TIME_PARAMETERS,
            L"Type",
            cfg->type,
            sizeof(cfg->type) / sizeof(cfg->type[0])) != 0) {
        return -1;
    }

    if (reg_read_string(
            HKEY_LOCAL_MACHINE,
            W32TIME_PARAMETERS,
            L"NtpServer",
            cfg->ntp_server,
            sizeof(cfg->ntp_server) / sizeof(cfg->ntp_server[0])) != 0) {
        cfg->ntp_server[0] = L'\0';
    }

    if (reg_read_dword(HKEY_LOCAL_MACHINE, W32TIME_NTP_CLIENT, L"SpecialPollInterval", &value) == 0) {
        cfg->special_poll_interval = value;
        cfg->has_special_poll_interval = 1;
    }

    if (reg_read_dword(HKEY_LOCAL_MACHINE, W32TIME_NTP_CLIENT, L"Enabled", &value) == 0) {
        cfg->ntp_client_enabled = value;
        cfg->has_ntp_client_enabled = 1;
    }

    if (reg_read_dword(HKEY_LOCAL_MACHINE, W32TIME_CONFIG, L"MinPollInterval", &value) == 0) {
        cfg->min_poll_interval = value;
        cfg->has_min_poll_interval = 1;
    }

    if (reg_read_dword(HKEY_LOCAL_MACHINE, W32TIME_CONFIG, L"MaxPollInterval", &value) == 0) {
        cfg->max_poll_interval = value;
        cfg->has_max_poll_interval = 1;
    }

    return 0;
}

int w32time_write_manual_servers(const wchar_t *peerlist)
{
    if (peerlist == NULL || peerlist[0] == L'\0') {
        SetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }

    if (reg_write_string(HKEY_LOCAL_MACHINE, W32TIME_PARAMETERS, L"NtpServer", peerlist) != 0) {
        return -1;
    }

    return reg_write_string(HKEY_LOCAL_MACHINE, W32TIME_PARAMETERS, L"Type", L"NTP");
}

int w32time_write_poll_interval(DWORD seconds)
{
    if (seconds == 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }

    return reg_write_dword(HKEY_LOCAL_MACHINE, W32TIME_NTP_CLIENT, L"SpecialPollInterval", seconds);
}

int ntp_parse_peer_list(const wchar_t *raw, ntp_peer_list_t *out)
{
    const wchar_t *p;

    if (raw == NULL || out == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }

    memset(out, 0, sizeof(*out));
    p = raw;

    while (*p != L'\0') {
        const wchar_t *token_start;
        const wchar_t *token_end;
        const wchar_t *comma;
        ntp_peer_t *peer;
        wchar_t flags_buf[32];

        while (is_space(*p)) {
            p++;
        }

        if (*p == L'\0') {
            break;
        }

        if (out->count >= NTP_MAX_PEERS) {
            SetLastError(ERROR_BUFFER_OVERFLOW);
            return -1;
        }

        token_start = p;
        while (*p != L'\0' && !is_space(*p)) {
            p++;
        }
        token_end = p;

        comma = token_start;
        while (comma < token_end && *comma != L',') {
            comma++;
        }

        if (comma == token_start) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return -1;
        }

        peer = &out->peers[out->count];
        if (copy_range(peer->host, sizeof(peer->host) / sizeof(peer->host[0]), token_start, comma) != 0) {
            return -1;
        }

        peer->flags = 0x8;
        if (comma < token_end) {
            if (copy_range(flags_buf, sizeof(flags_buf) / sizeof(flags_buf[0]), comma + 1, token_end) != 0) {
                return -1;
            }
            if (flags_buf[0] != L'\0') {
                peer->flags = (DWORD)wcstoul(flags_buf, NULL, 0);
            }
        }

        peer->enabled = 1;
        out->count++;
    }

    return 0;
}

int ntp_format_peer_list(const ntp_peer_list_t *list, wchar_t *buf, size_t chars)
{
    int i;
    size_t used = 0;

    if (list == NULL || buf == NULL || chars == 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }

    buf[0] = L'\0';
    for (i = 0; i < list->count; i++) {
        int written;

        if (list->peers[i].host[0] == L'\0') {
            SetLastError(ERROR_INVALID_PARAMETER);
            return -1;
        }

        written = _snwprintf(
            buf + used,
            chars - used,
            L"%ls%ls,0x%lx",
            used > 0 ? L" " : L"",
            list->peers[i].host,
            (unsigned long)list->peers[i].flags);
        if (written < 0 || (size_t)written >= chars - used) {
            SetLastError(ERROR_BUFFER_OVERFLOW);
            return -1;
        }

        used += (size_t)written;
    }

    return 0;
}
