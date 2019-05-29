#include "cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <windows.h>

#include "format.h"

#include "../core/config_file.h"
#include "../core/diagnostics.h"
#include "../core/domain.h"
#include "../core/error.h"
#include "../core/ntp_probe.h"
#include "../core/preset.h"
#include "../core/privilege.h"
#include "../core/service.h"
#include "../core/time_set.h"
#include "../core/winver.h"
#include "../core/w32time.h"
#include "../core/w32tm.h"
#include "../gui/gui.h"

#define GW32TIME_VERSION L"0.0.1"

static int arg_is(const wchar_t *arg, const wchar_t *value)
{
    return wcscmp(arg, value) == 0;
}

static void print_version(void)
{
    format_title();
    wprintf(L"Version %ls\n", GW32TIME_VERSION);
}

static void print_help(void)
{
    format_title();
    wprintf(L"\n");
    wprintf(L"Usage:\n");
    wprintf(L"  gw32time --help\n");
    wprintf(L"  gw32time --version\n");
    wprintf(L"  gw32time status [--raw] [--verbose]\n");
    wprintf(L"  gw32time gui\n");
    wprintf(L"  gw32time runtime\n");
    wprintf(L"  gw32time health\n");
    wprintf(L"  gw32time diag [--raw]\n");
    wprintf(L"  gw32time service status|start|restart\n");
    wprintf(L"  gw32time servers list\n");
    wprintf(L"  gw32time servers test <host>\n");
    wprintf(L"  gw32time servers set <host...> [--dry-run] [--yes] [--no-sync] [--force-domain]\n");
    wprintf(L"  gw32time poll get\n");
    wprintf(L"  gw32time poll set <seconds> [--dry-run] [--yes] [--force]\n");
    wprintf(L"  gw32time preset list\n");
    wprintf(L"  gw32time preset desktop|lab-fast|windows-default|domain [--dry-run] [--yes]\n");
    wprintf(L"  gw32time sane [--dry-run] [--yes]\n");
    wprintf(L"  gw32time backup <file>\n");
    wprintf(L"  gw32time restore <file> [--dry-run] [--yes]\n");
    wprintf(L"  gw32time menu\n");
    wprintf(L"  gw32time sync [--yes]\n");
}

static int print_admin_status(void)
{
    int is_admin = 0;
    int rc = privilege_is_admin(&is_admin);
    if (rc != 0) {
        format_section(L"Privileges");
        format_field(L"Admin", L"unknown");
        return rc;
    }

    format_section(L"Privileges");
    format_field(L"Admin", is_admin ? L"yes" : L"no");
    return 0;
}

static int print_service_status(void)
{
    svc_state_t state;
    svc_start_type_t start_type;

    if (svc_query_state(L"w32time", &state) != 0) {
        error_print_last(L"Query service state");
        return 1;
    }

    if (svc_query_start_type(L"w32time", &start_type) != 0) {
        error_print_last(L"Query service start type");
        return 1;
    }

    format_section(L"Service");
    format_field(L"Name", L"w32time");
    format_field(L"State", svc_state_name(state));
    format_field(L"Start", svc_start_type_name(start_type));
    return 0;
}

static int service_start_command(void)
{
    int is_admin = 0;

    if (privilege_is_admin(&is_admin) != 0 || !is_admin) {
        fwprintf(stderr, L"Starting Windows Time service requires an elevated administrator token.\n");
        return 1;
    }

    if (svc_start(L"w32time") != 0) {
        error_print_last(L"Start Windows Time service");
        return 1;
    }

    wprintf(L"Windows Time service start requested.\n");
    return print_service_status();
}

static int service_restart_command(void)
{
    int is_admin = 0;

    if (privilege_is_admin(&is_admin) != 0 || !is_admin) {
        fwprintf(stderr, L"Restarting Windows Time service requires an elevated administrator token.\n");
        return 1;
    }

    if (svc_restart(L"w32time") != 0) {
        error_print_last(L"Restart Windows Time service");
        return 1;
    }

    wprintf(L"Windows Time service restart requested.\n");
    return print_service_status();
}

static void print_admin_block(void)
{
    int is_admin = 0;

    format_section(L"Privileges");
    if (privilege_is_admin(&is_admin) != 0) {
        format_field(L"Admin", L"unknown");
        return;
    }

    format_field(L"Admin", is_admin ? L"yes" : L"no");
}

static void print_server_summary(const wchar_t *raw)
{
    ntp_peer_list_t peers;
    int i;

    if (raw == NULL || raw[0] == L'\0') {
        wprintf(L"Servers:  (none)\n");
        return;
    }

    if (ntp_parse_peer_list(raw, &peers) != 0 || peers.count == 0) {
        wprintf(L"Servers:  %ls\n", raw);
        return;
    }

    wprintf(L"Servers:  ");
    for (i = 0; i < peers.count; i++) {
        wprintf(L"%ls%ls", i > 0 ? L", " : L"", peers.peers[i].host);
    }
    wprintf(L"\n");
}

static void print_w32tm_block(const wchar_t *title, int (*query)(w32tm_raw_result_t *))
{
    w32tm_raw_result_t result;

    wprintf(L"\n%ls:\n", title);
    if (query(&result) != 0) {
        error_print_last(title);
        return;
    }

    wprintf(L"  Exit code: %lu\n", (unsigned long)result.exit_code);
    if (result.raw[0] != L'\0') {
        wprintf(L"%ls\n", result.raw);
    } else {
        wprintf(L"  (no output)\n");
    }
}

static int have_admin_token(void)
{
    int is_admin = 0;

    return privilege_is_admin(&is_admin) == 0 && is_admin;
}

static void print_w32tm_configuration_if_allowed(void)
{
    if (!have_admin_token()) {
        wprintf(L"\nw32tm /query /configuration:\n");
        wprintf(L"  Skipped: requires administrator privileges.\n");
        return;
    }

    print_w32tm_block(L"w32tm /query /configuration", w32tm_query_configuration_raw);
}

static void print_runtime_summary(void)
{
    w32tm_raw_result_t raw;
    w32tm_status_summary_t summary;
    wchar_t source_host[256];
    wchar_t *comma;

    if (w32tm_query_status_raw(&raw) != 0 || raw.exit_code != 0) {
        format_section(L"Runtime");
        format_field(L"Status", L"unavailable");
        return;
    }

    if (w32tm_parse_status_summary(raw.raw, &summary) != 0) {
        format_section(L"Runtime");
        format_field(L"Status", L"unparsed");
        return;
    }

    format_section(L"Runtime");
    if (summary.has_source) {
        wcsncpy(source_host, summary.source, (sizeof(source_host) / sizeof(source_host[0])) - 1);
        source_host[(sizeof(source_host) / sizeof(source_host[0])) - 1] = L'\0';
        comma = wcschr(source_host, L',');
        if (comma != NULL) {
            *comma = L'\0';
        }
        format_field(L"Source", source_host);
    }
    if (summary.has_stratum) {
        format_field(L"Stratum", summary.stratum);
    }
    if (summary.has_last_sync) {
        format_field(L"LastSync", summary.last_sync);
    }
    if (summary.has_poll_interval) {
        format_field(L"Poll", summary.poll_interval);
    }
}

