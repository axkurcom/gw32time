#include "service.h"

#include <stdlib.h>

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
    case SVC_START_MANUAL:
        return L"Manual";
    case SVC_START_DISABLED:
        return L"Disabled";
    default:
        return L"Unknown";
    }
}
