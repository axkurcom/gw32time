#include <winsock2.h>
#include <ws2tcpip.h>

#include "gui.h"
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <wincrypt.h>
#include <aclapi.h>

#include "resource.h"
#include "../core/config_file.h"
#include "../core/diagnostics.h"
#include "../core/domain.h"
#include "../core/ntp/ntp_checker.h"
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
#define IDM_SERVICE_START 50010
#define IDM_SERVICE_STOP 50011
#define IDM_SERVICE_RESTART 50012
#define IDM_SERVICE_MODE_AUTO 50013
#define IDM_SERVICE_MODE_MANUAL 50014
#define IDM_SERVICE_MODE_DELAYED 50015
#define IDM_SERVICE_MODE_DISABLED 50016
#define TIMER_CLOCK 1
#define TIMER_REALTIME_CHECK 2
#define TIMER_SYNC_BURST 3
#define REALTIME_MIN_SECONDS 1
#define REALTIME_MAX_SECONDS 3600
#define HELPER_PROTO_MAX_FIELD_CHARS 1024
#define HELPER_EXIT_REPLY_TIMEOUT_MS 800

enum {
    HELPER_OP_EXIT = 0,
    HELPER_OP_UAC_PING = 1,
    HELPER_OP_SYNC_NOW = 2,
    HELPER_OP_SET_TIME = 3,
    HELPER_OP_APPLY_SERVERS = 4,
    HELPER_OP_RESTORE_CONFIG = 5,
    HELPER_OP_SERVICE = 6,
    HELPER_OP_SET_POLL = 7
};

typedef struct {
    DWORD opcode;
    DWORD arg1_chars;
    DWORD arg2_chars;
} helper_frame_header_t;

typedef struct {
    wchar_t host[256];
    DWORD flags;
    int has_probe;
    int success_samples;
    int total_samples;
    int stratum;
    DWORD ping_ms;
    double delay_ms;
    double delay_min_ms;
    double offset_ms;
    double offset_mean_ms;
    double offset_stddev_ms;
    double offset_mad_ms;
    double jitter_ms;
    double score;
    int last_error;
    char last_reference_id[8];
    wchar_t ip[64];
    wchar_t ptr[256];
} server_row_t;

typedef struct {
    int row;
    wchar_t host[256];
    DWORD flags;
    int allow_delete;
    int delete_requested;
} server_edit_ctx_t;

typedef struct {
    SYSTEMTIME selected;
    int has_selected;
    int year_locked;
    int month_locked;
    int day_locked;
    int hour_locked;
    int minute_locked;
    int second_locked;
    int refreshing;
} set_time_dialog_ctx_t;

typedef struct {
    wchar_t host[256];
    DWORD flags;
    int has_probe;
    int success_samples;
    int total_samples;
    int stratum;
    DWORD ping_ms;
    double delay_ms;
    double delay_min_ms;
    double offset_ms;
    double offset_mean_ms;
    double offset_stddev_ms;
    double offset_mad_ms;
    double jitter_ms;
    double score;
    int last_error;
    char last_reference_id[8];
    wchar_t ip[64];
    wchar_t ptr[256];
} probe_result_msg_t;

typedef struct {
    HWND dialog;
    HANDLE cancel_event;
    int count;
    server_row_t rows[SERVER_MAX_ROWS];
} probe_run_ctx_t;

static HINSTANCE g_instance;
static HWND g_main_dialog = NULL;
static HANDLE g_probe_thread = NULL;
static probe_run_ctx_t *g_probe_ctx = NULL;
static HANDLE g_helper_pipe = INVALID_HANDLE_VALUE;
static HANDLE g_helper_process = NULL;
static LONG g_probe_running = 0;
static LONG g_probe_shutdown_requested = 0;
static int g_is_admin = 0;
static int g_helper_uac_ok = 0;
static HFONT g_bold_font = NULL;
static int g_realtime_seconds = 2;
static int g_realtime_updating = 0;
static int g_sync_burst_remaining = 0;
static int g_poll_updating = 0;
static server_row_t g_rows[SERVER_MAX_ROWS];
static int g_row_count = 0;
static int selected_row(HWND dialog);
static void start_probe_all_async(HWND dialog);
static void update_admin_controls(HWND dialog);
static void layout_header_time(HWND dialog);
static void update_realtime_controls(HWND dialog);
static void restart_realtime_timer(HWND dialog);
static int parse_realtime_seconds(HWND dialog);
static void apply_probe_result(const probe_result_msg_t *msg);
static void bump_main_window_layer(HWND dialog);
static int run_elevated_helper(HWND dialog, DWORD opcode, const wchar_t *arg1, const wchar_t *arg2);
static int run_service_action(HWND dialog, const wchar_t *action, const wchar_t *mode);
static void trigger_sync_probe_burst(HWND dialog);
static void refresh_service_runtime(HWND dialog);
static void apply_poll_interval(HWND dialog);
static void close_elevated_helper(void);
static void drain_probe_messages(HWND dialog);
static void request_probe_cancel(void);
static void wait_probe_thread_briefly(void);
static int random_bytes(unsigned char *buf, DWORD size);
static int current_user_pipe_sa(SECURITY_ATTRIBUTES *sa, PACL *acl_out, PSID *sid_out);
static int verify_helper_pipe_client(HANDLE pipe, HANDLE helper_process);

static int random_bytes(unsigned char *buf, DWORD size)
{
    HCRYPTPROV prov = 0;

    if (buf == NULL || size == 0) {
        return -1;
    }
    if (!CryptAcquireContextW(&prov, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT | CRYPT_SILENT)) {
        return -1;
    }
    if (!CryptGenRandom(prov, size, buf)) {
        CryptReleaseContext(prov, 0);
        return -1;
    }
    CryptReleaseContext(prov, 0);
    return 0;
}

static int current_user_pipe_sa(SECURITY_ATTRIBUTES *sa, PACL *acl_out, PSID *sid_out)
{
    HANDLE token = NULL;
    DWORD token_size = 0;
    TOKEN_USER *token_user = NULL;
    EXPLICIT_ACCESSW ea;
    PACL acl = NULL;
    PSID sid = NULL;
    DWORD rc;

    if (sa == NULL || acl_out == NULL || sid_out == NULL) {
        return -1;
    }
    *acl_out = NULL;
    *sid_out = NULL;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return -1;
    }
    GetTokenInformation(token, TokenUser, NULL, 0, &token_size);
    if (token_size == 0) {
        CloseHandle(token);
        return -1;
    }
    token_user = (TOKEN_USER *)malloc(token_size);
    if (token_user == NULL) {
        CloseHandle(token);
        SetLastError(ERROR_OUTOFMEMORY);
        return -1;
    }
    if (!GetTokenInformation(token, TokenUser, token_user, token_size, &token_size)) {
        free(token_user);
        CloseHandle(token);
        return -1;
    }
    sid = (PSID)malloc(GetLengthSid(token_user->User.Sid));
    if (sid == NULL) {
        free(token_user);
        CloseHandle(token);
        SetLastError(ERROR_OUTOFMEMORY);
        return -1;
    }
    if (!CopySid(GetLengthSid(token_user->User.Sid), sid, token_user->User.Sid)) {
        free(sid);
        free(token_user);
        CloseHandle(token);
        return -1;
    }
    ZeroMemory(&ea, sizeof(ea));
    ea.grfAccessPermissions = GENERIC_ALL;
    ea.grfAccessMode = SET_ACCESS;
    ea.grfInheritance = NO_INHERITANCE;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType = TRUSTEE_IS_USER;
    ea.Trustee.ptstrName = (LPWSTR)sid;
    rc = SetEntriesInAclW(1, &ea, NULL, &acl);
    free(token_user);
    CloseHandle(token);
    if (rc != ERROR_SUCCESS) {
        free(sid);
        SetLastError(rc);
        return -1;
    }

    ZeroMemory(sa, sizeof(*sa));
    sa->nLength = sizeof(*sa);
    sa->bInheritHandle = FALSE;
    sa->lpSecurityDescriptor = LocalAlloc(LPTR, SECURITY_DESCRIPTOR_MIN_LENGTH);
    if (sa->lpSecurityDescriptor == NULL) {
        LocalFree(acl);
        free(sid);
        SetLastError(ERROR_OUTOFMEMORY);
        return -1;
    }
    if (!InitializeSecurityDescriptor(sa->lpSecurityDescriptor, SECURITY_DESCRIPTOR_REVISION) ||
        !SetSecurityDescriptorDacl(sa->lpSecurityDescriptor, TRUE, acl, FALSE)) {
        LocalFree(sa->lpSecurityDescriptor);
        sa->lpSecurityDescriptor = NULL;
        LocalFree(acl);
        free(sid);
        return -1;
    }

    *acl_out = acl;
    *sid_out = sid;
    return 0;
}

