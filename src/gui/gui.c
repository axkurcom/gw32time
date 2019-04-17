#include <winsock2.h>
#include <ws2tcpip.h>

#include "gui.h"
#include <commctrl.h>
#include <commdlg.h>
#include <stdio.h>
#include <string.h>

#include "resource.h"
#include "../core/config_file.h"
#include "../core/diagnostics.h"
#include "../core/domain.h"
#include "../core/ntp_probe.h"
#include "../core/privilege.h"
#include "../core/service.h"
#include "../core/w32time.h"
#include "../core/w32tm.h"

#define SERVER_MAX_ROWS NTP_MAX_PEERS
#define FLAG_MASK_VALID 0x0f
#define WM_APP_PROBE_ROW (WM_APP + 1)
#define WM_APP_PROBE_DONE (WM_APP + 2)

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

static HINSTANCE g_instance;
static HWND g_main_dialog = NULL;
static HANDLE g_probe_thread = NULL;
static LONG g_probe_running = 0;
static server_row_t g_rows[SERVER_MAX_ROWS];
static int g_row_count = 0;
static int selected_row(HWND dialog);
static void start_probe_all_async(HWND dialog);

static void set_text(HWND dialog, int id, const wchar_t *text)
{
    SetDlgItemTextW(dialog, id, text != NULL && text[0] != L'\0' ? text : L"unknown");
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

static void probe_server_row(int index)
{
    ntp_probe_result_t result;

    g_rows[index].has_probe = 0;
    g_rows[index].ip[0] = L'\0';
    g_rows[index].ptr[0] = L'\0';
    if (resolve_ip_ptr(
            g_rows[index].host,
            g_rows[index].ip,
            sizeof(g_rows[index].ip) / sizeof(g_rows[index].ip[0]),
            g_rows[index].ptr,
            sizeof(g_rows[index].ptr) / sizeof(g_rows[index].ptr[0])) != 0) {
        g_rows[index].ip[0] = L'\0';
        g_rows[index].ptr[0] = L'\0';
    }

    if (ntp_probe(g_rows[index].host, 3000, &result) == 0 && result.ok) {
        g_rows[index].has_probe = 1;
        g_rows[index].stratum = result.stratum;
        g_rows[index].ping_ms = result.rtt_ms;
        g_rows[index].offset_ms = result.offset_ms;
    }
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

static void test_server(HWND dialog)
{
    wchar_t message[512];
    ntp_probe_result_t result;
    int row = selected_row(dialog);

    if (row < 0 || row >= g_row_count) {
        MessageBoxW(dialog, L"Select a server row to test.", L"GW32TIME", MB_ICONWARNING);
        return;
    }

    if (ntp_probe(g_rows[row].host, 3000, &result) != 0 || !result.ok) {
        _snwprintf(
            message,
            sizeof(message) / sizeof(message[0]),
            L"NTP probe failed: %ls",
            result.error[0] ? result.error : L"unknown error");
        message[(sizeof(message) / sizeof(message[0])) - 1] = L'\0';
        MessageBoxW(dialog, message, L"GW32TIME", MB_ICONERROR);
        return;
    }

    _snwprintf(
        message,
        sizeof(message) / sizeof(message[0]),
        L"NTP probe OK.\nRTT: %lu ms\nOffset: %.0f ms\nStratum: %d",
        (unsigned long)result.rtt_ms,
        result.offset_ms,
        result.stratum);
    message[(sizeof(message) / sizeof(message[0])) - 1] = L'\0';
    MessageBoxW(dialog, message, L"GW32TIME", MB_ICONINFORMATION);
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
        MessageBoxW(dialog, L"Applying servers requires administrator privileges.", L"GW32TIME", MB_ICONWARNING);
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
    HWND dialog = (HWND)param;
    int i;

    for (i = 0; i < g_row_count; i++) {
        probe_server_row(i);
        PostMessageW(dialog, WM_APP_PROBE_ROW, (WPARAM)i, 0);
    }
    PostMessageW(dialog, WM_APP_PROBE_DONE, 0, 0);
    return 0;
}

static void start_probe_all_async(HWND dialog)
{
    if (InterlockedCompareExchange(&g_probe_running, 1, 0) != 0) {
        return;
    }

    if (g_row_count <= 0) {
        InterlockedExchange(&g_probe_running, 0);
        return;
    }

    EnableWindow(GetDlgItem(dialog, IDC_PROBE_ALL), FALSE);
    EnableWindow(GetDlgItem(dialog, IDC_APPLY_SERVERS), FALSE);
    EnableWindow(GetDlgItem(dialog, IDC_ADD_SERVER), FALSE);
    EnableWindow(GetDlgItem(dialog, IDC_UPDATE_SERVER), FALSE);
    EnableWindow(GetDlgItem(dialog, IDC_DELETE_SERVER), FALSE);
    SetWindowTextW(GetDlgItem(dialog, IDC_PROBE_ALL), L"Probing...");

    g_probe_thread = CreateThread(NULL, 0, probe_all_thread_proc, dialog, 0, NULL);
    if (g_probe_thread == NULL) {
        InterlockedExchange(&g_probe_running, 0);
        SetWindowTextW(GetDlgItem(dialog, IDC_PROBE_ALL), L"Probe all");
        EnableWindow(GetDlgItem(dialog, IDC_DELETE_SERVER), TRUE);
        EnableWindow(GetDlgItem(dialog, IDC_UPDATE_SERVER), TRUE);
        EnableWindow(GetDlgItem(dialog, IDC_ADD_SERVER), TRUE);
        EnableWindow(GetDlgItem(dialog, IDC_APPLY_SERVERS), TRUE);
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
    SetWindowTextW(GetDlgItem(dialog, IDC_PROBE_ALL), L"Probe all");
    EnableWindow(GetDlgItem(dialog, IDC_DELETE_SERVER), TRUE);
    EnableWindow(GetDlgItem(dialog, IDC_UPDATE_SERVER), TRUE);
    EnableWindow(GetDlgItem(dialog, IDC_ADD_SERVER), TRUE);
    EnableWindow(GetDlgItem(dialog, IDC_APPLY_SERVERS), TRUE);
    EnableWindow(GetDlgItem(dialog, IDC_PROBE_ALL), TRUE);
}

static INT_PTR CALLBACK main_dialog_proc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam)
{
    (void)lparam;

    switch (message) {
    case WM_INITDIALOG:
        g_main_dialog = dialog;
        init_servers_table(dialog);
        refresh_status(dialog);
        start_probe_all_async(dialog);
        return TRUE;

    case WM_APP_PROBE_ROW:
        if ((int)wparam >= 0 && (int)wparam < g_row_count) {
            server_row_to_table(GetDlgItem(dialog, IDC_SERVERS_TABLE), (int)wparam);
        }
        return TRUE;

    case WM_APP_PROBE_DONE:
        finish_probe_all_async(dialog);
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
        case IDC_TEST:
            test_server(dialog);
            return TRUE;
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
    INITCOMMONCONTROLSEX icc;
    g_instance = instance;

    ZeroMemory(&icc, sizeof(icc));
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    return (int)DialogBoxParamW(g_instance, MAKEINTRESOURCEW(IDD_MAIN), NULL, main_dialog_proc, 0);
}