static void print_peer_summary(const wchar_t *raw)
{
    ntp_peer_list_t peers;

    if (raw == NULL || raw[0] == L'\0') {
        wprintf(L"Peers:    0\n");
        return;
    }

    if (ntp_parse_peer_list(raw, &peers) != 0) {
        wprintf(L"Peers:    unparsed\n");
        return;
    }

    wprintf(L"Peers:    %d\n", peers.count);
}

static int print_status(int raw, int verbose)
{
    svc_state_t state = SVC_STATE_UNKNOWN;
    svc_start_type_t start_type = SVC_START_UNKNOWN;
    w32time_config_t config;
    domain_info_t domain;

    if (svc_query_state(L"w32time", &state) != 0) {
        state = SVC_STATE_UNKNOWN;
    }

    if (svc_query_start_type(L"w32time", &start_type) != 0) {
        start_type = SVC_START_UNKNOWN;
    }

    if (w32time_read_config(&config) != 0) {
        error_print_last(L"Read W32Time configuration");
        return 1;
    }

    format_title();
    wprintf(L"\n\n");
    wprintf(L"Service:  %ls\n", svc_state_name(state));
    wprintf(L"Start:    %ls\n", svc_start_type_name(start_type));
    wprintf(L"Type:     %ls\n", config.type[0] ? config.type : L"unknown");
    print_server_summary(config.ntp_server);
    print_peer_summary(config.ntp_server);
    if (config.has_special_poll_interval) {
        wprintf(L"Poll:     %lu sec\n", (unsigned long)config.special_poll_interval);
    } else {
        wprintf(L"Poll:     unknown\n");
    }
    if (domain_query(&domain) == 0 && domain.joined) {
        wprintf(L"Domain:   joined");
        if (domain.name[0] != L'\0') {
            wprintf(L" (%ls)", domain.name);
        }
        wprintf(L"\n");
    }
    print_admin_block();

    if (raw) {
        print_w32tm_block(L"w32tm /query /status", w32tm_query_status_raw);
    }

    if (verbose) {
        print_w32tm_block(L"w32tm /query /peers", w32tm_query_peers_raw);
        print_w32tm_configuration_if_allowed();
    }

    return 0;
}

static int print_diag(int raw)
{
    int rc;
    health_t health;
    os_info_t os;

    wprintf(L"Windows Time Diagnostics\n");
    wprintf(L"========================\n\n");

    if (winver_query(&os) == 0) {
        wprintf(L"OS:\n");
        wprintf(L"  Version: %lu.%lu build %lu\n",
            (unsigned long)os.major,
            (unsigned long)os.minor,
            (unsigned long)os.build);
        wprintf(L"  Arch:    %ls\n\n", os_arch_name(os.arch));
    }

    rc = print_status(0, 0);
    if (rc != 0) {
        return rc;
    }

    if (diagnostics_evaluate_health(&health) == 0) {
        wprintf(L"\nHealth:\n");
        wprintf(L"  State:  %ls\n", health_state_name(health.state));
        wprintf(L"  Reason: %ls\n", health.reason);
    }

    print_runtime_summary();
    if (raw) {
        print_w32tm_block(L"w32tm /query /status", w32tm_query_status_raw);
        print_w32tm_block(L"w32tm /query /peers", w32tm_query_peers_raw);
        print_w32tm_configuration_if_allowed();
    }
    return 0;
}

static int run_internal_set_time(int argc, wchar_t **argv)
{
    SYSTEMTIME st;
    unsigned short y, mo, d, h, mi, s;

    if (argc < 4) {
        return 2;
    }

    if (swscanf(argv[2], L"%hu-%hu-%hu", &y, &mo, &d) != 3) {
        return 2;
    }
    if (swscanf(argv[3], L"%hu:%hu:%hu", &h, &mi, &s) != 3) {
        return 2;
    }

    ZeroMemory(&st, sizeof(st));
    st.wYear = y;
    st.wMonth = mo;
    st.wDay = d;
    st.wHour = h;
    st.wMinute = mi;
    st.wSecond = s;
    st.wMilliseconds = 0;

    if (time_set_local(&st) != 0) {
        DWORD err = GetLastError();
        if (err == ERROR_PRIVILEGE_NOT_HELD || err == ERROR_ACCESS_DENIED) {
            return 5;
        }
        return 1;
    }
    return 0;
}

static int run_internal_sync_now(void)
{
    int is_admin = 0;
    svc_state_t service_state = SVC_STATE_UNKNOWN;
    w32tm_raw_result_t result;

    if (privilege_is_admin(&is_admin) != 0 || !is_admin) {
        return 5;
    }
    if (svc_query_state(L"w32time", &service_state) == 0 && service_state != SVC_STATE_RUNNING) {
        if (svc_start(L"w32time") != 0) {
            return 1;
        }
        Sleep(1200);
    }
    if (w32tm_resync_raw(&result) != 0 || result.exit_code != 0) {
        return 1;
    }
    return 0;
}

static int run_internal_apply_servers(int argc, wchar_t **argv)
{
    w32tm_raw_result_t result;

    if (argc < 3 || argv[2] == NULL || argv[2][0] == L'\0') {
        return 2;
    }
    if (w32time_write_manual_servers(argv[2]) != 0 ||
        w32tm_config_manual_peers_raw(argv[2], &result) != 0 ||
        result.exit_code != 0 ||
        svc_restart(L"w32time") != 0) {
        return 1;
    }
    return 0;
}

static int run_internal_restore_config(int argc, wchar_t **argv)
{
    w32time_config_t config;
    w32tm_raw_result_t result;

    if (argc < 3 || argv[2] == NULL || argv[2][0] == L'\0') {
        return 2;
    }
    if (config_file_read(argv[2], &config) != 0 || w32time_write_config(&config) != 0) {
        return 1;
    }
    if (w32tm_config_update_raw(&result) != 0 || result.exit_code != 0 || svc_restart(L"w32time") != 0) {
        return 1;
    }
    return 0;
}

