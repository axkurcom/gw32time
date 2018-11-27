#include "gui.h"

#include <commdlg.h>
#include <stdio.h>
#include <string.h>

#include "resource.h"
#include "../core/config_file.h"
#include "../core/diagnostics.h"
#include "../core/privilege.h"
#include "../core/service.h"
#include "../core/w32time.h"
#include "../core/w32tm.h"

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

static void sync_now(HWND dialog)
{
    int is_admin = 0;
    svc_state_t service_state = SVC_STATE_UNKNOWN;
    w32tm_raw_result_t result;

    if (privilege_is_admin(&is_admin) != 0 || !is_admin) {
        MessageBoxW(dialog, L"Sync requires an elevated administrator token.", L"GW32TIME", MB_ICONWARNING);
        return;
    }

    if (svc_query_state(L"w32time", &service_state) == 0 && service_state != SVC_STATE_RUNNING) {
        if (svc_start(L"w32time") != 0) {
            MessageBoxW(dialog, L"Could not start Windows Time service.", L"GW32TIME", MB_ICONERROR);
            return;
        }
        Sleep(1200);
    }

    if (w32tm_resync_raw(&result) != 0 || result.exit_code != 0) {
        MessageBoxW(dialog, L"Windows Time resync failed.", L"GW32TIME", MB_ICONERROR);
        refresh_status(dialog);
        return;
    }

    MessageBoxW(dialog, L"Windows Time resync was requested.", L"GW32TIME", MB_ICONINFORMATION);
    refresh_status(dialog);
}

static int choose_config_file(HWND dialog, wchar_t *path, DWORD chars, int save)
{
    OPENFILENAMEW ofn;

    ZeroMemory(&ofn, sizeof(ofn));
    path[0] = L'\0';
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = dialog;
    ofn.lpstrFilter = L"Configuration backup (*.ini)\0*.ini\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = chars;
    ofn.lpstrDefExt = L"ini";
    ofn.Flags = OFN_HIDEREADONLY | OFN_PATHMUSTEXIST;
    if (save) {
        ofn.Flags |= OFN_OVERWRITEPROMPT;
        return GetSaveFileNameW(&ofn) ? 1 : 0;
    }

    ofn.Flags |= OFN_FILEMUSTEXIST;
    return GetOpenFileNameW(&ofn) ? 1 : 0;
}

static void backup_config(HWND dialog)
{
    wchar_t path[MAX_PATH];
    w32time_config_t config;

    if (!choose_config_file(dialog, path, sizeof(path) / sizeof(path[0]), 1)) {
        return;
    }

    if (w32time_read_config(&config) != 0 || config_file_write(path, &config) != 0) {
        MessageBoxW(dialog, L"Could not write configuration backup.", L"GW32TIME", MB_ICONERROR);
        return;
    }

    MessageBoxW(dialog, L"Configuration backup was written.", L"GW32TIME", MB_ICONINFORMATION);
}

static void restore_config(HWND dialog)
{
    wchar_t path[MAX_PATH];
    w32time_config_t config;
    w32tm_raw_result_t result;
    int is_admin = 0;

    if (!choose_config_file(dialog, path, sizeof(path) / sizeof(path[0]), 0)) {
        return;
    }

    if (privilege_is_admin(&is_admin) != 0 || !is_admin) {
        MessageBoxW(dialog, L"Restore requires an elevated administrator token.", L"GW32TIME", MB_ICONWARNING);
        return;
    }

    if (MessageBoxW(dialog, L"Restore W32Time configuration from this backup?", L"GW32TIME", MB_YESNO | MB_ICONQUESTION) != IDYES) {
        return;
    }

    if (config_file_read(path, &config) != 0 || w32time_write_config(&config) != 0) {
        MessageBoxW(dialog, L"Could not restore configuration backup.", L"GW32TIME", MB_ICONERROR);
        return;
    }

    if (w32tm_config_update_raw(&result) != 0 || result.exit_code != 0 || svc_restart(L"w32time") != 0) {
        MessageBoxW(dialog, L"Configuration was restored, but service refresh failed.", L"GW32TIME", MB_ICONWARNING);
        refresh_status(dialog);
        return;
    }

    MessageBoxW(dialog, L"Configuration backup was restored.", L"GW32TIME", MB_ICONINFORMATION);
    refresh_status(dialog);
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
        case IDC_SYNC:
            sync_now(dialog);
            return TRUE;
        case IDC_BACKUP:
            backup_config(dialog);
            return TRUE;
        case IDC_RESTORE:
            restore_config(dialog);
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
