#include <string.h>
#include <wchar.h>
#include <windows.h>

#include "ntp/ntp_checker.h"
#include "ntp_probe.h"

static void set_probe_error(ntp_probe_result_t *out, const wchar_t *message)
{
    if (out == NULL) {
        return;
    }
    _snwprintf(out->error, sizeof(out->error) / sizeof(out->error[0]), L"%ls", message);
    out->error[(sizeof(out->error) / sizeof(out->error[0])) - 1] = L'\0';
}

static const wchar_t *map_error_text(gw_ntp_error_t error)
{
    if (error == GW_NTP_ERR_DNS) {
        return L"DNS lookup failed.";
    }
    if (error == GW_NTP_ERR_TIMEOUT) {
        return L"No valid SNTP response received.";
    }
    if (error == GW_NTP_ERR_SHORT_PACKET) {
        return L"SNTP response packet is too short.";
    }
    if (error == GW_NTP_ERR_INVALID_RESPONSE) {
        return L"SNTP response mode is invalid.";
    }
    if (error == GW_NTP_ERR_KISS_OF_DEATH) {
        return L"SNTP server returned kiss-of-death stratum.";
    }
    return L"SNTP request failed.";
}

static int wide_host_to_utf8(const wchar_t *host, char *out, size_t out_len)
{
    int written;

    if (host == NULL || out == NULL || out_len == 0) {
        return -1;
    }
    written = WideCharToMultiByte(CP_UTF8, 0, host, -1, out, (int)out_len, NULL, NULL);
    if (written <= 0) {
        return -1;
    }
    out[out_len - 1] = '\0';
    return 0;
}

int ntp_probe(const wchar_t *host, int timeout_ms, ntp_probe_result_t *out)
{
    char host_utf8[256];
    gw_ntp_checker_config_t cfg;
    gw_ntp_checker_result_t result;

    if (host == NULL || host[0] == L'\0' || timeout_ms <= 0 || out == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }

    ZeroMemory(out, sizeof(*out));
    if (wide_host_to_utf8(host, host_utf8, sizeof(host_utf8)) != 0) {
        set_probe_error(out, L"Host name conversion failed.");
        return 0;
    }

    ZeroMemory(&cfg, sizeof(cfg));
    cfg.samples = 1;
    cfg.timeout_ms = timeout_ms;
    cfg.interval_ms = 0;
    cfg.port = 123;

    if (gw_ntp_checker_server(host_utf8, &cfg, &result) != 0) {
        set_probe_error(out, L"SNTP checker engine failed.");
        return 0;
    }

    if (result.last_error != GW_NTP_OK || result.success_samples <= 0) {
        out->dns_ok = (result.last_error != GW_NTP_ERR_DNS) ? 1 : 0;
        set_probe_error(out, map_error_text(result.last_error));
        return 0;
    }

    out->ok = 1;
    out->dns_ok = 1;
    out->offset_ms = result.offset_mean_ms;
    out->stratum = result.stratum;
    if (result.delay_mean_ms < 0.0) {
        out->rtt_ms = 0;
    } else {
        out->rtt_ms = (DWORD)(result.delay_mean_ms > 4294967295.0 ? 4294967295.0 : result.delay_mean_ms);
    }
    return 0;
}