static int run_internal_service_cmd(int argc, wchar_t **argv)
{
    if (argc < 3) {
        return 2;
    }
    if (arg_is(argv[2], L"start")) {
        return svc_start(L"w32time") == 0 ? 0 : 1;
    }
    if (arg_is(argv[2], L"stop")) {
        return svc_stop(L"w32time") == 0 ? 0 : 1;
    }
    if (arg_is(argv[2], L"restart")) {
        return svc_restart(L"w32time") == 0 ? 0 : 1;
    }
    if (arg_is(argv[2], L"mode")) {
        if (argc < 4) {
            return 2;
        }
        if (arg_is(argv[3], L"auto")) {
            return svc_set_start_type(L"w32time", SVC_START_AUTO) == 0 ? 0 : 1;
        }
        if (arg_is(argv[3], L"manual")) {
            return svc_set_start_type(L"w32time", SVC_START_MANUAL) == 0 ? 0 : 1;
        }
        if (arg_is(argv[3], L"delayed")) {
            return svc_set_start_type(L"w32time", SVC_START_AUTO_DELAYED) == 0 ? 0 : 1;
        }
        if (arg_is(argv[3], L"disabled")) {
            return svc_set_start_type(L"w32time", SVC_START_DISABLED) == 0 ? 0 : 1;
        }
        return 2;
    }
    return 2;
}

static int run_internal_set_poll(int argc, wchar_t **argv)
{
    wchar_t *end = NULL;
    unsigned long seconds_ul;
    DWORD seconds;
    w32tm_raw_result_t result;

    if (argc < 3 || argv[2] == NULL || argv[2][0] == L'\0') {
        return 2;
    }

    seconds_ul = wcstoul(argv[2], &end, 10);
    if (end == argv[2] || *end != L'\0' || seconds_ul == 0 || seconds_ul > 0xffffffffUL) {
        return 2;
    }
    seconds = (DWORD)seconds_ul;

    if (w32time_write_poll_interval(seconds) != 0) {
        return 1;
    }
    if (w32tm_config_update_raw(&result) != 0 || result.exit_code != 0) {
        return 1;
    }
    return 0;
}

static int run_internal_uac_ping(void)
{
    return 0;
}

static wchar_t *helper_skip_space(wchar_t *p)
{
    while (p != NULL && (*p == L' ' || *p == L'\t')) {
        p++;
    }
    return p;
}

static wchar_t *helper_next_arg(wchar_t **cursor)
{
    wchar_t *start;
    wchar_t *p;

    if (cursor == NULL || *cursor == NULL) {
        return NULL;
    }
    p = helper_skip_space(*cursor);
    if (*p == L'\0') {
        *cursor = p;
        return NULL;
    }
    if (*p == L'"') {
        p++;
        start = p;
        while (*p != L'\0' && *p != L'"') {
            p++;
        }
        if (*p == L'"') {
            *p = L'\0';
            p++;
        }
        *cursor = p;
        return start;
    }
    start = p;
    while (*p != L'\0' && *p != L' ' && *p != L'\t') {
        p++;
    }
    if (*p != L'\0') {
        *p = L'\0';
        p++;
    }
    *cursor = p;
    return start;
}

static int run_helper_command(wchar_t *command)
{
    wchar_t *cursor = command;
    wchar_t *verb = helper_next_arg(&cursor);
    wchar_t *argv_local[5];

    if (verb == NULL) {
        return 2;
    }
    if (arg_is(verb, L"__uac-ping")) {
        return run_internal_uac_ping();
    }
    if (arg_is(verb, L"__sync-now")) {
        return run_internal_sync_now();
    }
    if (arg_is(verb, L"__set-time")) {
        argv_local[0] = L"gw32time";
        argv_local[1] = verb;
        argv_local[2] = helper_next_arg(&cursor);
        argv_local[3] = helper_next_arg(&cursor);
        return run_internal_set_time(4, argv_local);
    }
    if (arg_is(verb, L"__apply-servers")) {
        argv_local[0] = L"gw32time";
        argv_local[1] = verb;
        argv_local[2] = helper_next_arg(&cursor);
        return run_internal_apply_servers(3, argv_local);
    }
    if (arg_is(verb, L"__restore-config")) {
        argv_local[0] = L"gw32time";
        argv_local[1] = verb;
        argv_local[2] = helper_next_arg(&cursor);
        return run_internal_restore_config(3, argv_local);
    }
    if (arg_is(verb, L"__svc")) {
        argv_local[0] = L"gw32time";
        argv_local[1] = verb;
        argv_local[2] = helper_next_arg(&cursor);
        argv_local[3] = helper_next_arg(&cursor);
        return run_internal_service_cmd(argv_local[3] != NULL ? 4 : 3, argv_local);
    }
    if (arg_is(verb, L"__set-poll")) {
        argv_local[0] = L"gw32time";
        argv_local[1] = verb;
        argv_local[2] = helper_next_arg(&cursor);
        return run_internal_set_poll(3, argv_local);
    }
    return 2;
}

static int run_internal_helper_server(int argc, wchar_t **argv)
{
    HANDLE pipe;
    DWORD bytes_read;
    DWORD bytes_written;
    wchar_t command[2048];
    DWORD exit_code;

    if (argc < 3 || argv[2] == NULL || argv[2][0] == L'\0') {
        return 2;
    }

    pipe = CreateNamedPipeW(
        argv[2],
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        1,
        sizeof(exit_code),
        sizeof(command),
        0,
        NULL);
    if (pipe == INVALID_HANDLE_VALUE) {
        return 1;
    }

    if (!ConnectNamedPipe(pipe, NULL) && GetLastError() != ERROR_PIPE_CONNECTED) {
        CloseHandle(pipe);
        return 1;
    }

    for (;;) {
        ZeroMemory(command, sizeof(command));
        if (!ReadFile(pipe, command, sizeof(command) - sizeof(wchar_t), &bytes_read, NULL) || bytes_read == 0) {
            break;
        }
        command[(sizeof(command) / sizeof(command[0])) - 1] = L'\0';
        if (wcscmp(command, L"__exit") == 0) {
            exit_code = 0;
            WriteFile(pipe, &exit_code, sizeof(exit_code), &bytes_written, NULL);
            break;
        }
        exit_code = (DWORD)run_helper_command(command);
        if (!WriteFile(pipe, &exit_code, sizeof(exit_code), &bytes_written, NULL)) {
            break;
        }
    }

    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
    return 0;
}

static int print_health(void)
{
    health_t health;

    if (diagnostics_evaluate_health(&health) != 0) {
        error_print_last(L"Evaluate health");
        return 1;
    }

    wprintf(L"Health: %ls\n", health_state_name(health.state));
    wprintf(L"Reason: %ls\n", health.reason);
    return health.state == HEALTH_BROKEN ? 1 : 0;
}