static int verify_helper_pipe_client(HANDLE pipe, HANDLE helper_process)
{
    HMODULE k32;
    BOOL(WINAPI *fn_get_client_pid)(HANDLE, PULONG);
    ULONG client_pid = 0;
    DWORD helper_pid;

    if (pipe == NULL || pipe == INVALID_HANDLE_VALUE || helper_process == NULL) {
        return -1;
    }
    k32 = GetModuleHandleW(L"kernel32.dll");
    if (k32 == NULL) {
        return -1;
    }
    fn_get_client_pid = (BOOL(WINAPI *)(HANDLE, PULONG))GetProcAddress(k32, "GetNamedPipeClientProcessId");
    if (fn_get_client_pid == NULL) {
        SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
        return -1;
    }
    helper_pid = GetProcessId(helper_process);
    if (helper_pid == 0) {
        return -1;
    }
    if (!fn_get_client_pid(pipe, &client_pid)) {
        return -1;
    }
    if ((DWORD)client_pid != helper_pid) {
        SetLastError(ERROR_ACCESS_DENIED);
        return -1;
    }
    return 0;
}

static void set_text(HWND dialog, int id, const wchar_t *text)
{
    SetDlgItemTextW(dialog, id, text != NULL && text[0] != L'\0' ? text : L"unknown");
}

static int str_eq(const wchar_t *a, const wchar_t *b)
{
    return a != NULL && b != NULL && wcscmp(a, b) == 0;
}

static const wchar_t *ntp_error_label(int err)
{
    switch (err) {
    case GW_NTP_OK:
        return L"OK";
    case GW_NTP_ERR_DNS:
        return L"DNS";
    case GW_NTP_ERR_SOCKET:
        return L"SOCKET";
    case GW_NTP_ERR_TIMEOUT:
        return L"TIMEOUT";
    case GW_NTP_ERR_SHORT_PACKET:
        return L"SHORT";
    case GW_NTP_ERR_INVALID_RESPONSE:
        return L"INVALID";
    case GW_NTP_ERR_KISS_OF_DEATH:
        return L"KOD";
    default:
        return L"UNKNOWN";
    }
}

static int is_wspace(wchar_t ch)
{
    return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n';
}

static int utf8_from_wide(const wchar_t *input, char *out, size_t out_len)
{
    int written;

    if (input == NULL || out == NULL || out_len == 0) {
        return -1;
    }
    written = WideCharToMultiByte(CP_UTF8, 0, input, -1, out, (int)out_len, NULL, NULL);
    if (written <= 0) {
        return -1;
    }
    out[out_len - 1] = '\0';
    return 0;
}

static int trim_host_inplace(wchar_t *text)
{
    size_t start = 0;
    size_t end;
    size_t len;
    size_t out_len;

    if (text == NULL) {
        return 0;
    }

    len = wcslen(text);
    while (start < len && is_wspace(text[start])) {
        start++;
    }
    if (start == len) {
        text[0] = L'\0';
        return 0;
    }

    end = len;
    while (end > start && is_wspace(text[end - 1])) {
        end--;
    }

    out_len = end - start;
    if (start > 0 && out_len > 0) {
        memmove(text, text + start, out_len * sizeof(wchar_t));
    }
    text[out_len] = L'\0';
    return 1;
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

static DWORD parse_poll_seconds(HWND dialog, int *ok)
{
    wchar_t text[32];
    wchar_t *end = NULL;
    unsigned long value;

    if (ok != NULL) {
        *ok = 0;
    }

    GetDlgItemTextW(dialog, IDC_POLL_VALUE, text, sizeof(text) / sizeof(text[0]));
    value = wcstoul(text, &end, 10);
    if (end == text || *end != L'\0' || value == 0 || value > 0xffffffffUL) {
        return 0;
    }
    if (ok != NULL) {
        *ok = 1;
    }
    return (DWORD)value;
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
    _snwprintf(uac, sizeof(uac) / sizeof(uac[0]), L"%ls", (g_is_admin || g_helper_uac_ok) ? L"[UAC ✔]" : L"[UAC ✘]");
    uac[(sizeof(uac) / sizeof(uac[0])) - 1] = L'\0';
    set_text(dialog, IDC_UAC_STATUS, uac);
    update_admin_controls(dialog);
}

static void refresh_service_runtime(HWND dialog)
{
    svc_state_t state = SVC_STATE_UNKNOWN;
    svc_start_type_t start_type = SVC_START_UNKNOWN;

    if (svc_query_state(L"w32time", &state) == 0) {
        set_text(dialog, IDC_SERVICE, svc_state_name(state));
    } else {
        set_text(dialog, IDC_SERVICE, L"unknown");
    }
    if (svc_query_start_type(L"w32time", &start_type) == 0) {
        set_text(dialog, IDC_START, svc_start_type_name(start_type));
    } else {
        set_text(dialog, IDC_START, L"unknown");
    }
}

static void update_admin_controls(HWND dialog)
{
    EnableWindow(GetDlgItem(dialog, IDC_SET_TIME), TRUE);
    if (g_probe_running == 0) {
        EnableWindow(GetDlgItem(dialog, IDC_APPLY_SERVERS), TRUE);
    }
}

static int pipe_write_all(HANDLE pipe, const void *buf, DWORD size)
{
    const unsigned char *p = (const unsigned char *)buf;
    DWORD total = 0;

    while (total < size) {
        DWORD chunk = 0;
        if (!WriteFile(pipe, p + total, size - total, &chunk, NULL) || chunk == 0) {
            return -1;
        }
        total += chunk;
    }
    return 0;
}

static int pipe_read_all(HANDLE pipe, void *buf, DWORD size)
{
    unsigned char *p = (unsigned char *)buf;
    DWORD total = 0;

    while (total < size) {
        DWORD chunk = 0;
        if (!ReadFile(pipe, p + total, size - total, &chunk, NULL) || chunk == 0) {
            return -1;
        }
        total += chunk;
    }
    return 0;
}

static int send_helper_request(HANDLE pipe, DWORD opcode, const wchar_t *arg1, const wchar_t *arg2, DWORD *exit_code)
{
    helper_frame_header_t hdr;
    DWORD rc = 1;
    size_t arg1_len = (arg1 != NULL) ? wcslen(arg1) : 0;
    size_t arg2_len = (arg2 != NULL) ? wcslen(arg2) : 0;

    if (arg1_len > HELPER_PROTO_MAX_FIELD_CHARS || arg2_len > HELPER_PROTO_MAX_FIELD_CHARS) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }

    ZeroMemory(&hdr, sizeof(hdr));
    hdr.opcode = opcode;
    hdr.arg1_chars = (DWORD)arg1_len;
    hdr.arg2_chars = (DWORD)arg2_len;

    if (pipe_write_all(pipe, &hdr, sizeof(hdr)) != 0) {
        return -1;
    }
    if (hdr.arg1_chars > 0 &&
        pipe_write_all(pipe, arg1, hdr.arg1_chars * (DWORD)sizeof(wchar_t)) != 0) {
        return -1;
    }
    if (hdr.arg2_chars > 0 &&
        pipe_write_all(pipe, arg2, hdr.arg2_chars * (DWORD)sizeof(wchar_t)) != 0) {
        return -1;
    }
    if (pipe_read_all(pipe, &rc, sizeof(rc)) != 0) {
        return -1;
    }
    if (exit_code != NULL) {
        *exit_code = rc;
    }
    return 0;
}

static void try_send_helper_exit_nohang(void)
{
    helper_frame_header_t hdr;
    DWORD exit_code = 0;
    DWORD available = 0;
    DWORD waited = 0;

    if (g_helper_pipe == INVALID_HANDLE_VALUE) {
        return;
    }

    ZeroMemory(&hdr, sizeof(hdr));
    hdr.opcode = HELPER_OP_EXIT;
    if (pipe_write_all(g_helper_pipe, &hdr, sizeof(hdr)) != 0) {
        return;
    }

    while (waited < HELPER_EXIT_REPLY_TIMEOUT_MS) {
        if (PeekNamedPipe(g_helper_pipe, NULL, 0, NULL, &available, NULL) && available >= sizeof(exit_code)) {
            pipe_read_all(g_helper_pipe, &exit_code, sizeof(exit_code));
            return;
        }
        if (g_helper_process != NULL && WaitForSingleObject(g_helper_process, 0) == WAIT_OBJECT_0) {
            return;
        }
        Sleep(20);
        waited += 20;
    }
}

static void close_elevated_helper(void)
{
    if (g_helper_pipe != INVALID_HANDLE_VALUE) {
        try_send_helper_exit_nohang();
        CloseHandle(g_helper_pipe);
        g_helper_pipe = INVALID_HANDLE_VALUE;
    }
    if (g_helper_process != NULL) {
        CloseHandle(g_helper_process);
        g_helper_process = NULL;
    }
    g_helper_uac_ok = 0;
}

