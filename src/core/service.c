#include "service.h"

#include <stdlib.h>

#ifndef _SERVICE_DELAYED_AUTO_START_INFO_
typedef struct _SERVICE_DELAYED_AUTO_START_INFO {
    BOOL fDelayedAutostart;
} SERVICE_DELAYED_AUTO_START_INFO, *LPSERVICE_DELAYED_AUTO_START_INFO;
#endif

static svc_state_t map_state(DWORD state)
{
    switch (state) {
    case SERVICE_STOPPED:
        return SVC_STATE_STOPPED;
    case SERVICE_START_PENDING:
        return SVC_STATE_START_PENDING;
    case SERVICE_STOP_PENDING:
        return SVC_STATE_STOP_PENDING;
    case SERVICE_RUNNING:
        return SVC_STATE_RUNNING;
    case SERVICE_CONTINUE_PENDING:
        return SVC_STATE_CONTINUE_PENDING;
    case SERVICE_PAUSE_PENDING:
        return SVC_STATE_PAUSE_PENDING;
    case SERVICE_PAUSED:
        return SVC_STATE_PAUSED;
    default:
        return SVC_STATE_UNKNOWN;
    }
}

static svc_start_type_t map_start_type(DWORD start_type)
{
    switch (start_type) {
    case SERVICE_BOOT_START:
        return SVC_START_BOOT;
    case SERVICE_SYSTEM_START:
        return SVC_START_SYSTEM;
    case SERVICE_AUTO_START:
        return SVC_START_AUTO;
    case SERVICE_DEMAND_START:
        return SVC_START_MANUAL;
    case SERVICE_DISABLED:
        return SVC_START_DISABLED;
    default:
        return SVC_START_UNKNOWN;
    }
}

static int query_delayed_auto(SC_HANDLE service, int *out_delayed)
{
    SERVICE_DELAYED_AUTO_START_INFO info;
    DWORD bytes_needed = 0;

    if (out_delayed == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }
    *out_delayed = 0;

    ZeroMemory(&info, sizeof(info));
    if (!QueryServiceConfig2W(
            service,
            SERVICE_CONFIG_DELAYED_AUTO_START_INFO,
            (LPBYTE)&info,
            sizeof(info),
            &bytes_needed)) {
        return -1;
    }
    *out_delayed = info.fDelayedAutostart ? 1 : 0;
    return 0;
}

static SC_HANDLE open_service_handle(const wchar_t *name, DWORD manager_access, DWORD service_access, SC_HANDLE *scm)
{
    SC_HANDLE manager;
    SC_HANDLE service;

    manager = OpenSCManagerW(NULL, NULL, manager_access);
    if (manager == NULL) {
        return NULL;
    }

    service = OpenServiceW(manager, name, service_access);
    if (service == NULL) {
        DWORD last_error = GetLastError();
        CloseServiceHandle(manager);
        SetLastError(last_error);
        return NULL;
    }

    *scm = manager;
    return service;
}

static SC_HANDLE open_service_for_query(const wchar_t *name, DWORD access, SC_HANDLE *scm)
{
    return open_service_handle(name, SC_MANAGER_CONNECT, access, scm);
}

int svc_query_state(const wchar_t *name, svc_state_t *out)
{
    SC_HANDLE scm;
    SC_HANDLE service;
    SERVICE_STATUS_PROCESS status;
    DWORD bytes_needed = 0;

    if (name == NULL || out == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }

    *out = SVC_STATE_UNKNOWN;
    service = open_service_for_query(name, SERVICE_QUERY_STATUS, &scm);
    if (service == NULL) {
        return -1;
    }

    if (!QueryServiceStatusEx(
            service,
            SC_STATUS_PROCESS_INFO,
            (LPBYTE)&status,
            sizeof(status),
            &bytes_needed)) {
        DWORD last_error = GetLastError();
        CloseServiceHandle(service);
        CloseServiceHandle(scm);
        SetLastError(last_error);
        return -1;
    }

    *out = map_state(status.dwCurrentState);
    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    return 0;
}