static void print_resync_failure_hints(void)
{
    fwprintf(stderr, L"Try:\n");
    fwprintf(stderr, L"  - run 'gw32time health'\n");
    fwprintf(stderr, L"  - run 'gw32time servers list'\n");
    fwprintf(stderr, L"  - test UDP/123 with 'gw32time servers test <host>'\n");
    fwprintf(stderr, L"  - run 'gw32time diag' for raw w32tm output\n");
}

static int has_arg(int argc, wchar_t **argv, const wchar_t *value)
{
    int i;

    for (i = 2; i < argc; i++) {
        if (arg_is(argv[i], value)) {
            return 1;
        }
    }

    return 0;
}

static int confirm_action(const wchar_t *prompt, int assume_yes)
{
    wchar_t answer[16];

    if (assume_yes) {
        return 1;
    }

    wprintf(L"%ls [y/N] ", prompt);
    if (fgetws(answer, sizeof(answer) / sizeof(answer[0]), stdin) == NULL) {
        return 0;
    }

    return answer[0] == L'y' || answer[0] == L'Y';
}

static int run_sync_now(int argc, wchar_t **argv)
{
    int is_admin = 0;
    int assume_yes = has_arg(argc, argv, L"--yes");
    svc_state_t state = SVC_STATE_UNKNOWN;
    w32tm_raw_result_t result;

    if (privilege_is_admin(&is_admin) != 0 || !is_admin) {
        fwprintf(stderr, L"Sync requires an elevated administrator token.\n");
        return 1;
    }

    if (svc_query_state(L"w32time", &state) != 0) {
        error_print_last(L"Query service state");
        return 1;
    }

    if (state != SVC_STATE_RUNNING) {
        wprintf(L"Windows Time service is %ls.\n", svc_state_name(state));
        if (!confirm_action(L"Start Windows Time service before resync?", assume_yes)) {
            wprintf(L"Sync cancelled.\n");
            return 1;
        }

        if (svc_start(L"w32time") != 0) {
            error_print_last(L"Start Windows Time service");
            return 1;
        }
        Sleep(1200);
    }

    wprintf(L"Requesting Windows Time resync...\n\n");
    if (w32tm_resync_raw(&result) != 0) {
        error_print_last(L"w32tm /resync");
        return 1;
    }

    if (result.raw[0] != L'\0') {
        wprintf(L"%ls\n", result.raw);
    }

    if (result.exit_code != 0) {
        fwprintf(stderr, L"Resync failed with exit code %lu.\n", (unsigned long)result.exit_code);
        print_resync_failure_hints();
        return 1;
    }

    wprintf(L"Resync requested.\n");
    print_w32tm_block(L"w32tm /query /status", w32tm_query_status_raw);
    return 0;
}

static int list_servers(void)
{
    w32time_config_t config;
    ntp_peer_list_t peers;
    int i;

    if (w32time_read_config(&config) != 0) {
        error_print_last(L"Read W32Time configuration");
        return 1;
    }

    format_section(L"NTP servers");
    format_field(L"Raw", config.ntp_server[0] ? config.ntp_server : L"(none)");

    if (config.ntp_server[0] == L'\0') {
        return 0;
    }

    if (ntp_parse_peer_list(config.ntp_server, &peers) != 0) {
        error_print_last(L"Parse NTP server list");
        return 1;
    }

    if (peers.count == 0) {
        format_field(L"Peers", L"(none)");
        return 0;
    }

    for (i = 0; i < peers.count; i++) {
        wprintf(
            L"  %d. %ls\n",
            i + 1,
            peers.peers[i].host);
        wprintf(
            L"     Flags: 0x%lx (%ls)\n",
            (unsigned long)peers.peers[i].flags,
            ntp_peer_flags_description(peers.peers[i].flags));
    }

    return 0;
}

static int test_server(const wchar_t *host)
{
    ntp_probe_result_t result;

    if (host == NULL || host[0] == L'\0') {
        fwprintf(stderr, L"Usage: gw32time servers test <host>\n");
        return 2;
    }

    wprintf(L"Testing NTP server: %ls\n\n", host);
    if (ntp_probe(host, 1500, &result) != 0) {
        error_print_last(L"NTP probe");
        return 1;
    }

    wprintf(L"DNS:      %ls\n", result.dns_ok ? L"OK" : L"Failed");
    wprintf(L"UDP/123:  %ls\n", result.ok ? L"OK" : L"Failed");
    if (result.ok) {
        wprintf(L"RTT:      %lu ms\n", (unsigned long)result.rtt_ms);
        wprintf(L"Offset:   %.0f ms\n", result.offset_ms);
        wprintf(L"Stratum:  %d\n", result.stratum);
        wprintf(L"Result:   OK\n");
        return 0;
    }

    wprintf(L"Result:   Failed\n");
    if (result.error[0] != L'\0') {
        wprintf(L"Reason:   %ls\n", result.error);
    }
    return 1;
}

static int is_option(const wchar_t *arg)
{
    return arg != NULL && arg[0] == L'-';
}

static int validate_server_host(const wchar_t *host)
{
    const wchar_t *p;

    if (host == NULL || host[0] == L'\0') {
        SetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }

    for (p = host; *p != L'\0'; p++) {
        if (*p <= L' ' || *p == L',' || *p == L'"' || *p == L'\\' || *p == L'/') {
            SetLastError(ERROR_INVALID_PARAMETER);
            return -1;
        }
    }

    return 0;
}

static int parse_server_args(int argc, wchar_t **argv, ntp_peer_list_t *out)
{
    int i;

    if (out == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }

    ZeroMemory(out, sizeof(*out));
    for (i = 3; i < argc; i++) {
        ntp_peer_t *peer;

        if (is_option(argv[i])) {
            continue;
        }

        if (validate_server_host(argv[i]) != 0) {
            return -1;
        }

        if (out->count >= NTP_MAX_PEERS) {
            SetLastError(ERROR_BUFFER_OVERFLOW);
            return -1;
        }

        peer = &out->peers[out->count];
        wcsncpy(peer->host, argv[i], (sizeof(peer->host) / sizeof(peer->host[0])) - 1);
        peer->host[(sizeof(peer->host) / sizeof(peer->host[0])) - 1] = L'\0';
        peer->flags = 0x8;
        peer->enabled = 1;
        out->count++;
    }

    if (out->count == 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }

    return 0;
}

