#include "gui.h"

#include <stdio.h>
#include <string.h>

#include "resource.h"
#include "../core/diagnostics.h"
#include "../core/service.h"
#include "../core/w32time.h"

static HINSTANCE g_instance;

static void set_text(HWND dialog, int id, const wchar_t *text)
{
    SetDlgItemTextW(dialog, id, text != NULL && text[0] != L'\0' ? text : L"unknown");
}

static void refresh_status(HWND dialog)
{
    svc_state_t service_state = SVC_STATE_UNKNOWN;
    svc_start_type_t start_type = SVC_START_UNKNOWN;
    w32time_config_t config;
    health_t health;
    wchar_t poll[64];

    if (diagnostics_evaluate_health(&health) == 0) {
        set_text(dialog, IDC_HEALTH, health_state_name(health.state));
    } else {
        set_text(dialog, IDC_HEALTH, L"unknown");
    }

    if (svc_query_state(L"w32time", &service_state) == 0) {
        set_text(dialog, IDC_SERVICE, svc_state_name(service_state));
    } else {
        set_text(dialog, IDC_SERVICE, L"unknown");
    }

    if (svc_query_start_type(L"w32time", &start_type) == 0) {
        set_text(dialog, IDC_START, svc_start_type_name(start_type));
    } else {
        set_text(dialog, IDC_START, L"unknown");
    }

    if (w32time_read_config(&config) != 0) {
        set_text(dialog, IDC_TYPE, L"unknown");
        set_text(dialog, IDC_SERVERS, L"unavailable");
        set_text(dialog, IDC_POLL, L"unknown");
        return;
    }

    set_text(dialog, IDC_TYPE, config.type);
    set_text(dialog, IDC_SERVERS, config.ntp_server[0] ? config.ntp_server : L"(none)");
    if (config.has_special_poll_interval) {
        _snwprintf(poll, sizeof(poll) / sizeof(poll[0]), L"%lu sec", (unsigned long)config.special_poll_interval);
        poll[(sizeof(poll) / sizeof(poll[0])) - 1] = L'\0';
        set_text(dialog, IDC_POLL, poll);
    } else {
        set_text(dialog, IDC_POLL, L"unknown");
    }
}

static INT_PTR CALLBACK main_dialog_proc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam)
{
    (void)lparam;

    switch (message) {
    case WM_INITDIALOG:
        refresh_status(dialog);
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case IDC_REFRESH:
            refresh_status(dialog);
            return TRUE;
        case IDC_EXIT:
        case IDCANCEL:
            EndDialog(dialog, 0);
            return TRUE;
        default:
            return FALSE;
        }
    default:
        return FALSE;
    }
}

int gui_launch(HINSTANCE instance)
{
    g_instance = instance;
    return (int)DialogBoxParamW(g_instance, MAKEINTRESOURCEW(IDD_MAIN), NULL, main_dialog_proc, 0);
}