static int ensure_elevated_helper(HWND dialog)
{
    wchar_t exe_path[MAX_PATH];
    wchar_t pipe_name[160];
    wchar_t params[192];
    SECURITY_ATTRIBUTES sa;
    PACL pipe_acl = NULL;
    PSID pipe_sid = NULL;
    unsigned char nonce[8];
    SHELLEXECUTEINFOW sei;
    DWORD last_error = ERROR_SUCCESS;

    if (g_helper_pipe != INVALID_HANDLE_VALUE) {
        if (g_helper_process == NULL || WaitForSingleObject(g_helper_process, 0) != WAIT_OBJECT_0) {
            return 0;
        }
        close_elevated_helper();
    }
    if (g_helper_pipe != INVALID_HANDLE_VALUE) {
        return 0;
    }
    if (GetModuleFileNameW(NULL, exe_path, sizeof(exe_path) / sizeof(exe_path[0])) == 0) {
        return -1;
    }
    if (random_bytes(nonce, sizeof(nonce)) != 0) {
        return -1;
    }
    _snwprintf(
        pipe_name,
        sizeof(pipe_name) / sizeof(pipe_name[0]),
        L"\\\\.\\pipe\\gw32time-helper-%lu-%02X%02X%02X%02X%02X%02X%02X%02X",
        (unsigned long)GetCurrentProcessId(),
        (unsigned)nonce[0],
        (unsigned)nonce[1],
        (unsigned)nonce[2],
        (unsigned)nonce[3],
        (unsigned)nonce[4],
        (unsigned)nonce[5],
        (unsigned)nonce[6],
        (unsigned)nonce[7]);
    pipe_name[(sizeof(pipe_name) / sizeof(pipe_name[0])) - 1] = L'\0';
    _snwprintf(params, sizeof(params) / sizeof(params[0]), L"__helper \"%ls\"", pipe_name);
    params[(sizeof(params) / sizeof(params[0])) - 1] = L'\0';
    if (current_user_pipe_sa(&sa, &pipe_acl, &pipe_sid) != 0) {
        return -1;
    }

    g_helper_pipe = CreateNamedPipeW(
        pipe_name,
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        1,
        sizeof(DWORD),
        2048 * sizeof(wchar_t),
        0,
        &sa);
    LocalFree(sa.lpSecurityDescriptor);
    if (pipe_acl != NULL) {
        LocalFree(pipe_acl);
    }
    if (pipe_sid != NULL) {
        free(pipe_sid);
    }
    if (g_helper_pipe == INVALID_HANDLE_VALUE) {
        return -1;
    }

    ZeroMemory(&sei, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd = dialog;
    sei.lpVerb = L"runas";
    sei.lpFile = exe_path;
    sei.lpParameters = params;
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) {
        last_error = GetLastError();
        CloseHandle(g_helper_pipe);
        g_helper_pipe = INVALID_HANDLE_VALUE;
        SetLastError(last_error);
        if (last_error == ERROR_CANCELLED) {
            return -2;
        }
        return -1;
    }
    if (sei.hProcess == NULL) {
        CloseHandle(g_helper_pipe);
        g_helper_pipe = INVALID_HANDLE_VALUE;
        return -1;
    }

    if (!ConnectNamedPipe(g_helper_pipe, NULL)) {
        last_error = GetLastError();
        if (last_error != ERROR_PIPE_CONNECTED) {
            CloseHandle(g_helper_pipe);
            g_helper_pipe = INVALID_HANDLE_VALUE;
            CloseHandle(sei.hProcess);
            SetLastError(last_error);
            return -1;
        }
    }
    if (verify_helper_pipe_client(g_helper_pipe, sei.hProcess) != 0) {
        last_error = GetLastError();
        CloseHandle(g_helper_pipe);
        g_helper_pipe = INVALID_HANDLE_VALUE;
        CloseHandle(sei.hProcess);
        SetLastError(last_error);
        return -1;
    }
    g_helper_process = sei.hProcess;
    g_helper_uac_ok = 1;
    return 0;
}

static int run_elevated_helper(HWND dialog, DWORD opcode, const wchar_t *arg1, const wchar_t *arg2)
{
    DWORD exit_code = 1;

    if (ensure_elevated_helper(dialog) != 0) {
        return -1;
    }

    if (send_helper_request(g_helper_pipe, opcode, arg1, arg2, &exit_code) != 0) {
        close_elevated_helper();
        if (ensure_elevated_helper(dialog) != 0) {
            return -1;
        }
        if (send_helper_request(g_helper_pipe, opcode, arg1, arg2, &exit_code) != 0) {
            close_elevated_helper();
            return -1;
        }
    }
    return (int)exit_code;
}

