#include "cli.h"

#include <stdio.h>
#include <wchar.h>

#include "../core/diagnostics.h"
#include "../core/error.h"
#include "../core/privilege.h"
#include "../core/service.h"
#include "../core/w32time.h"
#include "../core/w32tm.h"

#define GW32TIME_VERSION L"0.0.1"

static int arg_is(const wchar_t *arg, const wchar_t *value)
{
    return wcscmp(arg, value) == 0;
}

static void print_version(void)
{
    wprintf(L"gw32time %ls\n", GW32TIME_VERSION);
}

static void print_help(void)
{
    wprintf(L"GW32TIME - Windows Time Service control frontend\n\n");
    wprintf(L"Usage:\n");
    wprintf(L"  gw32time --help\n");
    wprintf(L"  gw32time --version\n");
    wprintf(L"  gw32time status [--raw]\n");
    wprintf(L"  gw32time health\n");
    wprintf(L"  gw32time diag\n");
    wprintf(L"  gw32time service status\n");
    wprintf(L"  gw32time servers list\n");
    wprintf(L"  gw32time sync [--yes]\n");
}

static int print_admin_status(void)
{
    int is_admin = 0;
    int rc = privilege_is_admin(&is_admin);
    if (rc != 0) {
        fwprintf(stderr, L"Admin: unknown\n");
        return rc;
    }

    wprintf(L"Admin: %ls\n", is_admin ? L"yes" : L"no");
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

    wprintf(L"Service:\n");
    wprintf(L"  Name:       w32time\n");
    wprintf(L"  State:      %ls\n", svc_state_name(state));
    wprintf(L"  Start type: %ls\n", svc_start_type_name(start_type));
    return 0;
}

static void print_admin_block(void)
{
    int is_admin = 0;

    wprintf(L"\nPrivileges:\n");
    if (privilege_is_admin(&is_admin) != 0) {
        wprintf(L"  Admin:   unknown\n");
        return;
    }

    wprintf(L"  Admin:   %ls\n", is_admin ? L"yes" : L"no");
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

static int print_status(int raw)
{
    svc_state_t state = SVC_STATE_UNKNOWN;
    svc_start_type_t start_type = SVC_START_UNKNOWN;
    w32time_config_t config;

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

    wprintf(L"GW32TIME\n");
    wprintf(L"Graphical UI for Windows Time Service\n\n");
    wprintf(L"Service:  %ls\n", svc_state_name(state));
    wprintf(L"Start:    %ls\n", svc_start_type_name(start_type));
    wprintf(L"Type:     %ls\n", config.type[0] ? config.type : L"unknown");
    print_server_summary(config.ntp_server);
    if (config.has_special_poll_interval) {
        wprintf(L"Poll:     %lu sec\n", (unsigned long)config.special_poll_interval);
    } else {
        wprintf(L"Poll:     unknown\n");
    }
    print_admin_block();

    if (raw) {
        print_w32tm_block(L"w32tm /query /status", w32tm_query_status_raw);
    }

    return 0;
}

static int print_diag(void)
{
    int rc;
    health_t health;

    wprintf(L"Windows Time Diagnostics\n");
    wprintf(L"========================\n\n");

    rc = print_status(0);
    if (rc != 0) {
        return rc;
    }

    if (diagnostics_evaluate_health(&health) == 0) {
        wprintf(L"\nHealth:\n");
        wprintf(L"  State:  %ls\n", health_state_name(health.state));
        wprintf(L"  Reason: %ls\n", health.reason);
    }

    print_w32tm_block(L"w32tm /query /status", w32tm_query_status_raw);
    print_w32tm_block(L"w32tm /query /peers", w32tm_query_peers_raw);
    print_w32tm_block(L"w32tm /query /configuration", w32tm_query_configuration_raw);
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
        fwprintf(stderr, L"Try 'gw32time diag' for service, peer, and configuration details.\n");
        return 1;
    }

    wprintf(L"Resync requested.\n");
    print_w32tm_block(L"w32tm /query /status", w32tm_query_status_raw);
    return 0;
}

static const wchar_t *peer_flags_description(DWORD flags)
{
    switch (flags) {
    case 0x1:
        return L"special poll interval";
    case 0x8:
        return L"client";
    case 0x9:
        return L"client, special poll interval";
    default:
        return L"custom Windows Time flags";
    }
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

    wprintf(L"NTP servers:\n");
    wprintf(L"  Raw: %ls\n", config.ntp_server[0] ? config.ntp_server : L"(none)");

    if (config.ntp_server[0] == L'\0') {
        return 0;
    }

    if (ntp_parse_peer_list(config.ntp_server, &peers) != 0) {
        error_print_last(L"Parse NTP server list");
        return 1;
    }

    if (peers.count == 0) {
        wprintf(L"  (none)\n");
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
            peer_flags_description(peers.peers[i].flags));
    }

    return 0;
}


int cli_dispatch(int argc, wchar_t **argv)
{
    if (argc <= 1) {
        print_help();
        return 0;
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

    if (arg_is(argv[1], L"status")) {
        return print_status(argc >= 3 && arg_is(argv[2], L"--raw"));
    }

    if (arg_is(argv[1], L"diag")) {
        return print_diag();
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

        fwprintf(stderr, L"Usage: gw32time service status\n");
        return 2;
    }

    if (arg_is(argv[1], L"servers")) {
        if (argc >= 3 && arg_is(argv[2], L"list")) {
            return list_servers();
        }

        fwprintf(stderr, L"Usage: gw32time servers list\n");
        return 2;
    }

    fwprintf(stderr, L"Unknown command: %ls\n", argv[1]);
    fwprintf(stderr, L"Run 'gw32time --help' for usage.\n");
    return 2;
}
