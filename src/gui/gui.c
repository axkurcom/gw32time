#include <winsock2.h>
#include <ws2tcpip.h>

#include "gui.h"
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "resource.h"
#include "../core/config_file.h"
#include "../core/diagnostics.h"
#include "../core/domain.h"
#include "../core/ntp_probe.h"
#include "../core/privilege.h"
#include "../core/service.h"
#include "../core/time_set.h"
#include "../core/w32time.h"
#include "../core/w32tm.h"

#define SERVER_MAX_ROWS NTP_MAX_PEERS
#define FLAG_MASK_VALID 0x0f
#define WM_APP_PROBE_RESULT (WM_APP + 1)
#define WM_APP_PROBE_DONE (WM_APP + 2)
#define IDM_BACKUP_CONFIG 50001
#define IDM_RESTORE_CONFIG 50002
#define TIMER_CLOCK 1
#define TIMER_REALTIME_CHECK 2
#define REALTIME_MIN_SECONDS 1
#define REALTIME_MAX_SECONDS 3600

typedef struct {
    wchar_t host[256];
    DWORD flags;
    int has_probe;
    int stratum;
    DWORD ping_ms;
    double offset_ms;
    wchar_t ip[64];
    wchar_t ptr[256];
} server_row_t;

typedef struct {
    int row;
    wchar_t host[256];
    DWORD flags;
} server_edit_ctx_t;

typedef struct {
    SYSTEMTIME selected;
    int has_selected;
} set_time_dialog_ctx_t;

typedef struct {
    wchar_t host[256];
    DWORD flags;
    int has_probe;
    int stratum;
    DWORD ping_ms;
    double offset_ms;
    wchar_t ip[64];
    wchar_t ptr[256];
} probe_result_msg_t;

typedef struct {
    HWND dialog;
    int count;
    server_row_t rows[SERVER_MAX_ROWS];
} probe_run_ctx_t;

static HINSTANCE g_instance;
static HWND g_main_dialog = NULL;
static HANDLE g_probe_thread = NULL;
static LONG g_probe_running = 0;
static int g_is_admin = 0;
static HFONT g_bold_font = NULL;
static int g_realtime_seconds = 15;
static int g_realtime_updating = 0;
static server_row_t g_rows[SERVER_MAX_ROWS];
static int g_row_count = 0;
static int selected_row(HWND dialog);
static void start_probe_all_async(HWND dialog);
static void update_admin_controls(HWND dialog);
static int relaunch_elevated_gui(HWND dialog);
static int relaunch_elevated_main(HWND dialog, const wchar_t *args, int close_current);
static void layout_header_time(HWND dialog);
static void update_realtime_controls(HWND dialog);
static void restart_realtime_timer(HWND dialog);
static int parse_realtime_seconds(HWND dialog);
static void apply_probe_result(const probe_result_msg_t *msg);
static void bump_main_window_layer(HWND dialog);
static int run_elevated_set_time(HWND dialog, const SYSTEMTIME *st);

static void set_text(HWND dialog, int id, const wchar_t *text)
{
    SetDlgItemTextW(dialog, id, text != NULL && text[0] != L'\0' ? text : L"unknown");
}