int svc_query_start_type(const wchar_t *name, svc_start_type_t *out)
{
    SC_HANDLE scm;
    SC_HANDLE service;
    QUERY_SERVICE_CONFIGW *config;
    DWORD bytes_needed = 0;
    DWORD last_error;

    if (name == NULL || out == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }

    *out = SVC_START_UNKNOWN;
    service = open_service_for_query(name, SERVICE_QUERY_CONFIG, &scm);
    if (service == NULL) {
        return -1;
    }

    QueryServiceConfigW(service, NULL, 0, &bytes_needed);
    last_error = GetLastError();
    if (last_error != ERROR_INSUFFICIENT_BUFFER) {
        CloseServiceHandle(service);
        CloseServiceHandle(scm);
        SetLastError(last_error);
        return -1;
    }

    config = (QUERY_SERVICE_CONFIGW *)malloc(bytes_needed);
    if (config == NULL) {
        CloseServiceHandle(service);
        CloseServiceHandle(scm);
        SetLastError(ERROR_OUTOFMEMORY);
        return -1;
    }

    if (!QueryServiceConfigW(service, config, bytes_needed, &bytes_needed)) {
        last_error = GetLastError();
        free(config);
        CloseServiceHandle(service);
        CloseServiceHandle(scm);
        SetLastError(last_error);
        return -1;
    }

    *out = map_start_type(config->dwStartType);
    if (*out == SVC_START_AUTO) {
        int delayed = 0;
        if (query_delayed_auto(service, &delayed) == 0 && delayed) {
            *out = SVC_START_AUTO_DELAYED;
        }
    }
    free(config);
    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    return 0;
}

int svc_start(const wchar_t *name)
{
    SC_HANDLE scm;
    SC_HANDLE service;
    DWORD last_error;

    if (name == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }

    service = open_service_handle(name, SC_MANAGER_CONNECT, SERVICE_START, &scm);
    if (service == NULL) {
        return -1;
    }

    if (!StartServiceW(service, 0, NULL)) {
        last_error = GetLastError();
        CloseServiceHandle(service);
        CloseServiceHandle(scm);
        if (last_error == ERROR_SERVICE_ALREADY_RUNNING) {
            return 0;
        }
        SetLastError(last_error);
        return -1;
    }

    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    return 0;
}

int svc_stop(const wchar_t *name)
{
    SC_HANDLE scm;
    SC_HANDLE service;
    SERVICE_STATUS status;
    DWORD last_error;

    if (name == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }

    service = open_service_handle(name, SC_MANAGER_CONNECT, SERVICE_STOP | SERVICE_QUERY_STATUS, &scm);
    if (service == NULL) {
        return -1;
    }

    if (!ControlService(service, SERVICE_CONTROL_STOP, &status)) {
        last_error = GetLastError();
        CloseServiceHandle(service);
        CloseServiceHandle(scm);
        if (last_error == ERROR_SERVICE_NOT_ACTIVE) {
            return 0;
        }
        SetLastError(last_error);
        return -1;
    }

    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    return 0;
}

static int svc_wait_for_state(const wchar_t *name, svc_state_t desired, DWORD timeout_ms)
{
    DWORD waited = 0;
    svc_state_t state;

    while (waited <= timeout_ms) {
        if (svc_query_state(name, &state) == 0 && state == desired) {
            return 0;
        }
        Sleep(250);
        waited += 250;
    }
    SetLastError(ERROR_SERVICE_REQUEST_TIMEOUT);
    return -1;
}

static int svc_wait_until_not_pending(const wchar_t *name, svc_state_t *out, DWORD timeout_ms)
{
    DWORD waited = 0;
    svc_state_t state = SVC_STATE_UNKNOWN;

    while (waited <= timeout_ms) {
        if (svc_query_state(name, &state) != 0) {
            return -1;
        }
        if (state != SVC_STATE_START_PENDING && state != SVC_STATE_STOP_PENDING) {
            if (out != NULL) {
                *out = state;
            }
            return 0;
        }
        Sleep(250);
        waited += 250;
    }
    SetLastError(ERROR_SERVICE_REQUEST_TIMEOUT);
    return -1;
}

static int svc_stop_for_restart(const wchar_t *name, DWORD timeout_ms)
{
    DWORD waited = 0;
    DWORD last_error = ERROR_SUCCESS;
    svc_state_t state = SVC_STATE_UNKNOWN;

    while (waited <= timeout_ms) {
        if (svc_query_state(name, &state) != 0) {
            return -1;
        }
        if (state == SVC_STATE_STOPPED) {
            return 0;
        }
        if (state == SVC_STATE_STOP_PENDING) {
            Sleep(250);
            waited += 250;
            continue;
        }
        if (state == SVC_STATE_RUNNING) {
            if (svc_stop(name) == 0) {
                return svc_wait_for_state(name, SVC_STATE_STOPPED, timeout_ms - waited);
            }
            last_error = GetLastError();
            if (last_error == ERROR_SERVICE_CANNOT_ACCEPT_CTRL || last_error == ERROR_DEPENDENT_SERVICES_RUNNING) {
                Sleep(250);
                waited += 250;
                continue;
            }
            SetLastError(last_error);
            return -1;
        }
        if (state == SVC_STATE_START_PENDING) {
            Sleep(250);
            waited += 250;
            continue;
        }
        SetLastError(ERROR_SERVICE_CANNOT_ACCEPT_CTRL);
        return -1;
    }
    SetLastError(last_error != ERROR_SUCCESS ? last_error : ERROR_SERVICE_REQUEST_TIMEOUT);
    return -1;
}

