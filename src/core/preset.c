#include "preset.h"

#include <string.h>

static int name_is(const wchar_t *left, const wchar_t *right)
{
    return lstrcmpiW(left, right) == 0;
}

static void set_string(wchar_t *dst, size_t chars, const wchar_t *value)
{
    wcsncpy(dst, value, chars - 1);
    dst[chars - 1] = L'\0';
}

int preset_lookup(const wchar_t *name, preset_t *out)
{
    if (name == NULL || out == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }

    ZeroMemory(out, sizeof(*out));
    out->name = name;

    if (name_is(name, L"desktop")) {
        out->display_mode = L"NTP/manual";
        out->display_servers = L"time.cloudflare.com,0x8 pool.ntp.org,0x8 time.google.com,0x8";
        set_string(out->config.type, sizeof(out->config.type) / sizeof(out->config.type[0]), L"NTP");
        set_string(
            out->config.ntp_server,
            sizeof(out->config.ntp_server) / sizeof(out->config.ntp_server[0]),
            out->display_servers);
        out->config.special_poll_interval = 1024;
        out->config.has_special_poll_interval = 1;
        return 0;
    }

    if (name_is(name, L"windows-default")) {
        out->display_mode = L"NTP/manual";
        out->display_servers = L"time.windows.com,0x8";
        set_string(out->config.type, sizeof(out->config.type) / sizeof(out->config.type[0]), L"NTP");
        set_string(
            out->config.ntp_server,
            sizeof(out->config.ntp_server) / sizeof(out->config.ntp_server[0]),
            out->display_servers);
        out->config.special_poll_interval = 604800;
        out->config.has_special_poll_interval = 1;
        return 0;
    }

    if (name_is(name, L"domain")) {
        out->display_mode = L"NT5DS";
        out->display_servers = L"(unchanged)";
        set_string(out->config.type, sizeof(out->config.type) / sizeof(out->config.type[0]), L"NT5DS");
        return 0;
    }

    SetLastError(ERROR_NOT_FOUND);
    return -1;
}