static int print_servers_set_dry_run(int argc, wchar_t **argv)
{
    w32time_config_t config;
    ntp_peer_list_t desired;
    wchar_t formatted[1024];
    int dry_run = has_arg(argc, argv, L"--dry-run");
    int assume_yes = has_arg(argc, argv, L"--yes");
    int no_sync = has_arg(argc, argv, L"--no-sync");
    int force_domain = has_arg(argc, argv, L"--force-domain");
    int is_admin = 0;
    domain_info_t domain;
    w32tm_raw_result_t result;

    if (parse_server_args(argc, argv, &desired) != 0) {
        error_print_last(L"Parse server arguments");
        fwprintf(stderr, L"Usage: gw32time servers set <host...> [--dry-run] [--yes] [--no-sync] [--force-domain]\n");
        return 2;
    }

    if (ntp_format_peer_list(&desired, formatted, sizeof(formatted) / sizeof(formatted[0])) != 0) {
        error_print_last(L"Format NTP peer list");
        return 1;
    }

    if (w32time_read_config(&config) != 0) {
        error_print_last(L"Read W32Time configuration");
        return 1;
    }

    wprintf(L"Planned changes:\n\n");
    wprintf(L"NTP servers:\n");
    wprintf(L"  current: %ls\n", config.ntp_server[0] ? config.ntp_server : L"(none)");
    wprintf(L"  new:     %ls\n\n", formatted);
    wprintf(L"Sync mode:\n");
    wprintf(L"  current: %ls\n", config.type[0] ? config.type : L"unknown");
    wprintf(L"  new:     NTP/manual\n\n");
    wprintf(L"Actions:\n");
    wprintf(L"  - write NtpServer\n");
    wprintf(L"  - run w32tm /config /manualpeerlist:\"%ls\" /syncfromflags:manual /update\n", formatted);
    wprintf(L"  - restart w32time\n");
    if (!no_sync) {
        wprintf(L"  - run w32tm /resync\n");
    }

    if (domain_query(&domain) == 0 && domain.joined) {
        wprintf(L"\nDomain-joined machine detected");
        if (domain.name[0] != L'\0') {
            wprintf(L": %ls", domain.name);
        }
        wprintf(L".\n");
        wprintf(L"Manual NTP changes may be overwritten by policy or break domain expectations.\n");
        if (!force_domain) {
            wprintf(L"Use --force-domain to apply anyway.\n");
        }
    }

    if (dry_run) {
        return 0;
    }

    if (domain_query(&domain) == 0 && domain.joined && !force_domain) {
        fwprintf(stderr, L"\nRefusing manual NTP changes on a domain-joined machine.\n");
        fwprintf(stderr, L"Use --force-domain to override.\n");
        return 1;
    }

    if (privilege_is_admin(&is_admin) != 0 || !is_admin) {
        fwprintf(stderr, L"\nChanging NTP servers requires an elevated administrator token.\n");
        return 1;
    }

    if (!confirm_action(L"\nApply these changes?", assume_yes)) {
        wprintf(L"Apply cancelled.\n");
        return 1;
    }

    if (w32time_write_manual_servers(formatted) != 0) {
        error_print_last(L"Write W32Time server configuration");
        return 1;
    }

    if (w32tm_config_manual_peers_raw(formatted, &result) != 0) {
        error_print_last(L"w32tm /config");
        return 1;
    }
    if (result.raw[0] != L'\0') {
        wprintf(L"\n%ls\n", result.raw);
    }
    if (result.exit_code != 0) {
        fwprintf(stderr, L"w32tm /config failed with exit code %lu.\n", (unsigned long)result.exit_code);
        return 1;
    }

    if (svc_restart(L"w32time") != 0) {
        error_print_last(L"Restart Windows Time service");
        return 1;
    }

    if (!no_sync) {
        if (w32tm_resync_raw(&result) != 0) {
            error_print_last(L"w32tm /resync");
            return 1;
        }
        if (result.raw[0] != L'\0') {
            wprintf(L"%ls\n", result.raw);
        }
        if (result.exit_code != 0) {
            fwprintf(stderr, L"Resync failed with exit code %lu.\n", (unsigned long)result.exit_code);
            return 1;
        }
    }

    wprintf(L"Servers updated.\n");
    return 0;
}

static int poll_get(void)
{
    w32time_config_t config;

    if (w32time_read_config(&config) != 0) {
        error_print_last(L"Read W32Time configuration");
        return 1;
    }

    format_section(L"Polling interval");
    if (config.has_special_poll_interval) {
        format_field_dword(L"Special", config.special_poll_interval, L"sec");
    } else {
        format_field(L"Special", L"unknown");
    }

    if (config.has_min_poll_interval) {
        format_field_dword(L"Min", config.min_poll_interval, NULL);
    }

    if (config.has_max_poll_interval) {
        format_field_dword(L"Max", config.max_poll_interval, NULL);
    }

    return 0;
}

static int parse_dword_arg(const wchar_t *arg, DWORD *out)
{
    wchar_t *end;
    unsigned long value;

    if (arg == NULL || out == NULL || arg[0] == L'\0') {
        SetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }

    value = wcstoul(arg, &end, 10);
    if (*end != L'\0' || value == 0 || value > 0xffffffffUL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }

    *out = (DWORD)value;
    return 0;
}

static int validate_poll_interval(DWORD seconds, int force)
{
    if (seconds < 64 && !force) {
        fwprintf(stderr, L"Polling intervals below 64 seconds require --force.\n");
        return -1;
    }

    if (seconds < 256) {
        fwprintf(stderr, L"Warning: polling interval is aggressive.\n");
    } else if (seconds > 86400) {
        fwprintf(stderr, L"Warning: polling interval is longer than one day.\n");
    }

    return 0;
}