int svc_restart(const wchar_t *name)
{
    svc_state_t state;

    if (svc_wait_until_not_pending(name, &state, 10000) != 0) {
        return -1;
    }
    if (state == SVC_STATE_RUNNING || state == SVC_STATE_STOP_PENDING || state == SVC_STATE_START_PENDING) {
        if (svc_stop_for_restart(name, 10000) != 0) {
            return -1;
        }
    } else if (state != SVC_STATE_STOPPED) {
        SetLastError(ERROR_SERVICE_CANNOT_ACCEPT_CTRL);
        return -1;
    }

    if (svc_start(name) != 0) {
        if (svc_query_state(name, &state) != 0 || state != SVC_STATE_RUNNING) {
            return -1;
        }
    }

    return svc_wait_for_state(name, SVC_STATE_RUNNING, 10000);
}

int svc_set_start_type(const wchar_t *name, svc_start_type_t start_type)
{
    SC_HANDLE scm;
    SC_HANDLE service;
    DWORD win_start = SERVICE_NO_CHANGE;
    SERVICE_DELAYED_AUTO_START_INFO delayed;
    int apply_delayed = 0;
    DWORD last_error;

    if (name == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }

    switch (start_type) {
    case SVC_START_AUTO:
        win_start = SERVICE_AUTO_START;
        delayed.fDelayedAutostart = FALSE;
        apply_delayed = 1;
        break;
    case SVC_START_AUTO_DELAYED:
        win_start = SERVICE_AUTO_START;
        delayed.fDelayedAutostart = TRUE;
        apply_delayed = 1;
        break;
    case SVC_START_MANUAL:
        win_start = SERVICE_DEMAND_START;
        delayed.fDelayedAutostart = FALSE;
        apply_delayed = 1;
        break;
    case SVC_START_DISABLED:
        win_start = SERVICE_DISABLED;
        delayed.fDelayedAutostart = FALSE;
        apply_delayed = 1;
        break;
    default:
        SetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }

    service = open_service_handle(name, SC_MANAGER_CONNECT, SERVICE_CHANGE_CONFIG, &scm);
    if (service == NULL) {
        return -1;
    }

    if (!ChangeServiceConfigW(
            service,
            SERVICE_NO_CHANGE,
            win_start,
            SERVICE_NO_CHANGE,
            NULL,
            NULL,
            NULL,
            NULL,
            NULL,
            NULL,
            NULL)) {
        last_error = GetLastError();
        CloseServiceHandle(service);
        CloseServiceHandle(scm);
        SetLastError(last_error);
        return -1;
    }

    if (apply_delayed) {
        if (!ChangeServiceConfig2W(service, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, (LPVOID)&delayed)) {
            last_error = GetLastError();
            CloseServiceHandle(service);
            CloseServiceHandle(scm);
            SetLastError(last_error);
            return -1;
        }
    }

    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    return 0;
}

const wchar_t *svc_state_name(svc_state_t state)
{
    switch (state) {
    case SVC_STATE_STOPPED:
        return L"Stopped";
    case SVC_STATE_START_PENDING:
        return L"Start pending";
    case SVC_STATE_STOP_PENDING:
        return L"Stop pending";
    case SVC_STATE_RUNNING:
        return L"Running";
    case SVC_STATE_CONTINUE_PENDING:
        return L"Continue pending";
    case SVC_STATE_PAUSE_PENDING:
        return L"Pause pending";
    case SVC_STATE_PAUSED:
        return L"Paused";
    default:
        return L"Unknown";
    }
}

const wchar_t *svc_start_type_name(svc_start_type_t start_type)
{
    switch (start_type) {
    case SVC_START_BOOT:
        return L"Boot";
    case SVC_START_SYSTEM:
        return L"System";
    case SVC_START_AUTO:
        return L"Automatic";
    case SVC_START_AUTO_DELAYED:
        return L"Automatic (Delayed)";
    case SVC_START_MANUAL:
        return L"Manual";
    case SVC_START_DISABLED:
        return L"Disabled";
    default:
        return L"Unknown";
    }
}