static void bump_main_window_layer(HWND dialog)
{
    if (dialog == NULL) {
        return;
    }
    SetWindowPos(
        dialog,
        HWND_TOPMOST,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    SetWindowPos(
        dialog,
        HWND_NOTOPMOST,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

static void format_local_time_text(wchar_t *out, size_t chars)
{
    SYSTEMTIME st;

    GetLocalTime(&st);
    _snwprintf(
        out,
        chars,
        L"%04u-%02u-%02u %02u:%02u:%02u",
        (unsigned)st.wYear,
        (unsigned)st.wMonth,
        (unsigned)st.wDay,
        (unsigned)st.wHour,
        (unsigned)st.wMinute,
        (unsigned)st.wSecond);
    out[chars - 1] = L'\0';
}

static int parse_realtime_seconds(HWND dialog)
{
    wchar_t text[32];
    wchar_t *end = NULL;
    long value;

    GetDlgItemTextW(dialog, IDC_REALTIME_SECONDS, text, sizeof(text) / sizeof(text[0]));
    if (text[0] == L'\0') {
        return g_realtime_seconds;
    }

    value = wcstol(text, &end, 10);
    if (end == text || *end != L'\0') {
        return g_realtime_seconds;
    }
    if (value < REALTIME_MIN_SECONDS) {
        value = REALTIME_MIN_SECONDS;
    } else if (value > REALTIME_MAX_SECONDS) {
        value = REALTIME_MAX_SECONDS;
    }
    return (int)value;
}

static void update_realtime_controls(HWND dialog)
{
    int enabled = (IsDlgButtonChecked(dialog, IDC_REALTIME_CHECK) == BST_CHECKED);
    wchar_t unit[16];

    ShowWindow(GetDlgItem(dialog, IDC_REALTIME_EVERY), enabled ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(dialog, IDC_REALTIME_SECONDS), enabled ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(dialog, IDC_REALTIME_SPIN), enabled ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(dialog, IDC_REALTIME_UNIT), enabled ? SW_SHOW : SW_HIDE);

    if (!enabled) {
        KillTimer(dialog, TIMER_REALTIME_CHECK);
        return;
    }

    g_realtime_seconds = parse_realtime_seconds(dialog);
    _snwprintf(unit, sizeof(unit) / sizeof(unit[0]), L"%ls", g_realtime_seconds == 1 ? L"second" : L"seconds");
    unit[(sizeof(unit) / sizeof(unit[0])) - 1] = L'\0';
    SetDlgItemTextW(dialog, IDC_REALTIME_UNIT, unit);
    g_realtime_updating = 1;
    SetDlgItemInt(dialog, IDC_REALTIME_SECONDS, (UINT)g_realtime_seconds, FALSE);
    g_realtime_updating = 0;
}

static void restart_realtime_timer(HWND dialog)
{
    if (IsDlgButtonChecked(dialog, IDC_REALTIME_CHECK) != BST_CHECKED) {
        KillTimer(dialog, TIMER_REALTIME_CHECK);
        return;
    }

    g_realtime_seconds = parse_realtime_seconds(dialog);
    g_realtime_updating = 1;
    SetDlgItemInt(dialog, IDC_REALTIME_SECONDS, (UINT)g_realtime_seconds, FALSE);
    g_realtime_updating = 0;
    KillTimer(dialog, TIMER_REALTIME_CHECK);
    SetTimer(dialog, TIMER_REALTIME_CHECK, (UINT)(g_realtime_seconds * 1000), NULL);
}

static void refresh_datetime_block(HWND dialog)
{
    wchar_t current[64];
    wchar_t uac[16];
    int is_admin = 0;

    format_local_time_text(current, sizeof(current) / sizeof(current[0]));
    set_text(dialog, IDC_CURRENT_TIME, current);
    layout_header_time(dialog);

    if (privilege_is_admin(&is_admin) == 0 && is_admin) {
        g_is_admin = 1;
    } else {
        g_is_admin = 0;
    }
    _snwprintf(uac, sizeof(uac) / sizeof(uac[0]), L"%ls", g_is_admin ? L"[UAC ✔]" : L"[UAC ✘]");
    uac[(sizeof(uac) / sizeof(uac[0])) - 1] = L'\0';
    set_text(dialog, IDC_UAC_STATUS, uac);
    update_admin_controls(dialog);
}

static void update_admin_controls(HWND dialog)
{
    EnableWindow(GetDlgItem(dialog, IDC_SET_TIME), TRUE);
    if (g_probe_running == 0) {
        EnableWindow(GetDlgItem(dialog, IDC_APPLY_SERVERS), TRUE);
    }
}

static int relaunch_elevated_gui(HWND dialog)
{
    return relaunch_elevated_main(dialog, L"gui", 1);
}

static int run_elevated_set_time(HWND dialog, const SYSTEMTIME *st)
{
    wchar_t exe_path[MAX_PATH];
    wchar_t date_arg[32];
    wchar_t time_arg[32];
    wchar_t params[128];
    SHELLEXECUTEINFOW sei;
    DWORD exit_code = 1;

    if (st == NULL) {
        return -1;
    }
    if (GetModuleFileNameW(NULL, exe_path, sizeof(exe_path) / sizeof(exe_path[0])) == 0) {
        return -1;
    }

    _snwprintf(date_arg, sizeof(date_arg) / sizeof(date_arg[0]), L"%04u-%02u-%02u", st->wYear, st->wMonth, st->wDay);
    date_arg[(sizeof(date_arg) / sizeof(date_arg[0])) - 1] = L'\0';
    _snwprintf(time_arg, sizeof(time_arg) / sizeof(time_arg[0]), L"%02u:%02u:%02u", st->wHour, st->wMinute, st->wSecond);
    time_arg[(sizeof(time_arg) / sizeof(time_arg[0])) - 1] = L'\0';
    _snwprintf(params, sizeof(params) / sizeof(params[0]), L"__set-time %ls %ls", date_arg, time_arg);
    params[(sizeof(params) / sizeof(params[0])) - 1] = L'\0';

    ZeroMemory(&sei, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd = dialog;
    sei.lpVerb = L"runas";
    sei.lpFile = exe_path;
    sei.lpParameters = params;
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) {
        return -1;
    }
    if (sei.hProcess == NULL) {
        return -1;
    }
    WaitForSingleObject(sei.hProcess, INFINITE);
    if (!GetExitCodeProcess(sei.hProcess, &exit_code)) {
        CloseHandle(sei.hProcess);
        return -1;
    }
    CloseHandle(sei.hProcess);
    return (int)exit_code;
}

static int relaunch_elevated_main(HWND dialog, const wchar_t *args, int close_current)
{
    wchar_t exe_path[MAX_PATH];
    HINSTANCE rc;

    if (GetModuleFileNameW(NULL, exe_path, sizeof(exe_path) / sizeof(exe_path[0])) == 0) {
        MessageBoxW(dialog, L"Could not locate executable path for elevation.", L"GW32TIME", MB_ICONERROR);
        return -1;
    }

    rc = ShellExecuteW(dialog, L"runas", exe_path, args, NULL, SW_SHOWNORMAL);
    if ((INT_PTR)rc <= 32) {
        return -1;
    }
    if (close_current && dialog != NULL) {
        EndDialog(dialog, 0);
    }
    return 0;
}

static HFONT ensure_bold_font(void)
{
    LOGFONTW lf;
    HFONT base_font;

    if (g_bold_font != NULL) {
        return g_bold_font;
    }

    base_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    if (base_font == NULL || GetObjectW(base_font, sizeof(lf), &lf) == 0) {
        return NULL;
    }
    lf.lfWeight = FW_BOLD;
    g_bold_font = CreateFontIndirectW(&lf);
    return g_bold_font;
}

static void layout_header_time(HWND dialog)
{
    HWND header = GetDlgItem(dialog, IDC_HEADER_TEXT);
    HWND uac = GetDlgItem(dialog, IDC_UAC_STATUS);
    HWND current = GetDlgItem(dialog, IDC_CURRENT_TIME);
    HFONT font = ensure_bold_font();
    RECT client;
    SIZE sz_header;
    SIZE sz_uac;
    SIZE sz_time;
    wchar_t header_text[128];
    wchar_t time_text[64];
    wchar_t uac_text[16];
    HDC dc;
    HFONT old_font;
    int same_line;
    int x_time;

    if (header == NULL || uac == NULL || current == NULL) {
        return;
    }

    GetWindowTextW(header, header_text, sizeof(header_text) / sizeof(header_text[0]));
    GetWindowTextW(uac, uac_text, sizeof(uac_text) / sizeof(uac_text[0]));
    GetWindowTextW(current, time_text, sizeof(time_text) / sizeof(time_text[0]));
    GetClientRect(dialog, &client);

    dc = GetDC(dialog);
    if (dc == NULL) {
        return;
    }
    old_font = NULL;
    if (font != NULL) {
        old_font = (HFONT)SelectObject(dc, font);
    }
    GetTextExtentPoint32W(dc, header_text, (int)wcslen(header_text), &sz_header);
    GetTextExtentPoint32W(dc, uac_text, (int)wcslen(uac_text), &sz_uac);
    GetTextExtentPoint32W(dc, time_text, (int)wcslen(time_text), &sz_time);
    if (old_font != NULL) {
        SelectObject(dc, old_font);
    }
    ReleaseDC(dialog, dc);

    same_line = (10 + sz_header.cx + 12 + sz_uac.cx + 16 + sz_time.cx + 10 <= client.right);
    if (same_line) {
        x_time = client.right - sz_time.cx - 10;
        MoveWindow(uac, 10 + sz_header.cx + 12, 10, sz_uac.cx + 4, 12, TRUE);
        MoveWindow(header, 10, 10, 300, 12, TRUE);
        MoveWindow(current, x_time, 10, sz_time.cx + 4, 12, TRUE);
    } else {
        x_time = client.right - sz_time.cx - 10;
        MoveWindow(uac, 10, 24, sz_uac.cx + 4, 12, TRUE);
        MoveWindow(header, 10, 10, 300, 12, TRUE);
        MoveWindow(current, x_time, 24, sz_time.cx + 4, 12, TRUE);
    }
}

static INT_PTR CALLBACK set_time_dialog_proc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam)
{
    set_time_dialog_ctx_t *ctx = (set_time_dialog_ctx_t *)GetWindowLongPtrW(dialog, GWLP_USERDATA);

    switch (message) {
    case WM_INITDIALOG: {
        SYSTEMTIME st;
        HFONT bold_font;
        HWND time_ctrl;

        SetWindowLongPtrW(dialog, GWLP_USERDATA, lparam);
        ctx = (set_time_dialog_ctx_t *)lparam;
        GetLocalTime(&st);
        DateTime_SetSystemtime(GetDlgItem(dialog, IDC_SET_DATE_VALUE), GDT_VALID, &st);
        time_ctrl = GetDlgItem(dialog, IDC_SET_CLOCK_VALUE);
        DateTime_SetSystemtime(time_ctrl, GDT_VALID, &st);
        DateTime_SetFormat(time_ctrl, L"HH':'mm':'ss");
        SetTimer(dialog, TIMER_CLOCK, 1000, NULL);

        bold_font = ensure_bold_font();
        if (bold_font != NULL) {
            SendDlgItemMessageW(dialog, IDC_SET_DATE_VALUE, WM_SETFONT, (WPARAM)bold_font, TRUE);
            SendDlgItemMessageW(dialog, IDC_SET_CLOCK_VALUE, WM_SETFONT, (WPARAM)bold_font, TRUE);
        }
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wparam) == IDOK) {
            SYSTEMTIME date_part;
            SYSTEMTIME time_part;

            if (DateTime_GetSystemtime(GetDlgItem(dialog, IDC_SET_DATE_VALUE), &date_part) != GDT_VALID ||
                DateTime_GetSystemtime(GetDlgItem(dialog, IDC_SET_CLOCK_VALUE), &time_part) != GDT_VALID) {
                MessageBoxW(dialog, L"Invalid date or time value.", L"GW32TIME", MB_ICONWARNING);
                return TRUE;
            }
            if (ctx != NULL) {
                ZeroMemory(&ctx->selected, sizeof(ctx->selected));
                ctx->selected.wYear = date_part.wYear;
                ctx->selected.wMonth = date_part.wMonth;
                ctx->selected.wDay = date_part.wDay;
                ctx->selected.wHour = time_part.wHour;
                ctx->selected.wMinute = time_part.wMinute;
                ctx->selected.wSecond = time_part.wSecond;
                ctx->selected.wMilliseconds = 0;
                ctx->has_selected = 1;
            }
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        if (LOWORD(wparam) == IDCANCEL) {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        return FALSE;
    case WM_TIMER:
        if (wparam == TIMER_CLOCK) {
            HWND date_ctrl = GetDlgItem(dialog, IDC_SET_DATE_VALUE);
            HWND time_ctrl = GetDlgItem(dialog, IDC_SET_CLOCK_VALUE);
            SYSTEMTIME st;
            GetLocalTime(&st);
            DateTime_SetSystemtime(date_ctrl, GDT_VALID, &st);
            DateTime_SetSystemtime(time_ctrl, GDT_VALID, &st);
            return TRUE;
        }
        return FALSE;
    case WM_DESTROY:
        KillTimer(dialog, TIMER_CLOCK);
        return TRUE;
    default:
        return FALSE;
    }
}

static void set_local_datetime(HWND dialog)
{
    set_time_dialog_ctx_t ctx;
    int rc;

    ZeroMemory(&ctx, sizeof(ctx));

    rc = (int)DialogBoxParamW(g_instance, MAKEINTRESOURCEW(IDD_SET_TIME), dialog, set_time_dialog_proc, (LPARAM)&ctx);
    if (rc != IDOK || !ctx.has_selected) {
        bump_main_window_layer(dialog);
        return;
    }

    if (!g_is_admin) {
        rc = run_elevated_set_time(dialog, &ctx.selected);
        if (rc == 0) {
            refresh_datetime_block(dialog);
            MessageBoxW(dialog, L"Local date/time updated.", L"GW32TIME", MB_ICONINFORMATION);
        }
        bump_main_window_layer(dialog);
        return;
    }

    if (time_set_local(&ctx.selected) == 0) {
        refresh_datetime_block(dialog);
        MessageBoxW(dialog, L"Local date/time updated.", L"GW32TIME", MB_ICONINFORMATION);
    }
    bump_main_window_layer(dialog);
}

static void format_flags(DWORD flags, wchar_t *buf, size_t chars)
{
    wchar_t notes[128];
    int used = 0;

    notes[0] = L'\0';
    if (flags & 0x8) {
        used += _snwprintf(notes + used, (sizeof(notes) / sizeof(notes[0])) - used, L"%lsClient", used ? L"+" : L"");
    }
    if (flags & 0x1) {
        used += _snwprintf(
            notes + used,
            (sizeof(notes) / sizeof(notes[0])) - used,
            L"%lsSpecialInterval",
            used ? L"+" : L"");
    }
    if (flags & 0x2) {
        used += _snwprintf(notes + used, (sizeof(notes) / sizeof(notes[0])) - used, L"%lsFallbackOnly", used ? L"+" : L"");
    }
    if (flags & 0x4) {
        used += _snwprintf(notes + used, (sizeof(notes) / sizeof(notes[0])) - used, L"%lsSymmetricActive", used ? L"+" : L"");
    }
    notes[(sizeof(notes) / sizeof(notes[0])) - 1] = L'\0';

    if (notes[0] != L'\0') {
        _snwprintf(buf, chars, L"0x%lx (%ls)", (unsigned long)flags, notes);
    } else {
        _snwprintf(buf, chars, L"0x%lx", (unsigned long)flags);
    }
    buf[chars - 1] = L'\0';
}

static void init_servers_table(HWND dialog)
{
    HWND table = GetDlgItem(dialog, IDC_SERVERS_TABLE);
    LVCOLUMNW column;
    int i;
    const wchar_t *titles[] = { L"Server", L"Flags", L"Stratum", L"Ping", L"Offset", L"IP", L"PTR" };
    int widths[] = { 120, 118, 52, 56, 70, 84, 156 };

    ListView_SetExtendedListViewStyle(table, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    for (i = 0; i < 7; i++) {
        ZeroMemory(&column, sizeof(column));
        column.mask = LVCF_WIDTH | LVCF_TEXT | LVCF_SUBITEM;
        column.cx = widths[i];
        column.pszText = (LPWSTR)titles[i];
        column.iSubItem = i;
        ListView_InsertColumn(table, i, &column);
    }
}

static void server_row_to_table(HWND table, int row_index)
{
    LVITEMW item;
    wchar_t buf[256];

    ZeroMemory(&item, sizeof(item));
    item.mask = LVIF_TEXT;
    item.iItem = row_index;
    item.iSubItem = 0;
    item.pszText = g_rows[row_index].host;
    if (ListView_GetItemCount(table) <= row_index) {
        ListView_InsertItem(table, &item);
    } else {
        ListView_SetItem(table, &item);
    }

    format_flags(g_rows[row_index].flags, buf, sizeof(buf) / sizeof(buf[0]));
    ListView_SetItemText(table, row_index, 1, buf);

    if (g_rows[row_index].has_probe) {
        _snwprintf(buf, sizeof(buf) / sizeof(buf[0]), L"%d", g_rows[row_index].stratum);
        buf[(sizeof(buf) / sizeof(buf[0])) - 1] = L'\0';
        ListView_SetItemText(table, row_index, 2, buf);
        _snwprintf(buf, sizeof(buf) / sizeof(buf[0]), L"%lu ms", (unsigned long)g_rows[row_index].ping_ms);
        buf[(sizeof(buf) / sizeof(buf[0])) - 1] = L'\0';
        ListView_SetItemText(table, row_index, 3, buf);
        _snwprintf(buf, sizeof(buf) / sizeof(buf[0]), L"%.0f ms", g_rows[row_index].offset_ms);
        buf[(sizeof(buf) / sizeof(buf[0])) - 1] = L'\0';
        ListView_SetItemText(table, row_index, 4, buf);
    } else {
        ListView_SetItemText(table, row_index, 2, L"-");
        ListView_SetItemText(table, row_index, 3, L"-");
        ListView_SetItemText(table, row_index, 4, L"-");
    }

    ListView_SetItemText(table, row_index, 5, g_rows[row_index].ip[0] ? g_rows[row_index].ip : L"-");
    ListView_SetItemText(table, row_index, 6, g_rows[row_index].ptr[0] ? g_rows[row_index].ptr : L"-");
}

static void refresh_servers_table(HWND dialog)
{
    HWND table = GetDlgItem(dialog, IDC_SERVERS_TABLE);
    int i;

    ListView_DeleteAllItems(table);
    for (i = 0; i < g_row_count; i++) {
        server_row_to_table(table, i);
    }
}

static void apply_probe_result(const probe_result_msg_t *msg)
{
    int i;
    HWND table;

    if (msg == NULL || g_main_dialog == NULL) {
        return;
    }

    for (i = 0; i < g_row_count; i++) {
        if (_wcsicmp(g_rows[i].host, msg->host) == 0 && g_rows[i].flags == msg->flags) {
            g_rows[i].has_probe = msg->has_probe;
            g_rows[i].stratum = msg->stratum;
            g_rows[i].ping_ms = msg->ping_ms;
            g_rows[i].offset_ms = msg->offset_ms;
            wcsncpy(g_rows[i].ip, msg->ip, (sizeof(g_rows[i].ip) / sizeof(g_rows[i].ip[0])) - 1);
            g_rows[i].ip[(sizeof(g_rows[i].ip) / sizeof(g_rows[i].ip[0])) - 1] = L'\0';
            wcsncpy(g_rows[i].ptr, msg->ptr, (sizeof(g_rows[i].ptr) / sizeof(g_rows[i].ptr[0])) - 1);
            g_rows[i].ptr[(sizeof(g_rows[i].ptr) / sizeof(g_rows[i].ptr[0])) - 1] = L'\0';
            table = GetDlgItem(g_main_dialog, IDC_SERVERS_TABLE);
            server_row_to_table(table, i);
            break;
        }
    }
}

static int resolve_ip_ptr(const wchar_t *host, wchar_t *ip, size_t ip_chars, wchar_t *ptr, size_t ptr_chars)
{
    WSADATA wsa;
    ADDRINFOW hints;
    ADDRINFOW *resolved = NULL;
    DWORD ip_len;
    int rc;

    ip[0] = L'\0';
    ptr[0] = L'\0';

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return -1;
    }

    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    rc = GetAddrInfoW(host, L"123", &hints, &resolved);
    if (rc != 0 || resolved == NULL) {
        WSACleanup();
        return -1;
    }

    ip_len = (DWORD)ip_chars;
    if (WSAAddressToStringW(resolved->ai_addr, (DWORD)resolved->ai_addrlen, NULL, ip, &ip_len) != 0) {
        FreeAddrInfoW(resolved);
        WSACleanup();
        return -1;
    }

    if (GetNameInfoW(
            resolved->ai_addr,
            (DWORD)resolved->ai_addrlen,
            ptr,
            (DWORD)ptr_chars,
            NULL,
            0,
            NI_NAMEREQD) != 0) {
        ptr[0] = L'\0';
    }

    FreeAddrInfoW(resolved);
    WSACleanup();
    return 0;
}

static void load_servers_from_config(const w32time_config_t *config)
{
    ntp_peer_list_t peers;
    int i;

    g_row_count = 0;
    if (config->ntp_server[0] == L'\0') {
        return;
    }

    if (ntp_parse_peer_list(config->ntp_server, &peers) != 0) {
        return;
    }

    for (i = 0; i < peers.count && i < SERVER_MAX_ROWS; i++) {
        g_rows[g_row_count].host[0] = L'\0';
        wcsncpy(g_rows[g_row_count].host, peers.peers[i].host, (sizeof(g_rows[g_row_count].host) / sizeof(g_rows[g_row_count].host[0])) - 1);
        g_rows[g_row_count].host[(sizeof(g_rows[g_row_count].host) / sizeof(g_rows[g_row_count].host[0])) - 1] = L'\0';
        g_rows[g_row_count].flags = peers.peers[i].flags;
        g_rows[g_row_count].has_probe = 0;
        g_rows[g_row_count].ip[0] = L'\0';
        g_rows[g_row_count].ptr[0] = L'\0';
        g_row_count++;
    }
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
        set_text(dialog, IDC_POLL, L"unknown");
        g_row_count = 0;
        refresh_servers_table(dialog);
        refresh_datetime_block(dialog);
        return;
    }

    set_text(dialog, IDC_TYPE, config.type);
    if (config.has_special_poll_interval) {
        _snwprintf(poll, sizeof(poll) / sizeof(poll[0]), L"%lu sec", (unsigned long)config.special_poll_interval);
        poll[(sizeof(poll) / sizeof(poll[0])) - 1] = L'\0';
        set_text(dialog, IDC_POLL, poll);
    } else {
        set_text(dialog, IDC_POLL, L"unknown");
    }

    load_servers_from_config(&config);
    refresh_servers_table(dialog);
    refresh_datetime_block(dialog);
}

static void sync_now(HWND dialog)
{
    int is_admin = 0;
    svc_state_t service_state = SVC_STATE_UNKNOWN;
    w32tm_raw_result_t result;

    if (privilege_is_admin(&is_admin) != 0 || !is_admin) {
        relaunch_elevated_gui(dialog);
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

    if (config_file_read(path, &config) != 0) {
        MessageBoxW(dialog, L"Could not read configuration backup.", L"GW32TIME", MB_ICONERROR);
        return;
    }

    {
        w32time_config_t current;
        wchar_t preview[1536];

        ZeroMemory(&current, sizeof(current));
        if (w32time_read_config(&current) == 0) {
            _snwprintf(
                preview,
                sizeof(preview) / sizeof(preview[0]),
                L"Restore W32Time configuration from this backup?\n\n"
                L"Current -> Backup\n"
                L"Type: %ls -> %ls\n"
                L"NtpServer: %ls -> %ls\n"
                L"SpecialPollInterval: %ls%lu -> %ls%lu\n"
                L"NtpClientEnabled: %ls%lu -> %ls%lu\n"
                L"MinPollInterval: %ls%lu -> %ls%lu\n"
                L"MaxPollInterval: %ls%lu -> %ls%lu",
                current.type[0] ? current.type : L"-",
                config.type[0] ? config.type : L"-",
                current.ntp_server[0] ? current.ntp_server : L"-",
                config.ntp_server[0] ? config.ntp_server : L"-",
                current.has_special_poll_interval ? L"" : L"(unset) ",
                (unsigned long)current.special_poll_interval,
                config.has_special_poll_interval ? L"" : L"(unset) ",
                (unsigned long)config.special_poll_interval,
                current.has_ntp_client_enabled ? L"" : L"(unset) ",
                (unsigned long)current.ntp_client_enabled,
                config.has_ntp_client_enabled ? L"" : L"(unset) ",
                (unsigned long)config.ntp_client_enabled,
                current.has_min_poll_interval ? L"" : L"(unset) ",
                (unsigned long)current.min_poll_interval,
                config.has_min_poll_interval ? L"" : L"(unset) ",
                (unsigned long)config.min_poll_interval,
                current.has_max_poll_interval ? L"" : L"(unset) ",
                (unsigned long)current.max_poll_interval,
                config.has_max_poll_interval ? L"" : L"(unset) ",
                (unsigned long)config.max_poll_interval);
            preview[(sizeof(preview) / sizeof(preview[0])) - 1] = L'\0';
            if (MessageBoxW(dialog, preview, L"GW32TIME", MB_YESNO | MB_ICONQUESTION) != IDYES) {
                return;
            }
        } else if (MessageBoxW(dialog, L"Restore W32Time configuration from this backup?", L"GW32TIME", MB_YESNO | MB_ICONQUESTION) != IDYES) {
            return;
        }
    }

    if (MessageBoxW(dialog, L"Continue restore and restart Windows Time service?", L"GW32TIME", MB_YESNO | MB_ICONQUESTION) != IDYES) {
        return;
    }

    if (w32time_write_config(&config) != 0) {
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

static int selected_row(HWND dialog)
{
    return ListView_GetNextItem(GetDlgItem(dialog, IDC_SERVERS_TABLE), -1, LVNI_SELECTED);
}

static DWORD read_flags_from_dialog(HWND dialog)
{
    DWORD flags = 0;

    if (IsDlgButtonChecked(dialog, IDC_DIALOG_FLAG_CLIENT) == BST_CHECKED) {
        flags |= 0x8;
    }
    if (IsDlgButtonChecked(dialog, IDC_DIALOG_FLAG_SPECIAL) == BST_CHECKED) {
        flags |= 0x1;
    }
    if (IsDlgButtonChecked(dialog, IDC_DIALOG_FLAG_FALLBACK) == BST_CHECKED) {
        flags |= 0x2;
    }
    if (IsDlgButtonChecked(dialog, IDC_DIALOG_FLAG_SYMMETRIC) == BST_CHECKED) {
        flags |= 0x4;
    }
    return flags;
}

static void write_flags_to_dialog(HWND dialog, DWORD flags)
{
    CheckDlgButton(dialog, IDC_DIALOG_FLAG_CLIENT, (flags & 0x8) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dialog, IDC_DIALOG_FLAG_SPECIAL, (flags & 0x1) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dialog, IDC_DIALOG_FLAG_FALLBACK, (flags & 0x2) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dialog, IDC_DIALOG_FLAG_SYMMETRIC, (flags & 0x4) ? BST_CHECKED : BST_UNCHECKED);
}

static INT_PTR CALLBACK server_edit_dialog_proc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam)
{
    server_edit_ctx_t *ctx = (server_edit_ctx_t *)GetWindowLongPtrW(dialog, GWLP_USERDATA);

    switch (message) {
    case WM_INITDIALOG:
        SetWindowLongPtrW(dialog, GWLP_USERDATA, lparam);
        ctx = (server_edit_ctx_t *)lparam;
        if (ctx != NULL) {
            SetDlgItemTextW(dialog, IDC_DIALOG_SERVER, ctx->host);
            write_flags_to_dialog(dialog, ctx->flags);
        }
        return TRUE;

    case WM_COMMAND:
        if (LOWORD(wparam) == IDOK) {
            DWORD flags;
            wchar_t host[256];

            if (ctx == NULL) {
                EndDialog(dialog, IDCANCEL);
                return TRUE;
            }

            GetDlgItemTextW(dialog, IDC_DIALOG_SERVER, host, sizeof(host) / sizeof(host[0]));
            if (host[0] == L'\0') {
                MessageBoxW(dialog, L"Server host is required.", L"GW32TIME", MB_ICONWARNING);
                return TRUE;
            }

            flags = read_flags_from_dialog(dialog);
            if ((flags & FLAG_MASK_VALID) == 0) {
                MessageBoxW(dialog, L"Choose at least one flag.", L"GW32TIME", MB_ICONWARNING);
                return TRUE;
            }

            wcsncpy(ctx->host, host, (sizeof(ctx->host) / sizeof(ctx->host[0])) - 1);
            ctx->host[(sizeof(ctx->host) / sizeof(ctx->host[0])) - 1] = L'\0';
            ctx->flags = flags;
            EndDialog(dialog, IDOK);
            return TRUE;
        }

        if (LOWORD(wparam) == IDCANCEL) {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        return FALSE;

    default:
        return FALSE;
    }
}

static int show_server_edit_dialog(HWND owner, server_edit_ctx_t *ctx)
{
    return (int)DialogBoxParamW(
        g_instance,
        MAKEINTRESOURCEW(IDD_SERVER_EDIT),
        owner,
        server_edit_dialog_proc,
        (LPARAM)ctx);
}

static void add_or_update_server(HWND dialog, int update)
{
    int row;
    server_edit_ctx_t ctx;

    if (update) {
        row = selected_row(dialog);
        if (row < 0 || row >= g_row_count) {
            MessageBoxW(dialog, L"Select a server row to update.", L"GW32TIME", MB_ICONWARNING);
            return;
        }
    } else {
        if (g_row_count >= SERVER_MAX_ROWS) {
            MessageBoxW(dialog, L"Server list is full.", L"GW32TIME", MB_ICONWARNING);
            return;
        }
        row = g_row_count++;
    }

    ZeroMemory(&ctx, sizeof(ctx));
    ctx.row = row;
    if (update) {
        wcsncpy(ctx.host, g_rows[row].host, (sizeof(ctx.host) / sizeof(ctx.host[0])) - 1);
        ctx.host[(sizeof(ctx.host) / sizeof(ctx.host[0])) - 1] = L'\0';
        ctx.flags = g_rows[row].flags;
    } else {
        ctx.flags = 0x9;
    }

    if (show_server_edit_dialog(dialog, &ctx) != IDOK) {
        if (!update) {
            g_row_count--;
        }
        return;
    }

    wcsncpy(g_rows[row].host, ctx.host, (sizeof(g_rows[row].host) / sizeof(g_rows[row].host[0])) - 1);
    g_rows[row].host[(sizeof(g_rows[row].host) / sizeof(g_rows[row].host[0])) - 1] = L'\0';
    g_rows[row].flags = ctx.flags;
    g_rows[row].has_probe = 0;
    g_rows[row].ip[0] = L'\0';
    g_rows[row].ptr[0] = L'\0';

    refresh_servers_table(dialog);
    start_probe_all_async(dialog);
}

static void delete_server(HWND dialog)
{
    int row = selected_row(dialog);
    int i;

    if (row < 0 || row >= g_row_count) {
        MessageBoxW(dialog, L"Select a server row to delete.", L"GW32TIME", MB_ICONWARNING);
        return;
    }

    for (i = row; i + 1 < g_row_count; i++) {
        g_rows[i] = g_rows[i + 1];
    }
    g_row_count--;
    refresh_servers_table(dialog);
}

static void apply_servers(HWND dialog)
{
    ntp_peer_list_t peers;
    wchar_t domain_label[320];
    wchar_t peerlist[1024];
    wchar_t warn[640];
    int is_admin = 0;
    domain_info_t domain;
    w32time_config_t config;
    w32tm_raw_result_t result;
    int i;

    if (g_row_count <= 0) {
        MessageBoxW(dialog, L"Add at least one server before apply.", L"GW32TIME", MB_ICONWARNING);
        return;
    }
    if (privilege_is_admin(&is_admin) != 0 || !is_admin) {
        relaunch_elevated_gui(dialog);
        return;
    }
    if (w32time_read_config(&config) != 0) {
        MessageBoxW(dialog, L"Could not read current W32Time configuration.", L"GW32TIME", MB_ICONERROR);
        return;
    }

    ZeroMemory(&domain, sizeof(domain));
    if (domain_query(&domain) == 0 && domain.joined) {
        if (domain.name[0] != L'\0') {
            _snwprintf(domain_label, sizeof(domain_label) / sizeof(domain_label[0]), L" (%ls)", domain.name);
            domain_label[(sizeof(domain_label) / sizeof(domain_label[0])) - 1] = L'\0';
        } else {
            domain_label[0] = L'\0';
        }

        if (_wcsicmp(config.type, L"NT5DS") == 0) {
            _snwprintf(
                warn,
                sizeof(warn) / sizeof(warn[0]),
                L"Domain-joined machine detected%ls.\n\nCurrent Type is NT5DS.\n"
                L"Manual NTP change can break Active Directory time hierarchy.\n\n"
                L"Apply manual NTP servers anyway?",
                domain_label);
        } else {
            _snwprintf(
                warn,
                sizeof(warn) / sizeof(warn[0]),
                L"Domain-joined machine detected%ls.\n\n"
                L"Manual NTP change may be overridden by policy or break domain time design.\n\n"
                L"Apply manual NTP servers anyway?",
                domain_label);
        }
        warn[(sizeof(warn) / sizeof(warn[0])) - 1] = L'\0';

        if (MessageBoxW(dialog, warn, L"GW32TIME", MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES) {
            return;
        }
    }

    ZeroMemory(&peers, sizeof(peers));
    peers.count = g_row_count;
    for (i = 0; i < g_row_count; i++) {
        wcsncpy(peers.peers[i].host, g_rows[i].host, (sizeof(peers.peers[i].host) / sizeof(peers.peers[i].host[0])) - 1);
        peers.peers[i].host[(sizeof(peers.peers[i].host) / sizeof(peers.peers[i].host[0])) - 1] = L'\0';
        peers.peers[i].flags = g_rows[i].flags;
        peers.peers[i].enabled = 1;
    }

    if (ntp_format_peer_list(&peers, peerlist, sizeof(peerlist) / sizeof(peerlist[0])) != 0) {
        MessageBoxW(dialog, L"Could not format server list.", L"GW32TIME", MB_ICONERROR);
        return;
    }

    if (w32time_write_manual_servers(peerlist) != 0 ||
        w32tm_config_manual_peers_raw(peerlist, &result) != 0 ||
        result.exit_code != 0 ||
        svc_restart(L"w32time") != 0) {
        MessageBoxW(dialog, L"Failed to apply server list.", L"GW32TIME", MB_ICONERROR);
        return;
    }

    MessageBoxW(dialog, L"Server list applied.", L"GW32TIME", MB_ICONINFORMATION);
    refresh_status(dialog);
}

static DWORD WINAPI probe_all_thread_proc(LPVOID param)
{
    probe_run_ctx_t *ctx = (probe_run_ctx_t *)param;
    int i;

    if (ctx == NULL) {
        return 0;
    }

    for (i = 0; i < ctx->count; i++) {
        probe_result_msg_t *msg = (probe_result_msg_t *)malloc(sizeof(probe_result_msg_t));
        if (msg == NULL) {
            continue;
        }

        ZeroMemory(msg, sizeof(*msg));
        wcsncpy(msg->host, ctx->rows[i].host, (sizeof(msg->host) / sizeof(msg->host[0])) - 1);
        msg->host[(sizeof(msg->host) / sizeof(msg->host[0])) - 1] = L'\0';
        msg->flags = ctx->rows[i].flags;

        ctx->rows[i].has_probe = 0;
        ctx->rows[i].ip[0] = L'\0';
        ctx->rows[i].ptr[0] = L'\0';
        if (resolve_ip_ptr(
                ctx->rows[i].host,
                ctx->rows[i].ip,
                sizeof(ctx->rows[i].ip) / sizeof(ctx->rows[i].ip[0]),
                ctx->rows[i].ptr,
                sizeof(ctx->rows[i].ptr) / sizeof(ctx->rows[i].ptr[0])) != 0) {
            ctx->rows[i].ip[0] = L'\0';
            ctx->rows[i].ptr[0] = L'\0';
        }

        {
            ntp_probe_result_t result;
            if (ntp_probe(ctx->rows[i].host, 3000, &result) == 0 && result.ok) {
                ctx->rows[i].has_probe = 1;
                ctx->rows[i].stratum = result.stratum;
                ctx->rows[i].ping_ms = result.rtt_ms;
                ctx->rows[i].offset_ms = result.offset_ms;
            }
        }

        msg->has_probe = ctx->rows[i].has_probe;
        msg->stratum = ctx->rows[i].stratum;
        msg->ping_ms = ctx->rows[i].ping_ms;
        msg->offset_ms = ctx->rows[i].offset_ms;
        wcsncpy(msg->ip, ctx->rows[i].ip, (sizeof(msg->ip) / sizeof(msg->ip[0])) - 1);
        msg->ip[(sizeof(msg->ip) / sizeof(msg->ip[0])) - 1] = L'\0';
        wcsncpy(msg->ptr, ctx->rows[i].ptr, (sizeof(msg->ptr) / sizeof(msg->ptr[0])) - 1);
        msg->ptr[(sizeof(msg->ptr) / sizeof(msg->ptr[0])) - 1] = L'\0';
        PostMessageW(ctx->dialog, WM_APP_PROBE_RESULT, 0, (LPARAM)msg);
    }
    PostMessageW(ctx->dialog, WM_APP_PROBE_DONE, 0, (LPARAM)ctx);
    return 0;
}

static void start_probe_all_async(HWND dialog)
{
    probe_run_ctx_t *ctx;
    int i;

    if (InterlockedCompareExchange(&g_probe_running, 1, 0) != 0) {
        return;
    }

    if (g_row_count <= 0) {
        InterlockedExchange(&g_probe_running, 0);
        return;
    }

    EnableWindow(GetDlgItem(dialog, IDC_PROBE_ALL), FALSE);
    SetWindowTextW(GetDlgItem(dialog, IDC_PROBE_ALL), L"Checking...");

    ctx = (probe_run_ctx_t *)malloc(sizeof(probe_run_ctx_t));
    if (ctx == NULL) {
        InterlockedExchange(&g_probe_running, 0);
        SetWindowTextW(GetDlgItem(dialog, IDC_PROBE_ALL), L"Check Servers");
        EnableWindow(GetDlgItem(dialog, IDC_PROBE_ALL), TRUE);
        MessageBoxW(dialog, L"Could not allocate probe context.", L"GW32TIME", MB_ICONERROR);
        return;
    }

    ZeroMemory(ctx, sizeof(*ctx));
    ctx->dialog = dialog;
    ctx->count = g_row_count;
    for (i = 0; i < g_row_count; i++) {
        ctx->rows[i] = g_rows[i];
    }

    g_probe_thread = CreateThread(NULL, 0, probe_all_thread_proc, ctx, 0, NULL);
    if (g_probe_thread == NULL) {
        free(ctx);
        InterlockedExchange(&g_probe_running, 0);
        SetWindowTextW(GetDlgItem(dialog, IDC_PROBE_ALL), L"Check Servers");
        EnableWindow(GetDlgItem(dialog, IDC_PROBE_ALL), TRUE);
        MessageBoxW(dialog, L"Could not start background probe.", L"GW32TIME", MB_ICONERROR);
    }
}

static void finish_probe_all_async(HWND dialog)
{
    if (g_probe_thread != NULL) {
        CloseHandle(g_probe_thread);
        g_probe_thread = NULL;
    }
    InterlockedExchange(&g_probe_running, 0);
    SetWindowTextW(GetDlgItem(dialog, IDC_PROBE_ALL), L"Check Servers");
    EnableWindow(GetDlgItem(dialog, IDC_PROBE_ALL), TRUE);
}

static INT_PTR CALLBACK main_dialog_proc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam)
{
    (void)lparam;

    switch (message) {
    case WM_INITDIALOG:
        g_main_dialog = dialog;
        bump_main_window_layer(dialog);
        init_servers_table(dialog);
        SendDlgItemMessageW(dialog, IDC_REALTIME_SPIN, UDM_SETRANGE32, REALTIME_MIN_SECONDS, REALTIME_MAX_SECONDS);
        SendDlgItemMessageW(dialog, IDC_REALTIME_SPIN, UDM_SETBUDDY, (WPARAM)GetDlgItem(dialog, IDC_REALTIME_SECONDS), 0);
        SetDlgItemInt(dialog, IDC_REALTIME_SECONDS, (UINT)g_realtime_seconds, FALSE);
        CheckDlgButton(dialog, IDC_REALTIME_CHECK, BST_UNCHECKED);
        update_realtime_controls(dialog);
        if (ensure_bold_font() != NULL) {
            SendDlgItemMessageW(dialog, IDC_HEADER_TEXT, WM_SETFONT, (WPARAM)g_bold_font, TRUE);
            SendDlgItemMessageW(dialog, IDC_UAC_STATUS, WM_SETFONT, (WPARAM)g_bold_font, TRUE);
            SendDlgItemMessageW(dialog, IDC_CURRENT_TIME, WM_SETFONT, (WPARAM)g_bold_font, TRUE);
        }
        refresh_status(dialog);
        start_probe_all_async(dialog);
        SetTimer(dialog, TIMER_CLOCK, 1000, NULL);
        return TRUE;

    case WM_TIMER:
        if (wparam == TIMER_CLOCK) {
            refresh_datetime_block(dialog);
            return TRUE;
        }
        if (wparam == TIMER_REALTIME_CHECK) {
            start_probe_all_async(dialog);
            return TRUE;
        }
        return FALSE;

    case WM_ACTIVATE:
        if (LOWORD(wparam) != WA_INACTIVE) {
            bump_main_window_layer(dialog);
        }
        return TRUE;
    case WM_SHOWWINDOW:
        if (wparam) {
            bump_main_window_layer(dialog);
        }
        return TRUE;

    case WM_APP_PROBE_RESULT:
        if (lparam != 0) {
            probe_result_msg_t *msg = (probe_result_msg_t *)lparam;
            apply_probe_result(msg);
            free(msg);
        }
        return TRUE;

    case WM_APP_PROBE_DONE:
        if (lparam != 0) {
            free((probe_run_ctx_t *)lparam);
        }
        finish_probe_all_async(dialog);
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case IDC_SET_TIME:
            set_local_datetime(dialog);
            return TRUE;
        case IDC_SYNC:
            sync_now(dialog);
            return TRUE;
        case IDC_REALTIME_CHECK:
            update_realtime_controls(dialog);
            restart_realtime_timer(dialog);
            if (IsDlgButtonChecked(dialog, IDC_REALTIME_CHECK) == BST_CHECKED) {
                start_probe_all_async(dialog);
            }
            return TRUE;
        case IDC_BACKUP_MENU: {
            HMENU menu = CreatePopupMenu();
            RECT rc;
            POINT pt;
            UINT selected;
            if (menu == NULL) {
                return TRUE;
            }
            AppendMenuW(menu, MF_STRING, IDM_BACKUP_CONFIG, L"Backup Config...");
            AppendMenuW(menu, MF_STRING, IDM_RESTORE_CONFIG, L"Restore Config...");
            GetWindowRect(GetDlgItem(dialog, IDC_BACKUP_MENU), &rc);
            pt.x = rc.left;
            pt.y = rc.bottom;
            selected = TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD, pt.x, pt.y, 0, dialog, NULL);
            DestroyMenu(menu);
            if (selected == IDM_BACKUP_CONFIG) {
                backup_config(dialog);
            } else if (selected == IDM_RESTORE_CONFIG) {
                restore_config(dialog);
            }
            return TRUE;
        }
        case IDC_ADD_SERVER:
            add_or_update_server(dialog, 0);
            return TRUE;
        case IDC_UPDATE_SERVER:
            add_or_update_server(dialog, 1);
            return TRUE;
        case IDC_DELETE_SERVER:
            delete_server(dialog);
            return TRUE;
        case IDC_APPLY_SERVERS:
            apply_servers(dialog);
            return TRUE;
        case IDC_PROBE_ALL:
            start_probe_all_async(dialog);
            return TRUE;
        case IDC_EXIT:
        case IDCANCEL:
            if (g_probe_running != 0) {
                MessageBoxW(dialog, L"Probe is still running in background.", L"GW32TIME", MB_ICONINFORMATION);
                return TRUE;
            }
            KillTimer(dialog, TIMER_CLOCK);
            KillTimer(dialog, TIMER_REALTIME_CHECK);
            EndDialog(dialog, 0);
            return TRUE;
        default:
            if (LOWORD(wparam) == IDC_REALTIME_SECONDS && HIWORD(wparam) == EN_CHANGE) {
                if (g_realtime_updating) {
                    return TRUE;
                }
                update_realtime_controls(dialog);
                restart_realtime_timer(dialog);
                return TRUE;
            }
            if (LOWORD(wparam) == IDC_REALTIME_SECONDS && HIWORD(wparam) == EN_KILLFOCUS) {
                update_realtime_controls(dialog);
                restart_realtime_timer(dialog);
                return TRUE;
            }
            return FALSE;
        }
    default:
        return FALSE;
    }
}

int gui_launch(HINSTANCE instance)
{
    INITCOMMONCONTROLSEX icc;
    g_instance = instance;

    ZeroMemory(&icc, sizeof(icc));
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_UPDOWN_CLASS;
    InitCommonControlsEx(&icc);

    return (int)DialogBoxParamW(g_instance, MAKEINTRESOURCEW(IDD_MAIN), NULL, main_dialog_proc, 0);
}