static int poll_set(int argc, wchar_t **argv)
{
    DWORD seconds;
    w32time_config_t config;
    int dry_run = has_arg(argc, argv, L"--dry-run");
    int assume_yes = has_arg(argc, argv, L"--yes");
    int force = has_arg(argc, argv, L"--force");
    int is_admin = 0;
    w32tm_raw_result_t result;

    if (argc < 4 || is_option(argv[3])) {
        fwprintf(stderr, L"Usage: gw32time poll set <seconds> [--dry-run] [--yes] [--force]\n");
        return 2;
    }

    if (parse_dword_arg(argv[3], &seconds) != 0) {
        error_print_last(L"Parse polling interval");
        return 2;
    }

    if (validate_poll_interval(seconds, force) != 0) {
        return 2;
    }

    if (w32time_read_config(&config) != 0) {
        error_print_last(L"Read W32Time configuration");
        return 1;
    }

    wprintf(L"Planned changes:\n\n");
    wprintf(L"Polling interval:\n");
    if (config.has_special_poll_interval) {
        wprintf(L"  current: %lu sec\n", (unsigned long)config.special_poll_interval);
    } else {
        wprintf(L"  current: unknown\n");
    }
    wprintf(L"  new:     %lu sec\n\n", (unsigned long)seconds);
    wprintf(L"Actions:\n");
    wprintf(L"  - write SpecialPollInterval\n");
    wprintf(L"  - run w32tm /config /update\n");
    wprintf(L"  - restart w32time\n");

    if (dry_run) {
        return 0;
    }

    if (privilege_is_admin(&is_admin) != 0 || !is_admin) {
        fwprintf(stderr, L"\nChanging polling interval requires an elevated administrator token.\n");
        return 1;
    }

    if (!confirm_action(L"\nApply these changes?", assume_yes)) {
        wprintf(L"Apply cancelled.\n");
        return 1;
    }

    if (w32time_write_poll_interval(seconds) != 0) {
        error_print_last(L"Write SpecialPollInterval");
        return 1;
    }

    if (w32tm_config_update_raw(&result) != 0) {
        error_print_last(L"w32tm /config /update");
        return 1;
    }
    if (result.raw[0] != L'\0') {
        wprintf(L"\n%ls\n", result.raw);
    }
    if (result.exit_code != 0) {
        fwprintf(stderr, L"w32tm /config /update failed with exit code %lu.\n", (unsigned long)result.exit_code);
        return 1;
    }

    if (svc_restart(L"w32time") != 0) {
        error_print_last(L"Restart Windows Time service");
        return 1;
    }

    wprintf(L"Polling interval updated.\n");
    return 0;
}

static int preset_dry_run(int argc, wchar_t **argv)
{
    const wchar_t *name;
    preset_t preset;
    int dry_run = has_arg(argc, argv, L"--dry-run");
    int assume_yes = has_arg(argc, argv, L"--yes");
    int is_admin = 0;
    w32tm_raw_result_t result;

    if (argc < 3) {
        fwprintf(stderr, L"Usage: gw32time preset desktop|lab-fast|windows-default|domain [--dry-run] [--yes]\n");
        return 2;
    }

    name = argv[2];
    if (preset_lookup(name, &preset) != 0) {
        fwprintf(stderr, L"Unknown preset: %ls\n", name);
        return 2;
    }

    wprintf(L"Preset plan: %ls\n\n", name);
    wprintf(L"Sync mode: %ls\n", preset.display_mode);
    wprintf(L"Servers:   %ls\n", preset.display_servers);
    if (preset.config.has_special_poll_interval) {
        wprintf(L"Poll:      %lu sec\n", (unsigned long)preset.config.special_poll_interval);
    } else {
        wprintf(L"Poll:      unchanged\n");
    }
    wprintf(L"\nActions:\n");
    wprintf(L"  - update W32Time registry values\n");
    wprintf(L"  - run w32tm /config /update\n");
    wprintf(L"  - restart w32time\n");

    if (dry_run) {
        return 0;
    }

    if (privilege_is_admin(&is_admin) != 0 || !is_admin) {
        fwprintf(stderr, L"\nApplying presets requires an elevated administrator token.\n");
        return 1;
    }

    if (!confirm_action(L"\nApply preset?", assume_yes)) {
        wprintf(L"Preset cancelled.\n");
        return 1;
    }

    if (w32time_write_config(&preset.config) != 0) {
        error_print_last(L"Write preset configuration");
        return 1;
    }

    if (w32tm_config_update_raw(&result) != 0) {
        error_print_last(L"w32tm /config /update");
        return 1;
    }
    if (result.raw[0] != L'\0') {
        wprintf(L"\n%ls\n", result.raw);
    }
    if (result.exit_code != 0) {
        fwprintf(stderr, L"w32tm /config /update failed with exit code %lu.\n", (unsigned long)result.exit_code);
        return 1;
    }

    if (svc_restart(L"w32time") != 0) {
        error_print_last(L"Restart Windows Time service");
        return 1;
    }

    wprintf(L"Preset applied.\n");
    return 0;
}

static int preset_list(void)
{
    int i;

    wprintf(L"Presets:\n");
    for (i = 0; i < preset_count(); i++) {
        wprintf(L"  %-16ls %ls\n", preset_name_at(i), preset_description_at(i));
    }
    return 0;
}

static int backup_config(const wchar_t *path)
{
    w32time_config_t config;

    if (path == NULL || path[0] == L'\0') {
        fwprintf(stderr, L"Usage: gw32time backup <file>\n");
        return 2;
    }

    if (w32time_read_config(&config) != 0) {
        error_print_last(L"Read W32Time configuration");
        return 1;
    }

    if (config_file_write(path, &config) != 0) {
        error_print_last(L"Write backup file");
        return 1;
    }

    wprintf(L"Backup written: %ls\n", path);
    return 0;
}

static int restore_config(int argc, wchar_t **argv)
{
    w32time_config_t current;
    w32time_config_t desired;
    int dry_run = has_arg(argc, argv, L"--dry-run");
    int assume_yes = has_arg(argc, argv, L"--yes");
    int is_admin = 0;
    w32tm_raw_result_t result;

    if (argc < 3 || argv[2][0] == L'\0') {
        fwprintf(stderr, L"Usage: gw32time restore <file> [--dry-run] [--yes]\n");
        return 2;
    }

    if (config_file_read(argv[2], &desired) != 0) {
        error_print_last(L"Read backup file");
        return 1;
    }

    if (w32time_read_config(&current) != 0) {
        error_print_last(L"Read W32Time configuration");
        return 1;
    }

    wprintf(L"Planned restore:\n\n");
    wprintf(L"Type:\n");
    wprintf(L"  current: %ls\n", current.type[0] ? current.type : L"unknown");
    wprintf(L"  new:     %ls\n\n", desired.type[0] ? desired.type : L"unknown");
    wprintf(L"NTP servers:\n");
    wprintf(L"  current: %ls\n", current.ntp_server[0] ? current.ntp_server : L"(none)");
    wprintf(L"  new:     %ls\n\n", desired.ntp_server[0] ? desired.ntp_server : L"(none)");
    wprintf(L"SpecialPollInterval:\n");
    if (current.has_special_poll_interval) {
        wprintf(L"  current: %lu sec\n", (unsigned long)current.special_poll_interval);
    } else {
        wprintf(L"  current: unknown\n");
    }
    if (desired.has_special_poll_interval) {
        wprintf(L"  new:     %lu sec\n", (unsigned long)desired.special_poll_interval);
    } else {
        wprintf(L"  new:     unchanged\n");
    }
    wprintf(L"\nActions:\n");
    wprintf(L"  - restore registry values from backup\n");
    wprintf(L"  - run w32tm /config /update\n");
    wprintf(L"  - restart w32time\n");

    if (dry_run) {
        return 0;
    }

    if (privilege_is_admin(&is_admin) != 0 || !is_admin) {
        fwprintf(stderr, L"\nRestoring configuration requires an elevated administrator token.\n");
        return 1;
    }

    if (!confirm_action(L"\nApply restore?", assume_yes)) {
        wprintf(L"Restore cancelled.\n");
        return 1;
    }

    if (w32time_write_config(&desired) != 0) {
        error_print_last(L"Write restored W32Time configuration");
        return 1;
    }

    if (w32tm_config_update_raw(&result) != 0) {
        error_print_last(L"w32tm /config /update");
        return 1;
    }
    if (result.raw[0] != L'\0') {
        wprintf(L"\n%ls\n", result.raw);
    }
    if (result.exit_code != 0) {
        fwprintf(stderr, L"w32tm /config /update failed with exit code %lu.\n", (unsigned long)result.exit_code);
        return 1;
    }

    if (svc_restart(L"w32time") != 0) {
        error_print_last(L"Restart Windows Time service");
        return 1;
    }

    wprintf(L"Configuration restored.\n");
    return 0;
}