static int run_elevated_set_time(HWND dialog, const SYSTEMTIME *st)
{
    wchar_t date_arg[32];
    wchar_t time_arg[32];

    if (st == NULL) {
        return -1;
    }
    _snwprintf(date_arg, sizeof(date_arg) / sizeof(date_arg[0]), L"%04u-%02u-%02u", (unsigned)st->wYear, (unsigned)st->wMonth, (unsigned)st->wDay);
    date_arg[(sizeof(date_arg) / sizeof(date_arg[0])) - 1] = L'\0';
    _snwprintf(time_arg, sizeof(time_arg) / sizeof(time_arg[0]), L"%02u:%02u:%02u", (unsigned)st->wHour, (unsigned)st->wMinute, (unsigned)st->wSecond);
    time_arg[(sizeof(time_arg) / sizeof(time_arg[0])) - 1] = L'\0';
    return run_elevated_helper(dialog, HELPER_OP_SET_TIME, date_arg, time_arg);
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

static void set_time_field(HWND dialog, int id, WORD value)
{
    wchar_t text[16];

    if (id == IDC_SET_YEAR_VALUE) {
        _snwprintf(text, sizeof(text) / sizeof(text[0]), L"%04u", (unsigned)value);
    } else {
        _snwprintf(text, sizeof(text) / sizeof(text[0]), L"%02u", (unsigned)value);
    }
    text[(sizeof(text) / sizeof(text[0])) - 1] = L'\0';
    SetDlgItemTextW(dialog, id, text);
}

static int read_time_field(HWND dialog, int id, WORD min_value, WORD max_value, WORD *out)
{
    BOOL translated = FALSE;
    UINT value = GetDlgItemInt(dialog, id, &translated, FALSE);

    if (!translated || value < min_value || value > max_value || out == NULL) {
        return -1;
    }
    *out = (WORD)value;
    return 0;
}

static int normalize_time_field(HWND dialog, int id)
{
    WORD value;

    if (id == IDC_SET_YEAR_VALUE) {
        if (read_time_field(dialog, id, 1601, 9999, &value) != 0) {
            return -1;
        }
    } else if (id == IDC_SET_MONTH_VALUE) {
        if (read_time_field(dialog, id, 1, 12, &value) != 0) {
            return -1;
        }
    } else if (id == IDC_SET_DAY_VALUE) {
        if (read_time_field(dialog, id, 1, 31, &value) != 0) {
            return -1;
        }
    } else if (id == IDC_SET_HOUR_VALUE) {
        if (read_time_field(dialog, id, 0, 23, &value) != 0) {
            return -1;
        }
    } else if (id == IDC_SET_MINUTE_VALUE || id == IDC_SET_SECOND_VALUE) {
        if (read_time_field(dialog, id, 0, 59, &value) != 0) {
            return -1;
        }
    } else {
        return -1;
    }
    set_time_field(dialog, id, value);
    return 0;
}

static void set_time_dialog_values(HWND dialog, const SYSTEMTIME *st, set_time_dialog_ctx_t *ctx)
{
    if (st == NULL) {
        return;
    }
    if (ctx != NULL) {
        ctx->refreshing = 1;
    }
    set_time_field(dialog, IDC_SET_YEAR_VALUE, st->wYear);
    set_time_field(dialog, IDC_SET_MONTH_VALUE, st->wMonth);
    set_time_field(dialog, IDC_SET_DAY_VALUE, st->wDay);
    set_time_field(dialog, IDC_SET_HOUR_VALUE, st->wHour);
    set_time_field(dialog, IDC_SET_MINUTE_VALUE, st->wMinute);
    set_time_field(dialog, IDC_SET_SECOND_VALUE, st->wSecond);
    if (ctx != NULL) {
        ctx->refreshing = 0;
    }
}

static int read_set_time_dialog_values(HWND dialog, SYSTEMTIME *out)
{
    FILETIME ft;

    if (out == NULL) {
        return -1;
    }
    ZeroMemory(out, sizeof(*out));
    if (read_time_field(dialog, IDC_SET_YEAR_VALUE, 1601, 9999, &out->wYear) != 0 ||
        read_time_field(dialog, IDC_SET_MONTH_VALUE, 1, 12, &out->wMonth) != 0 ||
        read_time_field(dialog, IDC_SET_DAY_VALUE, 1, 31, &out->wDay) != 0 ||
        read_time_field(dialog, IDC_SET_HOUR_VALUE, 0, 23, &out->wHour) != 0 ||
        read_time_field(dialog, IDC_SET_MINUTE_VALUE, 0, 59, &out->wMinute) != 0 ||
        read_time_field(dialog, IDC_SET_SECOND_VALUE, 0, 59, &out->wSecond) != 0) {
        return -1;
    }
    out->wMilliseconds = 0;
    if (!SystemTimeToFileTime(out, &ft)) {
        return -1;
    }
    return 0;
}

static INT_PTR CALLBACK set_time_dialog_proc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam)
{
    set_time_dialog_ctx_t *ctx = (set_time_dialog_ctx_t *)GetWindowLongPtrW(dialog, GWLP_USERDATA);

    switch (message) {
    case WM_INITDIALOG: {
        SYSTEMTIME st;
        HFONT bold_font;

        SetWindowLongPtrW(dialog, GWLP_USERDATA, lparam);
        ctx = (set_time_dialog_ctx_t *)lparam;
        GetLocalTime(&st);
        set_time_dialog_values(dialog, &st, ctx);
        SetTimer(dialog, TIMER_CLOCK, 1000, NULL);
        if (ctx != NULL) {
            ctx->year_locked = 0;
            ctx->month_locked = 0;
            ctx->day_locked = 0;
            ctx->hour_locked = 0;
            ctx->minute_locked = 0;
            ctx->second_locked = 0;
            ctx->refreshing = 0;
        }

        bold_font = ensure_bold_font();
        if (bold_font != NULL) {
            SendDlgItemMessageW(dialog, IDC_SET_YEAR_VALUE, WM_SETFONT, (WPARAM)bold_font, TRUE);
            SendDlgItemMessageW(dialog, IDC_SET_MONTH_VALUE, WM_SETFONT, (WPARAM)bold_font, TRUE);
            SendDlgItemMessageW(dialog, IDC_SET_DAY_VALUE, WM_SETFONT, (WPARAM)bold_font, TRUE);
            SendDlgItemMessageW(dialog, IDC_SET_HOUR_VALUE, WM_SETFONT, (WPARAM)bold_font, TRUE);
            SendDlgItemMessageW(dialog, IDC_SET_MINUTE_VALUE, WM_SETFONT, (WPARAM)bold_font, TRUE);
            SendDlgItemMessageW(dialog, IDC_SET_SECOND_VALUE, WM_SETFONT, (WPARAM)bold_font, TRUE);
        }
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wparam) == IDC_SET_TIME_REFRESH) {
            SYSTEMTIME st;
            GetLocalTime(&st);
            if (ctx != NULL) {
                ctx->year_locked = 0;
                ctx->month_locked = 0;
                ctx->day_locked = 0;
                ctx->hour_locked = 0;
                ctx->minute_locked = 0;
                ctx->second_locked = 0;
            }
            set_time_dialog_values(dialog, &st, ctx);
            return TRUE;
        }
        if (ctx != NULL && !ctx->refreshing && HIWORD(wparam) == EN_CHANGE) {
            if (LOWORD(wparam) == IDC_SET_YEAR_VALUE) {
                ctx->year_locked = 1;
            } else if (LOWORD(wparam) == IDC_SET_MONTH_VALUE) {
                ctx->month_locked = 1;
            } else if (LOWORD(wparam) == IDC_SET_DAY_VALUE) {
                ctx->day_locked = 1;
            } else if (LOWORD(wparam) == IDC_SET_HOUR_VALUE) {
                ctx->hour_locked = 1;
            } else if (LOWORD(wparam) == IDC_SET_MINUTE_VALUE) {
                ctx->minute_locked = 1;
            } else if (LOWORD(wparam) == IDC_SET_SECOND_VALUE) {
                ctx->second_locked = 1;
            }
        }
        if (HIWORD(wparam) == EN_KILLFOCUS) {
            normalize_time_field(dialog, LOWORD(wparam));
        }
        if (LOWORD(wparam) == IDOK) {
            SYSTEMTIME selected;

            if (read_set_time_dialog_values(dialog, &selected) != 0) {
                MessageBoxW(dialog, L"Invalid date or time value.", L"GW32TIME", MB_ICONWARNING);
                return TRUE;
            }
            set_time_dialog_values(dialog, &selected, ctx);
            if (ctx != NULL) {
                ctx->selected = selected;
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
            SYSTEMTIME st;

            GetLocalTime(&st);
            if (ctx != NULL) {
                ctx->refreshing = 1;
            }
            if (ctx == NULL || !ctx->year_locked) {
                set_time_field(dialog, IDC_SET_YEAR_VALUE, st.wYear);
            }
            if (ctx == NULL || !ctx->month_locked) {
                set_time_field(dialog, IDC_SET_MONTH_VALUE, st.wMonth);
            }
            if (ctx == NULL || !ctx->day_locked) {
                set_time_field(dialog, IDC_SET_DAY_VALUE, st.wDay);
            }
            if (ctx == NULL || !ctx->hour_locked) {
                set_time_field(dialog, IDC_SET_HOUR_VALUE, st.wHour);
            }
            if (ctx == NULL || !ctx->minute_locked) {
                set_time_field(dialog, IDC_SET_MINUTE_VALUE, st.wMinute);
            }
            if (ctx == NULL || !ctx->second_locked) {
                set_time_field(dialog, IDC_SET_SECOND_VALUE, st.wSecond);
            }
            if (ctx != NULL) {
                ctx->refreshing = 0;
            }
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
    int is_admin = 0;

    ZeroMemory(&ctx, sizeof(ctx));

    if (privilege_is_admin(&is_admin) != 0 || !is_admin) {
        rc = ensure_elevated_helper(dialog);
        if (rc != 0) {
            bump_main_window_layer(dialog);
            return;
        }
        refresh_datetime_block(dialog);
    }

    rc = (int)DialogBoxParamW(g_instance, MAKEINTRESOURCEW(IDD_SET_TIME), dialog, set_time_dialog_proc, (LPARAM)&ctx);
    if (rc != IDOK || !ctx.has_selected) {
        bump_main_window_layer(dialog);
        return;
    }

    if (privilege_is_admin(&is_admin) == 0 && is_admin) {
        rc = time_set_local(&ctx.selected);
    } else {
        rc = run_elevated_set_time(dialog, &ctx.selected);
    }

    if (rc == 0) {
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
    const wchar_t *titles[] = {
        L"Server", L"Flags", L"Reach", L"Delay", L"Dmin", L"Offset", L"Mean", L"Stddev", L"MAD", L"Jitter", L"Score",
        L"Stratum", L"Valid", L"Reason", L"IP", L"PTR"
    };
    int widths[] = { 122, 112, 54, 58, 54, 58, 58, 58, 54, 56, 52, 48, 52, 76, 94, 130 };

    ListView_SetExtendedListViewStyle(table, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    for (i = 0; i < 16; i++) {
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
    wchar_t reason[64];

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
        _snwprintf(buf, sizeof(buf) / sizeof(buf[0]), L"%d/%d", g_rows[row_index].success_samples, g_rows[row_index].total_samples);
        buf[(sizeof(buf) / sizeof(buf[0])) - 1] = L'\0';
        ListView_SetItemText(table, row_index, 2, buf);
        _snwprintf(buf, sizeof(buf) / sizeof(buf[0]), L"%.0f ms", g_rows[row_index].delay_ms);
        buf[(sizeof(buf) / sizeof(buf[0])) - 1] = L'\0';
        ListView_SetItemText(table, row_index, 3, buf);
        _snwprintf(buf, sizeof(buf) / sizeof(buf[0]), L"%.0f ms", g_rows[row_index].delay_min_ms);
        buf[(sizeof(buf) / sizeof(buf[0])) - 1] = L'\0';
        ListView_SetItemText(table, row_index, 4, buf);
        _snwprintf(buf, sizeof(buf) / sizeof(buf[0]), L"%.0f ms", g_rows[row_index].offset_ms);
        buf[(sizeof(buf) / sizeof(buf[0])) - 1] = L'\0';
        ListView_SetItemText(table, row_index, 5, buf);
        _snwprintf(buf, sizeof(buf) / sizeof(buf[0]), L"%.1f", g_rows[row_index].offset_mean_ms);
        buf[(sizeof(buf) / sizeof(buf[0])) - 1] = L'\0';
        ListView_SetItemText(table, row_index, 6, buf);
        _snwprintf(buf, sizeof(buf) / sizeof(buf[0]), L"%.1f", g_rows[row_index].offset_stddev_ms);
        buf[(sizeof(buf) / sizeof(buf[0])) - 1] = L'\0';
        ListView_SetItemText(table, row_index, 7, buf);
        _snwprintf(buf, sizeof(buf) / sizeof(buf[0]), L"%.1f", g_rows[row_index].offset_mad_ms);
        buf[(sizeof(buf) / sizeof(buf[0])) - 1] = L'\0';
        ListView_SetItemText(table, row_index, 8, buf);
        _snwprintf(buf, sizeof(buf) / sizeof(buf[0]), L"%.1f ms", g_rows[row_index].jitter_ms);
        buf[(sizeof(buf) / sizeof(buf[0])) - 1] = L'\0';
        ListView_SetItemText(table, row_index, 9, buf);
        _snwprintf(buf, sizeof(buf) / sizeof(buf[0]), L"%.2f", g_rows[row_index].score);
        buf[(sizeof(buf) / sizeof(buf[0])) - 1] = L'\0';
        ListView_SetItemText(table, row_index, 10, buf);
        _snwprintf(buf, sizeof(buf) / sizeof(buf[0]), L"%d", g_rows[row_index].stratum);
        buf[(sizeof(buf) / sizeof(buf[0])) - 1] = L'\0';
        ListView_SetItemText(table, row_index, 11, buf);
        ListView_SetItemText(table, row_index, 12, L"yes");
        if (g_rows[row_index].last_error == GW_NTP_ERR_KISS_OF_DEATH && g_rows[row_index].last_reference_id[0] != '\0') {
            wchar_t refid[16];
            MultiByteToWideChar(CP_UTF8, 0, g_rows[row_index].last_reference_id, -1, refid, sizeof(refid) / sizeof(refid[0]));
            refid[(sizeof(refid) / sizeof(refid[0])) - 1] = L'\0';
            _snwprintf(reason, sizeof(reason) / sizeof(reason[0]), L"%ls(%ls)", ntp_error_label(g_rows[row_index].last_error), refid);
            reason[(sizeof(reason) / sizeof(reason[0])) - 1] = L'\0';
            ListView_SetItemText(table, row_index, 13, reason);
        } else {
            _snwprintf(reason, sizeof(reason) / sizeof(reason[0]), L"%ls", ntp_error_label(g_rows[row_index].last_error));
            reason[(sizeof(reason) / sizeof(reason[0])) - 1] = L'\0';
            ListView_SetItemText(table, row_index, 13, reason);
        }
    } else {
        ListView_SetItemText(table, row_index, 2, L"-");
        ListView_SetItemText(table, row_index, 3, L"-");
        ListView_SetItemText(table, row_index, 4, L"-");
        ListView_SetItemText(table, row_index, 5, L"-");
        ListView_SetItemText(table, row_index, 6, L"-");
        ListView_SetItemText(table, row_index, 7, L"-");
        ListView_SetItemText(table, row_index, 8, L"-");
        ListView_SetItemText(table, row_index, 9, L"-");
        ListView_SetItemText(table, row_index, 10, L"-");
        ListView_SetItemText(table, row_index, 11, L"-");
        ListView_SetItemText(table, row_index, 12, L"no");
        _snwprintf(reason, sizeof(reason) / sizeof(reason[0]), L"%ls", ntp_error_label(g_rows[row_index].last_error));
        reason[(sizeof(reason) / sizeof(reason[0])) - 1] = L'\0';
        ListView_SetItemText(table, row_index, 13, reason);
    }

    ListView_SetItemText(table, row_index, 14, g_rows[row_index].ip[0] ? g_rows[row_index].ip : L"-");
    ListView_SetItemText(table, row_index, 15, g_rows[row_index].ptr[0] ? g_rows[row_index].ptr : L"-");
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
            g_rows[i].success_samples = msg->success_samples;
            g_rows[i].total_samples = msg->total_samples;
            g_rows[i].stratum = msg->stratum;
            g_rows[i].ping_ms = msg->ping_ms;
            g_rows[i].delay_ms = msg->delay_ms;
            g_rows[i].delay_min_ms = msg->delay_min_ms;
            g_rows[i].offset_ms = msg->offset_ms;
            g_rows[i].offset_mean_ms = msg->offset_mean_ms;
            g_rows[i].offset_stddev_ms = msg->offset_stddev_ms;
            g_rows[i].offset_mad_ms = msg->offset_mad_ms;
            g_rows[i].jitter_ms = msg->jitter_ms;
            g_rows[i].score = msg->score;
            g_rows[i].last_error = msg->last_error;
            strncpy(g_rows[i].last_reference_id, msg->last_reference_id, sizeof(g_rows[i].last_reference_id) - 1);
            g_rows[i].last_reference_id[sizeof(g_rows[i].last_reference_id) - 1] = '\0';
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
    ADDRINFOW *it;
    ADDRINFOW *selected = NULL;
    DWORD ip_len;
    int rc;

    ip[0] = L'\0';
    ptr[0] = L'\0';

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return -1;
    }

    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    rc = GetAddrInfoW(host, L"123", &hints, &resolved);
    if (rc != 0 || resolved == NULL) {
        WSACleanup();
        return -1;
    }

    for (it = resolved; it != NULL; it = it->ai_next) {
        if (it->ai_family == AF_INET || it->ai_family == AF_INET6) {
            selected = it;
            break;
        }
    }
    if (selected == NULL) {
        FreeAddrInfoW(resolved);
        WSACleanup();
        return -1;
    }

    ip_len = (DWORD)ip_chars;
    if (WSAAddressToStringW(selected->ai_addr, (DWORD)selected->ai_addrlen, NULL, ip, &ip_len) != 0) {
        FreeAddrInfoW(resolved);
        WSACleanup();
        return -1;
    }

    if (GetNameInfoW(
            selected->ai_addr,
            (DWORD)selected->ai_addrlen,
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
        wchar_t host[256];

        wcsncpy(host, peers.peers[i].host, (sizeof(host) / sizeof(host[0])) - 1);
        host[(sizeof(host) / sizeof(host[0])) - 1] = L'\0';
        if (!trim_host_inplace(host)) {
            continue;
        }

        g_rows[g_row_count].host[0] = L'\0';
        wcsncpy(g_rows[g_row_count].host, host, (sizeof(g_rows[g_row_count].host) / sizeof(g_rows[g_row_count].host[0])) - 1);
        g_rows[g_row_count].host[(sizeof(g_rows[g_row_count].host) / sizeof(g_rows[g_row_count].host[0])) - 1] = L'\0';
        g_rows[g_row_count].flags = peers.peers[i].flags;
        g_rows[g_row_count].has_probe = 0;
        g_rows[g_row_count].success_samples = 0;
        g_rows[g_row_count].total_samples = 0;
        g_rows[g_row_count].delay_ms = 0.0;
        g_rows[g_row_count].delay_min_ms = 0.0;
        g_rows[g_row_count].offset_ms = 0.0;
        g_rows[g_row_count].offset_mean_ms = 0.0;
        g_rows[g_row_count].offset_stddev_ms = 0.0;
        g_rows[g_row_count].offset_mad_ms = 0.0;
        g_rows[g_row_count].jitter_ms = 0.0;
        g_rows[g_row_count].score = 0.0;
        g_rows[g_row_count].last_error = GW_NTP_ERR_SOCKET;
        g_rows[g_row_count].last_reference_id[0] = '\0';
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
    if (config.has_special_poll_interval && !g_poll_updating) {
        g_poll_updating = 1;
        SetDlgItemInt(dialog, IDC_POLL_VALUE, (UINT)config.special_poll_interval, FALSE);
        g_poll_updating = 0;
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
        int rc = run_elevated_helper(dialog, HELPER_OP_SYNC_NOW, NULL, NULL);
        if (rc == -2) {
            return;
        }
        if (rc == 0) {
            MessageBoxW(dialog, L"Windows Time resync was requested.", L"GW32TIME", MB_ICONINFORMATION);
            refresh_status(dialog);
            trigger_sync_probe_burst(dialog);
        }
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
        return;
    }

    MessageBoxW(dialog, L"Windows Time resync was requested.", L"GW32TIME", MB_ICONINFORMATION);
    refresh_status(dialog);
    trigger_sync_probe_burst(dialog);
}

static void apply_poll_interval(HWND dialog)
{
    int ok = 0;
    int is_admin = 0;
    DWORD seconds = parse_poll_seconds(dialog, &ok);
    w32tm_raw_result_t result;

    if (!ok) {
        MessageBoxW(dialog, L"Poll interval must be a positive integer.", L"GW32TIME", MB_ICONWARNING);
        return;
    }

    if (privilege_is_admin(&is_admin) != 0 || !is_admin) {
        wchar_t params[64];
        int rc;
        _snwprintf(params, sizeof(params) / sizeof(params[0]), L"%lu", (unsigned long)seconds);
        params[(sizeof(params) / sizeof(params[0])) - 1] = L'\0';
        rc = run_elevated_helper(dialog, HELPER_OP_SET_POLL, params, NULL);
        if (rc == -2) {
            return;
        }
        if (rc != 0) {
            MessageBoxW(dialog, L"Failed to apply poll interval.", L"GW32TIME", MB_ICONERROR);
            return;
        }
    } else {
        if (w32time_write_poll_interval(seconds) != 0 ||
            w32tm_config_update_raw(&result) != 0 ||
            result.exit_code != 0) {
            MessageBoxW(dialog, L"Failed to apply poll interval.", L"GW32TIME", MB_ICONERROR);
            return;
        }
    }

    MessageBoxW(dialog, L"Poll interval updated.", L"GW32TIME", MB_ICONINFORMATION);
    refresh_status(dialog);
}

static void trigger_sync_probe_burst(HWND dialog)
{
    start_probe_all_async(dialog);
    g_sync_burst_remaining = 2;
    KillTimer(dialog, TIMER_SYNC_BURST);
    SetTimer(dialog, TIMER_SYNC_BURST, 1000, NULL);
}

static int restart_reached_running_state(void)
{
    svc_state_t state = SVC_STATE_UNKNOWN;
    int i;

    for (i = 0; i < 24; i++) {
        if (svc_query_state(L"w32time", &state) == 0 && state == SVC_STATE_RUNNING) {
            return 1;
        }
        Sleep(250);
    }
    return 0;
}

static int run_service_action(HWND dialog, const wchar_t *action, const wchar_t *mode)
{
    int is_admin = 0;

    if (privilege_is_admin(&is_admin) == 0 && is_admin) {
        if (str_eq(action, L"start")) {
            return svc_start(L"w32time") == 0 ? 0 : 1;
        }
        if (str_eq(action, L"stop")) {
            return svc_stop(L"w32time") == 0 ? 0 : 1;
        }
        if (str_eq(action, L"restart")) {
            return svc_restart(L"w32time") == 0 ? 0 : 1;
        }
        if (str_eq(action, L"mode") && mode != NULL) {
            if (str_eq(mode, L"auto")) {
                return svc_set_start_type(L"w32time", SVC_START_AUTO) == 0 ? 0 : 1;
            }
            if (str_eq(mode, L"manual")) {
                return svc_set_start_type(L"w32time", SVC_START_MANUAL) == 0 ? 0 : 1;
            }
            if (str_eq(mode, L"delayed")) {
                return svc_set_start_type(L"w32time", SVC_START_AUTO_DELAYED) == 0 ? 0 : 1;
            }
            if (str_eq(mode, L"disabled")) {
                return svc_set_start_type(L"w32time", SVC_START_DISABLED) == 0 ? 0 : 1;
            }
        }
        return 1;
    }

    return run_elevated_helper(dialog, HELPER_OP_SERVICE, action, mode);
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

    if (privilege_is_admin(&is_admin) != 0 || !is_admin) {
        if (run_elevated_helper(dialog, HELPER_OP_RESTORE_CONFIG, path, NULL) != 0) {
            MessageBoxW(dialog, L"Configuration was restored, but service refresh failed.", L"GW32TIME", MB_ICONWARNING);
            refresh_status(dialog);
            return;
        }
    } else {
        if (w32time_write_config(&config) != 0) {
            MessageBoxW(dialog, L"Could not restore configuration backup.", L"GW32TIME", MB_ICONERROR);
            return;
        }
        if (w32tm_config_update_raw(&result) != 0 || result.exit_code != 0 || svc_restart(L"w32time") != 0) {
            MessageBoxW(dialog, L"Configuration was restored, but service refresh failed.", L"GW32TIME", MB_ICONWARNING);
            refresh_status(dialog);
            return;
        }
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
            EnableWindow(GetDlgItem(dialog, IDC_DIALOG_DELETE), ctx->allow_delete ? TRUE : FALSE);
        }
        return TRUE;

    case WM_COMMAND:
        if (LOWORD(wparam) == IDC_DIALOG_CHECK) {
            wchar_t host[256];
            char host_utf8[256];
            gw_ntp_checker_config_t cfg;
            gw_ntp_checker_result_t result;
            gw_ntp_explain_t explain;
            wchar_t message[2048];
            int msg_len = 0;
            int i;

            GetDlgItemTextW(dialog, IDC_DIALOG_SERVER, host, sizeof(host) / sizeof(host[0]));
            if (!trim_host_inplace(host)) {
                MessageBoxW(dialog, L"Server host is required.", L"GW32TIME", MB_ICONWARNING);
                return TRUE;
            }
            if (utf8_from_wide(host, host_utf8, sizeof(host_utf8)) != 0) {
                MessageBoxW(dialog, L"NTP checker failed: host conversion.", L"GW32TIME", MB_ICONERROR);
                return TRUE;
            }

            gw_ntp_checker_default_config(&cfg);
            cfg.timeout_ms = 1000;
            cfg.interval_ms = 120;
            if (gw_ntp_checker_server(host_utf8, &cfg, &result) != 0) {
                MessageBoxW(dialog, L"NTP checker failed.", L"GW32TIME", MB_ICONERROR);
                return TRUE;
            }

            msg_len += _snwprintf(
                message + msg_len,
                (sizeof(message) / sizeof(message[0])) - msg_len,
                L"NTP checker report\nHost: %ls\nReach: %d/%d\nOffset median: %.2f ms\nOffset mean: %.2f ms\nOffset stddev: %.2f ms\nOffset MAD: %.2f ms\nDelay mean: %.2f ms\nDelay min: %.2f ms\nJitter: %.2f ms\nScore: %.2f\nStratum: %d\nValidation: %ls",
                host,
                result.success_samples,
                result.total_samples,
                result.offset_median_ms,
                result.offset_mean_ms,
                result.offset_stddev_ms,
                result.offset_mad_ms,
                result.delay_mean_ms,
                result.delay_min_ms,
                result.jitter_ms,
                result.score,
                result.stratum,
                ntp_error_label(result.last_error));

            if (result.last_error == GW_NTP_ERR_KISS_OF_DEATH && result.last_reference_id[0] != '\0') {
                wchar_t refid[16];
                MultiByteToWideChar(CP_UTF8, 0, result.last_reference_id, -1, refid, sizeof(refid) / sizeof(refid[0]));
                refid[(sizeof(refid) / sizeof(refid[0])) - 1] = L'\0';
                msg_len += _snwprintf(
                    message + msg_len,
                    (sizeof(message) / sizeof(message[0])) - msg_len,
                    L" (%ls)",
                    refid);
            }

            if (gw_ntp_checker_explain(&result, &explain) == 0 && explain.count > 0) {
                msg_len += _snwprintf(
                    message + msg_len,
                    (sizeof(message) / sizeof(message[0])) - msg_len,
                    L"\n\nExplain:");
                for (i = 0; i < explain.count && msg_len < (int)(sizeof(message) / sizeof(message[0])) - 4; i++) {
                    wchar_t line_w[160];
                    MultiByteToWideChar(CP_UTF8, 0, explain.lines[i], -1, line_w, sizeof(line_w) / sizeof(line_w[0]));
                    line_w[(sizeof(line_w) / sizeof(line_w[0])) - 1] = L'\0';
                    msg_len += _snwprintf(
                        message + msg_len,
                        (sizeof(message) / sizeof(message[0])) - msg_len,
                        L"\n  %ls",
                        line_w);
                }
            }

            message[(sizeof(message) / sizeof(message[0])) - 1] = L'\0';
            if (result.success_samples <= 0) {
                _snwprintf(
                    message + wcslen(message),
                    (sizeof(message) / sizeof(message[0])) - wcslen(message),
                    L"\n\nNo successful samples.");
                message[(sizeof(message) / sizeof(message[0])) - 1] = L'\0';
                MessageBoxW(dialog, message, L"GW32TIME", MB_ICONERROR);
                return TRUE;
            }

            MessageBoxW(dialog, message, L"GW32TIME", MB_ICONINFORMATION);
            return TRUE;
        }

        if (LOWORD(wparam) == IDC_DIALOG_DELETE) {
            if (ctx != NULL && ctx->allow_delete) {
                ctx->delete_requested = 1;
                EndDialog(dialog, IDOK);
                return TRUE;
            }
            return TRUE;
        }

        if (LOWORD(wparam) == IDOK) {
            DWORD flags;
            wchar_t host[256];

            if (ctx == NULL) {
                EndDialog(dialog, IDCANCEL);
                return TRUE;
            }

            GetDlgItemTextW(dialog, IDC_DIALOG_SERVER, host, sizeof(host) / sizeof(host[0]));
            if (!trim_host_inplace(host)) {
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
    ctx.allow_delete = update ? 1 : 0;
    ctx.delete_requested = 0;
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

    if (ctx.delete_requested && update) {
        int i;
        for (i = row; i + 1 < g_row_count; i++) {
            g_rows[i] = g_rows[i + 1];
        }
        g_row_count--;
        refresh_servers_table(dialog);
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

    if (privilege_is_admin(&is_admin) != 0 || !is_admin) {
        if (run_elevated_helper(dialog, HELPER_OP_APPLY_SERVERS, peerlist, NULL) != 0) {
            MessageBoxW(dialog, L"Failed to apply server list.", L"GW32TIME", MB_ICONERROR);
            return;
        }
    } else {
        if (w32time_write_manual_servers(peerlist) != 0 ||
            w32tm_config_manual_peers_raw(peerlist, &result) != 0 ||
            result.exit_code != 0 ||
            svc_restart(L"w32time") != 0) {
            MessageBoxW(dialog, L"Failed to apply server list.", L"GW32TIME", MB_ICONERROR);
            return;
        }
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
        if (InterlockedCompareExchange(&g_probe_shutdown_requested, 0, 0) != 0 ||
            (ctx->cancel_event != NULL && WaitForSingleObject(ctx->cancel_event, 0) == WAIT_OBJECT_0)) {
            break;
        }
        probe_result_msg_t *msg = (probe_result_msg_t *)malloc(sizeof(probe_result_msg_t));
        if (msg == NULL) {
            continue;
        }
        if (InterlockedCompareExchange(&g_probe_shutdown_requested, 0, 0) != 0 ||
            (ctx->cancel_event != NULL && WaitForSingleObject(ctx->cancel_event, 0) == WAIT_OBJECT_0)) {
            free(msg);
            break;
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
            char host_utf8[256];
            gw_ntp_checker_config_t cfg;
            gw_ntp_checker_result_t result;

            gw_ntp_checker_default_config(&cfg);
            cfg.samples = 3;
            cfg.timeout_ms = 900;
            cfg.interval_ms = 120;

            if (ctx->cancel_event != NULL && WaitForSingleObject(ctx->cancel_event, 0) == WAIT_OBJECT_0) {
                free(msg);
                break;
            }
            if (utf8_from_wide(ctx->rows[i].host, host_utf8, sizeof(host_utf8)) == 0 &&
                gw_ntp_checker_server(host_utf8, &cfg, &result) == 0) {
                ctx->rows[i].has_probe = result.success_samples > 0;
                ctx->rows[i].success_samples = result.success_samples;
                ctx->rows[i].total_samples = result.total_samples;
                ctx->rows[i].stratum = result.stratum;
                ctx->rows[i].ping_ms = result.delay_mean_ms > 0.0 ? (DWORD)result.delay_mean_ms : 0;
                ctx->rows[i].delay_ms = result.delay_mean_ms;
                ctx->rows[i].delay_min_ms = result.delay_min_ms;
                ctx->rows[i].offset_ms = result.offset_median_ms;
                ctx->rows[i].offset_mean_ms = result.offset_mean_ms;
                ctx->rows[i].offset_stddev_ms = result.offset_stddev_ms;
                ctx->rows[i].offset_mad_ms = result.offset_mad_ms;
                ctx->rows[i].jitter_ms = result.jitter_ms;
                ctx->rows[i].score = result.score;
                ctx->rows[i].last_error = result.last_error;
                strncpy(ctx->rows[i].last_reference_id, result.last_reference_id, sizeof(ctx->rows[i].last_reference_id) - 1);
                ctx->rows[i].last_reference_id[sizeof(ctx->rows[i].last_reference_id) - 1] = '\0';
            }
        }

        msg->has_probe = ctx->rows[i].has_probe;
        msg->success_samples = ctx->rows[i].success_samples;
        msg->total_samples = ctx->rows[i].total_samples;
        msg->stratum = ctx->rows[i].stratum;
        msg->ping_ms = ctx->rows[i].ping_ms;
        msg->delay_ms = ctx->rows[i].delay_ms;
        msg->delay_min_ms = ctx->rows[i].delay_min_ms;
        msg->offset_ms = ctx->rows[i].offset_ms;
        msg->offset_mean_ms = ctx->rows[i].offset_mean_ms;
        msg->offset_stddev_ms = ctx->rows[i].offset_stddev_ms;
        msg->offset_mad_ms = ctx->rows[i].offset_mad_ms;
        msg->jitter_ms = ctx->rows[i].jitter_ms;
        msg->score = ctx->rows[i].score;
        msg->last_error = ctx->rows[i].last_error;
        strncpy(msg->last_reference_id, ctx->rows[i].last_reference_id, sizeof(msg->last_reference_id) - 1);
        msg->last_reference_id[sizeof(msg->last_reference_id) - 1] = '\0';
        wcsncpy(msg->ip, ctx->rows[i].ip, (sizeof(msg->ip) / sizeof(msg->ip[0])) - 1);
        msg->ip[(sizeof(msg->ip) / sizeof(msg->ip[0])) - 1] = L'\0';
        wcsncpy(msg->ptr, ctx->rows[i].ptr, (sizeof(msg->ptr) / sizeof(msg->ptr[0])) - 1);
        msg->ptr[(sizeof(msg->ptr) / sizeof(msg->ptr[0])) - 1] = L'\0';
        if (InterlockedCompareExchange(&g_probe_shutdown_requested, 0, 0) != 0) {
            free(msg);
            break;
        }
        if (!PostMessageW(ctx->dialog, WM_APP_PROBE_RESULT, 0, (LPARAM)msg)) {
            free(msg);
        }
    }
    if (ctx->cancel_event != NULL) {
        CloseHandle(ctx->cancel_event);
        ctx->cancel_event = NULL;
    }
    if (InterlockedCompareExchange(&g_probe_shutdown_requested, 0, 0) != 0) {
        g_probe_ctx = NULL;
        free(ctx);
        InterlockedExchange(&g_probe_running, 0);
        return 0;
    }
    if (!PostMessageW(ctx->dialog, WM_APP_PROBE_DONE, 0, (LPARAM)ctx)) {
        g_probe_ctx = NULL;
        free(ctx);
        InterlockedExchange(&g_probe_running, 0);
    }
    return 0;
}

static void start_probe_all_async(HWND dialog)
{
    probe_run_ctx_t *ctx;
    int i;

    if (InterlockedCompareExchange(&g_probe_running, 1, 0) != 0) {
        return;
    }
    InterlockedExchange(&g_probe_shutdown_requested, 0);

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
    ctx->cancel_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (ctx->cancel_event == NULL) {
        free(ctx);
        InterlockedExchange(&g_probe_running, 0);
        SetWindowTextW(GetDlgItem(dialog, IDC_PROBE_ALL), L"Check Servers");
        EnableWindow(GetDlgItem(dialog, IDC_PROBE_ALL), TRUE);
        MessageBoxW(dialog, L"Could not allocate probe cancel event.", L"GW32TIME", MB_ICONERROR);
        return;
    }
    ctx->count = g_row_count;
    for (i = 0; i < g_row_count; i++) {
        ctx->rows[i] = g_rows[i];
    }

    g_probe_thread = CreateThread(NULL, 0, probe_all_thread_proc, ctx, 0, NULL);
    if (g_probe_thread == NULL) {
        CloseHandle(ctx->cancel_event);
        ctx->cancel_event = NULL;
        free(ctx);
        InterlockedExchange(&g_probe_running, 0);
        SetWindowTextW(GetDlgItem(dialog, IDC_PROBE_ALL), L"Check Servers");
        EnableWindow(GetDlgItem(dialog, IDC_PROBE_ALL), TRUE);
        MessageBoxW(dialog, L"Could not start background probe.", L"GW32TIME", MB_ICONERROR);
    } else {
        g_probe_ctx = ctx;
    }
}

static void finish_probe_all_async(HWND dialog)
{
    if (g_probe_thread != NULL) {
        WaitForSingleObject(g_probe_thread, INFINITE);
        CloseHandle(g_probe_thread);
        g_probe_thread = NULL;
    }
    g_probe_ctx = NULL;
    InterlockedExchange(&g_probe_running, 0);
    SetWindowTextW(GetDlgItem(dialog, IDC_PROBE_ALL), L"Check Servers");
    EnableWindow(GetDlgItem(dialog, IDC_PROBE_ALL), TRUE);
}

static void request_probe_cancel(void)
{
    InterlockedExchange(&g_probe_shutdown_requested, 1);
    if (g_probe_ctx != NULL && g_probe_ctx->cancel_event != NULL) {
        SetEvent(g_probe_ctx->cancel_event);
    }
}

static void wait_probe_thread_briefly(void)
{
    if (g_probe_thread == NULL) {
        return;
    }
    if (WaitForSingleObject(g_probe_thread, 1200) == WAIT_OBJECT_0) {
        CloseHandle(g_probe_thread);
        g_probe_thread = NULL;
        g_probe_ctx = NULL;
        InterlockedExchange(&g_probe_running, 0);
    }
}

static void drain_probe_messages(HWND dialog)
{
    MSG msg;

    if (dialog == NULL) {
        return;
    }
    while (PeekMessageW(&msg, dialog, WM_APP_PROBE_RESULT, WM_APP_PROBE_RESULT, PM_REMOVE)) {
        if (msg.lParam != 0) {
            free((probe_result_msg_t *)msg.lParam);
        }
    }
    while (PeekMessageW(&msg, dialog, WM_APP_PROBE_DONE, WM_APP_PROBE_DONE, PM_REMOVE)) {
        if (msg.lParam != 0) {
            free((probe_run_ctx_t *)msg.lParam);
        }
    }
}

static INT_PTR CALLBACK main_dialog_proc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam)
{
    (void)lparam;

    switch (message) {
    case WM_INITDIALOG:
        g_main_dialog = dialog;
        bump_main_window_layer(dialog);
        init_servers_table(dialog);
        SendDlgItemMessageW(dialog, IDC_POLL_SPIN, UDM_SETRANGE32, 1, 604800);
        SendDlgItemMessageW(dialog, IDC_POLL_SPIN, UDM_SETBUDDY, (WPARAM)GetDlgItem(dialog, IDC_POLL_VALUE), 0);
        SendDlgItemMessageW(dialog, IDC_REALTIME_SPIN, UDM_SETRANGE32, REALTIME_MIN_SECONDS, REALTIME_MAX_SECONDS);
        SendDlgItemMessageW(dialog, IDC_REALTIME_SPIN, UDM_SETBUDDY, (WPARAM)GetDlgItem(dialog, IDC_REALTIME_SECONDS), 0);
        SetDlgItemInt(dialog, IDC_REALTIME_SECONDS, (UINT)g_realtime_seconds, FALSE);
        CheckDlgButton(dialog, IDC_REALTIME_CHECK, BST_CHECKED);
        update_realtime_controls(dialog);
        restart_realtime_timer(dialog);
        if (ensure_bold_font() != NULL) {
            SendDlgItemMessageW(dialog, IDC_HEADER_TEXT, WM_SETFONT, (WPARAM)g_bold_font, TRUE);
            SendDlgItemMessageW(dialog, IDC_UAC_STATUS, WM_SETFONT, (WPARAM)g_bold_font, TRUE);
            SendDlgItemMessageW(dialog, IDC_CURRENT_TIME, WM_SETFONT, (WPARAM)g_bold_font, TRUE);
            SendDlgItemMessageW(dialog, IDC_POLL_VALUE, WM_SETFONT, (WPARAM)g_bold_font, TRUE);
        }
        refresh_status(dialog);
        start_probe_all_async(dialog);
        SetTimer(dialog, TIMER_CLOCK, 1000, NULL);
        return TRUE;

    case WM_TIMER:
        if (wparam == TIMER_CLOCK) {
            refresh_datetime_block(dialog);
            refresh_service_runtime(dialog);
            return TRUE;
        }
        if (wparam == TIMER_REALTIME_CHECK) {
            start_probe_all_async(dialog);
            return TRUE;
        }
        if (wparam == TIMER_SYNC_BURST) {
            if (g_sync_burst_remaining <= 0) {
                KillTimer(dialog, TIMER_SYNC_BURST);
                return TRUE;
            }
            if (g_probe_running == 0) {
                start_probe_all_async(dialog);
                g_sync_burst_remaining--;
            }
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

    case WM_NOTIFY: {
        LPNMHDR hdr = (LPNMHDR)lparam;
        if (hdr != NULL && hdr->idFrom == IDC_SERVERS_TABLE && hdr->code == NM_CLICK) {
            LPNMITEMACTIVATE act = (LPNMITEMACTIVATE)lparam;
            HWND table = GetDlgItem(dialog, IDC_SERVERS_TABLE);
            if (act != NULL && table != NULL && act->iItem >= 0) {
                ListView_SetItemState(table, act->iItem, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                add_or_update_server(dialog, 1);
                return TRUE;
            }
        }
        return FALSE;
    }

    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case IDC_UAC_STATUS:
            if (HIWORD(wparam) == STN_CLICKED) {
                int rc = run_elevated_helper(dialog, HELPER_OP_UAC_PING, NULL, NULL);
                if (rc == 0) {
                    refresh_datetime_block(dialog);
                }
                return TRUE;
            }
            return FALSE;
        case IDC_POLL_APPLY:
            apply_poll_interval(dialog);
            return TRUE;
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
        case IDC_SERVICE_MENU: {
            HMENU menu = CreatePopupMenu();
            HMENU mode_menu = CreatePopupMenu();
            RECT rc;
            POINT pt;
            UINT selected;
            if (menu == NULL || mode_menu == NULL) {
                if (mode_menu != NULL) {
                    DestroyMenu(mode_menu);
                }
                if (menu != NULL) {
                    DestroyMenu(menu);
                }
                return TRUE;
            }
            AppendMenuW(menu, MF_STRING, IDM_SERVICE_START, L"Start");
            AppendMenuW(menu, MF_STRING, IDM_SERVICE_STOP, L"Stop");
            AppendMenuW(menu, MF_STRING, IDM_SERVICE_RESTART, L"Restart");
            AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(mode_menu, MF_STRING, IDM_SERVICE_MODE_AUTO, L"Auto");
            AppendMenuW(mode_menu, MF_STRING, IDM_SERVICE_MODE_MANUAL, L"Manual");
            AppendMenuW(mode_menu, MF_STRING, IDM_SERVICE_MODE_DELAYED, L"Delayed");
            AppendMenuW(mode_menu, MF_STRING, IDM_SERVICE_MODE_DISABLED, L"Disabled");
            AppendMenuW(menu, MF_POPUP, (UINT_PTR)mode_menu, L"Mode");

            GetWindowRect(GetDlgItem(dialog, IDC_SERVICE_MENU), &rc);
            pt.x = rc.left;
            pt.y = rc.bottom;
            selected = TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD, pt.x, pt.y, 0, dialog, NULL);
            DestroyMenu(menu);

            if (selected == IDM_SERVICE_START) {
                set_text(dialog, IDC_SERVICE, L"Start pending");
                if (run_service_action(dialog, L"start", NULL) != 0) {
                    MessageBoxW(dialog, L"Service start failed.", L"GW32TIME", MB_ICONERROR);
                }
                refresh_status(dialog);
            } else if (selected == IDM_SERVICE_STOP) {
                set_text(dialog, IDC_SERVICE, L"Stop pending");
                if (run_service_action(dialog, L"stop", NULL) != 0) {
                    MessageBoxW(dialog, L"Service stop failed.", L"GW32TIME", MB_ICONERROR);
                }
                refresh_status(dialog);
            } else if (selected == IDM_SERVICE_RESTART) {
                set_text(dialog, IDC_SERVICE, L"Restart pending");
                if (run_service_action(dialog, L"restart", NULL) != 0) {
                    if (!restart_reached_running_state()) {
                        MessageBoxW(dialog, L"Service restart failed.", L"GW32TIME", MB_ICONERROR);
                    }
                }
                refresh_status(dialog);
            } else if (selected == IDM_SERVICE_MODE_AUTO) {
                if (run_service_action(dialog, L"mode", L"auto") != 0) {
                    MessageBoxW(dialog, L"Service mode update failed.", L"GW32TIME", MB_ICONERROR);
                }
                refresh_status(dialog);
            } else if (selected == IDM_SERVICE_MODE_MANUAL) {
                if (run_service_action(dialog, L"mode", L"manual") != 0) {
                    MessageBoxW(dialog, L"Service mode update failed.", L"GW32TIME", MB_ICONERROR);
                }
                refresh_status(dialog);
            } else if (selected == IDM_SERVICE_MODE_DELAYED) {
                if (run_service_action(dialog, L"mode", L"delayed") != 0) {
                    MessageBoxW(dialog, L"Service mode update failed.", L"GW32TIME", MB_ICONERROR);
                }
                refresh_status(dialog);
            } else if (selected == IDM_SERVICE_MODE_DISABLED) {
                if (run_service_action(dialog, L"mode", L"disabled") != 0) {
                    MessageBoxW(dialog, L"Service mode update failed.", L"GW32TIME", MB_ICONERROR);
                }
                refresh_status(dialog);
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
                request_probe_cancel();
                wait_probe_thread_briefly();
            }
            KillTimer(dialog, TIMER_CLOCK);
            KillTimer(dialog, TIMER_REALTIME_CHECK);
            KillTimer(dialog, TIMER_SYNC_BURST);
            close_elevated_helper();
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
            if (LOWORD(wparam) == IDC_POLL_VALUE && HIWORD(wparam) == EN_CHANGE) {
                if (g_poll_updating) {
                    return TRUE;
                }
                return TRUE;
            }
            if (LOWORD(wparam) == IDC_REALTIME_SECONDS && HIWORD(wparam) == EN_KILLFOCUS) {
                update_realtime_controls(dialog);
                restart_realtime_timer(dialog);
                return TRUE;
            }
            return FALSE;
        }
    case WM_DESTROY:
        request_probe_cancel();
        if (g_probe_thread != NULL) {
            wait_probe_thread_briefly();
            if (g_probe_thread != NULL) {
                CloseHandle(g_probe_thread);
                g_probe_thread = NULL;
                g_probe_ctx = NULL;
            }
        }
        InterlockedExchange(&g_probe_running, 0);
        drain_probe_messages(dialog);
        close_elevated_helper();
        if (g_bold_font != NULL) {
            DeleteObject(g_bold_font);
            g_bold_font = NULL;
        }
        return TRUE;
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
