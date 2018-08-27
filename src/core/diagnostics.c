#include "diagnostics.h"

#include <stdio.h>
#include <string.h>

#include "service.h"
#include "w32time.h"
#include "w32tm.h"

static void set_health(health_t *out, health_state_t state, const wchar_t *reason)
{
    out->state = state;
    _snwprintf(out->reason, sizeof(out->reason) / sizeof(out->reason[0]), L"%ls", reason);
    out->reason[(sizeof(out->reason) / sizeof(out->reason[0])) - 1] = L'\0';
}

static int string_is(const wchar_t *left, const wchar_t *right)
{
    return lstrcmpiW(left, right) == 0;
}

static int config_requires_ntp_servers(const w32time_config_t *config)
{
    return string_is(config->type, L"NTP") || string_is(config->type, L"AllSync");
}

int diagnostics_evaluate_health(health_t *out)
{
    svc_state_t service_state;
    svc_start_type_t start_type;
    w32time_config_t config;
    w32tm_raw_result_t status;

    if (out == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }

    set_health(out, HEALTH_UNKNOWN, L"Health has not been evaluated.");

    if (svc_query_state(L"w32time", &service_state) != 0) {
        set_health(out, HEALTH_UNKNOWN, L"Windows Time service state is unavailable.");
        return 0;
    }

    if (svc_query_start_type(L"w32time", &start_type) != 0) {
        set_health(out, HEALTH_UNKNOWN, L"Windows Time service start type is unavailable.");
        return 0;
    }

    if (start_type == SVC_START_DISABLED) {
        set_health(out, HEALTH_BROKEN, L"Windows Time service is disabled.");
        return 0;
    }

    if (service_state != SVC_STATE_RUNNING) {
        set_health(out, HEALTH_BROKEN, L"Windows Time service is not running.");
        return 0;
    }

    if (w32time_read_config(&config) != 0) {
        set_health(out, HEALTH_UNKNOWN, L"W32Time registry configuration is unavailable.");
        return 0;
    }

    if (string_is(config.type, L"NoSync")) {
        set_health(out, HEALTH_WARNING, L"Windows Time is configured with Type=NoSync.");
        return 0;
    }

    if (config.has_ntp_client_enabled && config.ntp_client_enabled == 0) {
        set_health(out, HEALTH_WARNING, L"W32Time NtpClient provider is disabled.");
        return 0;
    }

    if (config_requires_ntp_servers(&config) && config.ntp_server[0] == L'\0') {
        set_health(out, HEALTH_BROKEN, L"No NTP servers are configured.");
        return 0;
    }

    if (w32tm_query_status_raw(&status) != 0 || status.exit_code != 0) {
        set_health(out, HEALTH_WARNING, L"w32tm status query did not complete successfully.");
        return 0;
    }

    set_health(out, HEALTH_OK, L"Windows Time service and basic configuration look usable.");
    return 0;
}

const wchar_t *health_state_name(health_state_t state)
{
    switch (state) {
    case HEALTH_OK:
        return L"OK";
    case HEALTH_WARNING:
        return L"WARNING";
    case HEALTH_BROKEN:
        return L"BROKEN";
    default:
        return L"UNKNOWN";
    }
}