static void menu_pause(void)
{
    wchar_t input[8];

    wprintf(L"\nPress Enter to return to the menu.");
    fgetws(input, sizeof(input) / sizeof(input[0]), stdin);
}

static wchar_t *menu_next_token(wchar_t **cursor)
{
    wchar_t *start;

    while (**cursor == L' ' || **cursor == L'\t' || **cursor == L'\r' || **cursor == L'\n') {
        (*cursor)++;
    }

    if (**cursor == L'\0') {
        return NULL;
    }

    start = *cursor;
    while (**cursor != L'\0' && **cursor != L' ' && **cursor != L'\t' && **cursor != L'\r' && **cursor != L'\n') {
        (*cursor)++;
    }

    if (**cursor != L'\0') {
        **cursor = L'\0';
        (*cursor)++;
    }

    return start;
}

static int menu_edit_servers(void)
{
    wchar_t line[1024];
    wchar_t *cursor;
    wchar_t *argv[24];
    int argc = 3;
    wchar_t *token;

    wprintf(L"\nEdit NTP servers\n\n");
    list_servers();
    wprintf(L"\nEnter server hosts separated by spaces.\n");
    wprintf(L"Leave empty to cancel.\n");
    wprintf(L"Servers: ");

    if (fgetws(line, sizeof(line) / sizeof(line[0]), stdin) == NULL) {
        return 1;
    }

    argv[0] = L"gw32time";
    argv[1] = L"servers";
    argv[2] = L"set";
    cursor = line;
    while ((token = menu_next_token(&cursor)) != NULL) {
        if (argc >= (int)(sizeof(argv) / sizeof(argv[0]))) {
            fwprintf(stderr, L"Too many servers entered.\n");
            return 2;
        }
        argv[argc++] = token;
    }

    if (argc == 3) {
        wprintf(L"Edit cancelled.\n");
        return 0;
    }

    return print_servers_set_dry_run(argc, argv);
}

static int menu_test_server(void)
{
    wchar_t line[256];
    wchar_t *cursor;
    wchar_t *host;

    wprintf(L"\nTest NTP server\n\n");
    wprintf(L"Host: ");
    if (fgetws(line, sizeof(line) / sizeof(line[0]), stdin) == NULL) {
        return 1;
    }

    cursor = line;
    host = menu_next_token(&cursor);
    if (host == NULL) {
        wprintf(L"Test cancelled.\n");
        return 0;
    }

    if (menu_next_token(&cursor) != NULL) {
        fwprintf(stderr, L"Enter one server host only.\n");
        return 2;
    }

    return test_server(host);
}

static int menu_apply_preset(void)
{
    wchar_t line[64];
    wchar_t *cursor;
    wchar_t *name;
    wchar_t *argv_preset[] = { L"gw32time", L"preset", NULL };

    wprintf(L"\nApply preset\n\n");
    preset_list();
    wprintf(L"\nPreset: ");
    if (fgetws(line, sizeof(line) / sizeof(line[0]), stdin) == NULL) {
        return 1;
    }

    cursor = line;
    name = menu_next_token(&cursor);
    if (name == NULL) {
        wprintf(L"Preset cancelled.\n");
        return 0;
    }

    if (menu_next_token(&cursor) != NULL) {
        fwprintf(stderr, L"Enter one preset name only.\n");
        return 2;
    }

    argv_preset[2] = name;
    return preset_dry_run(3, argv_preset);
}

static int menu_backup_config(void)
{
    wchar_t line[260];
    wchar_t *cursor;
    wchar_t *path;

    wprintf(L"\nBackup configuration\n\n");
    wprintf(L"File: ");
    if (fgetws(line, sizeof(line) / sizeof(line[0]), stdin) == NULL) {
        return 1;
    }

    cursor = line;
    path = menu_next_token(&cursor);
    if (path == NULL) {
        wprintf(L"Backup cancelled.\n");
        return 0;
    }

    if (menu_next_token(&cursor) != NULL) {
        fwprintf(stderr, L"Enter one backup file path only.\n");
        return 2;
    }

    return backup_config(path);
}

static int menu_restore_config(void)
{
    wchar_t line[260];
    wchar_t *cursor;
    wchar_t *path;
    wchar_t *argv_restore[] = { L"gw32time", L"restore", NULL, L"--dry-run" };

    wprintf(L"\nRestore configuration\n\n");
    wprintf(L"File: ");
    if (fgetws(line, sizeof(line) / sizeof(line[0]), stdin) == NULL) {
        return 1;
    }

    cursor = line;
    path = menu_next_token(&cursor);
    if (path == NULL) {
        wprintf(L"Restore cancelled.\n");
        return 0;
    }

    if (menu_next_token(&cursor) != NULL) {
        fwprintf(stderr, L"Enter one restore file path only.\n");
        return 2;
    }

    argv_restore[2] = path;
    return restore_config(4, argv_restore);
}

static int run_menu(void)
{
    wchar_t input[32];

    for (;;) {
        wprintf(L"\n");
        print_status(0, 0);
        print_runtime_summary();
        wprintf(L"\n");
        print_health();
        wprintf(L"\nActions:\n");
        wprintf(L"  1. Show full status\n");
        wprintf(L"  2. Sync now\n");
        wprintf(L"  3. Edit NTP servers\n");
        wprintf(L"  4. Test NTP server\n");
        wprintf(L"  5. Apply preset\n");
        wprintf(L"  6. Backup config\n");
        wprintf(L"  7. Preview restore\n");
        wprintf(L"  8. Show diagnostics\n");
        wprintf(L"  0. Exit\n\n");
        wprintf(L"Select: ");

        if (fgetws(input, sizeof(input) / sizeof(input[0]), stdin) == NULL) {
            return 0;
        }

        if (input[0] == L'0') {
            return 0;
        } else if (input[0] == L'1') {
            wprintf(L"\nFull status\n\n");
            print_status(0, 0);
            wprintf(L"\n");
            poll_get();
            menu_pause();
        } else if (input[0] == L'2') {
            wchar_t *argv_sync[] = { L"gw32time", L"sync" };
            run_sync_now(2, argv_sync);
            menu_pause();
        } else if (input[0] == L'3') {
            menu_edit_servers();
            menu_pause();
        } else if (input[0] == L'4') {
            menu_test_server();
            menu_pause();
        } else if (input[0] == L'5') {
            menu_apply_preset();
            menu_pause();
        } else if (input[0] == L'6') {
            menu_backup_config();
            menu_pause();
        } else if (input[0] == L'7') {
            menu_restore_config();
            menu_pause();
        } else if (input[0] == L'8') {
            print_diag(0);
            menu_pause();
        } else {
            wprintf(L"Unknown selection.\n");
            menu_pause();
        }
    }
}

int cli_dispatch(int argc, wchar_t **argv)
{
    if (argc >= 2 && arg_is(argv[1], L"__set-time")) {
        return run_internal_set_time(argc, argv);
    }
    if (argc >= 2 && arg_is(argv[1], L"__sync-now")) {
        return run_internal_sync_now();
    }
    if (argc >= 2 && arg_is(argv[1], L"__apply-servers")) {
        return run_internal_apply_servers(argc, argv);
    }
    if (argc >= 2 && arg_is(argv[1], L"__restore-config")) {
        return run_internal_restore_config(argc, argv);
    }
    if (argc >= 2 && arg_is(argv[1], L"__svc")) {
        return run_internal_service_cmd(argc, argv);
    }
    if (argc >= 2 && arg_is(argv[1], L"__set-poll")) {
        return run_internal_set_poll(argc, argv);
    }
    if (argc >= 2 && arg_is(argv[1], L"__uac-ping")) {
        return run_internal_uac_ping();
    }
    if (argc >= 2 && arg_is(argv[1], L"__helper")) {
        return run_internal_helper_server(argc, argv);
    }

    if (argc <= 1) {
        FreeConsole();
        return gui_launch(GetModuleHandleW(NULL));
    }

    if (arg_is(argv[1], L"--help") || arg_is(argv[1], L"-h")) {
        print_help();
        return 0;
    }

    if (arg_is(argv[1], L"--version")) {
        print_version();
        return 0;
    }

    if (arg_is(argv[1], L"--admin")) {
        return print_admin_status();
    }

    if (arg_is(argv[1], L"menu")) {
        return run_menu();
    }

    if (arg_is(argv[1], L"status")) {
        return print_status(has_arg(argc, argv, L"--raw"), has_arg(argc, argv, L"--verbose"));
    }

    if (arg_is(argv[1], L"gui")) {
        FreeConsole();
        return gui_launch(GetModuleHandleW(NULL));
    }

    if (arg_is(argv[1], L"runtime")) {
        print_runtime_summary();
        return 0;
    }

    if (arg_is(argv[1], L"diag")) {
        return print_diag(has_arg(argc, argv, L"--raw"));
    }

    if (arg_is(argv[1], L"health")) {
        return print_health();
    }

    if (arg_is(argv[1], L"sync")) {
        return run_sync_now(argc, argv);
    }

    if (arg_is(argv[1], L"service")) {
        if (argc >= 3 && arg_is(argv[2], L"status")) {
            return print_service_status();
        }

        if (argc >= 3 && arg_is(argv[2], L"start")) {
            return service_start_command();
        }

        if (argc >= 3 && arg_is(argv[2], L"restart")) {
            return service_restart_command();
        }

        fwprintf(stderr, L"Usage: gw32time service status|start|restart\n");
        return 2;
    }

    if (arg_is(argv[1], L"servers")) {
        if (argc >= 3 && arg_is(argv[2], L"list")) {
            return list_servers();
        }

        if (argc >= 3 && arg_is(argv[2], L"test")) {
            if (argc >= 4) {
                return test_server(argv[3]);
            }

            fwprintf(stderr, L"Usage: gw32time servers test <host>\n");
            return 2;
        }

        if (argc >= 3 && arg_is(argv[2], L"set")) {
            return print_servers_set_dry_run(argc, argv);
        }

        fwprintf(stderr, L"Usage: gw32time servers list\n");
        fwprintf(stderr, L"       gw32time servers test <host>\n");
        fwprintf(stderr, L"       gw32time servers set <host...> [--dry-run] [--yes] [--no-sync] [--force-domain]\n");
        return 2;
    }

    if (arg_is(argv[1], L"poll")) {
        if (argc >= 3 && arg_is(argv[2], L"get")) {
            return poll_get();
        }

        if (argc >= 3 && arg_is(argv[2], L"set")) {
            return poll_set(argc, argv);
        }

        fwprintf(stderr, L"Usage: gw32time poll get\n");
        fwprintf(stderr, L"       gw32time poll set <seconds> [--dry-run] [--yes] [--force]\n");
        return 2;
    }

    if (arg_is(argv[1], L"preset")) {
        if (argc >= 3 && arg_is(argv[2], L"list")) {
            return preset_list();
        }
        return preset_dry_run(argc, argv);
    }

    if (arg_is(argv[1], L"sane")) {
        wchar_t *argv_sane[16];
        int i;
        int sane_argc = 3;

        argv_sane[0] = argv[0];
        argv_sane[1] = L"preset";
        argv_sane[2] = L"desktop";
        for (i = 2; i < argc && sane_argc < (int)(sizeof(argv_sane) / sizeof(argv_sane[0])); i++) {
            argv_sane[sane_argc++] = argv[i];
        }
        return preset_dry_run(sane_argc, argv_sane);
    }

    if (arg_is(argv[1], L"backup")) {
        if (argc >= 3) {
            return backup_config(argv[2]);
        }

        fwprintf(stderr, L"Usage: gw32time backup <file>\n");
        return 2;
    }

    if (arg_is(argv[1], L"restore")) {
        return restore_config(argc, argv);
    }

    fwprintf(stderr, L"Unknown command: %ls\n", argv[1]);
    fwprintf(stderr, L"Run 'gw32time --help' for usage.\n");
    return 2;
}
