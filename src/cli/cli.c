#include "cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#include "../core/config_file.h"
#include "../core/diagnostics.h"
#include "../core/error.h"
#include "../core/ntp_probe.h"
#include "../core/privilege.h"
#include "../core/service.h"
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
    wprintf(L"gw32time %ls\n", GW32TIME_VERSION);
}

static void print_help(void)
{
    wprintf(L"GW32TIME - Windows Time Service control frontend\n\n");
    wprintf(L"Usage:\n");
    wprintf(L"  gw32time --help\n");
    wprintf(L"  gw32time --version\n");
    wprintf(L"  gw32time status [--raw]\n");
    wprintf(L"  gw32time gui\n");
    wprintf(L"  gw32time health\n");
    wprintf(L"  gw32time diag\n");
    wprintf(L"  gw32time service status|start|restart\n");
    wprintf(L"  gw32time servers list\n");
    wprintf(L"  gw32time servers test <host>\n");
    wprintf(L"  gw32time servers set <host...> [--dry-run] [--yes] [--no-sync]\n");
    wprintf(L"  gw32time poll get\n");
    wprintf(L"  gw32time poll set <seconds> [--dry-run] [--yes] [--force]\n");
    wprintf(L"  gw32time preset desktop|windows-default|domain [--dry-run] [--yes]\n");
    wprintf(L"  gw32time backup <file>\n");
    wprintf(L"  gw32time restore <file> [--dry-run] [--yes]\n");
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
    print_peer_summary(config.ntp_server);
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

        if (argv[i][0] == L'\0' || wcschr(argv[i], L' ') != NULL || wcschr(argv[i], L'\t') != NULL) {
            SetLastError(ERROR_INVALID_PARAMETER);
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
    int is_admin = 0;
    w32tm_raw_result_t result;

    if (parse_server_args(argc, argv, &desired) != 0) {
        error_print_last(L"Parse server arguments");
        fwprintf(stderr, L"Usage: gw32time servers set <host...> [--dry-run] [--yes] [--no-sync]\n");
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

    if (dry_run) {
        return 0;
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

    wprintf(L"Polling interval:\n");
    if (config.has_special_poll_interval) {
        wprintf(L"  SpecialPollInterval: %lu sec\n", (unsigned long)config.special_poll_interval);
    } else {
        wprintf(L"  SpecialPollInterval: unknown\n");
    }

    if (config.has_min_poll_interval) {
        wprintf(L"  MinPollInterval:     %lu\n", (unsigned long)config.min_poll_interval);
    }

    if (config.has_max_poll_interval) {
        wprintf(L"  MaxPollInterval:     %lu\n", (unsigned long)config.max_poll_interval);
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
    const wchar_t *servers = NULL;
    const wchar_t *mode = L"NTP/manual";
    DWORD poll = 1024;
    int dry_run = has_arg(argc, argv, L"--dry-run");
    int assume_yes = has_arg(argc, argv, L"--yes");
    int is_admin = 0;
    w32time_config_t desired;
    w32tm_raw_result_t result;

    if (argc < 3) {
        fwprintf(stderr, L"Usage: gw32time preset desktop|windows-default|domain [--dry-run] [--yes]\n");
        return 2;
    }

    ZeroMemory(&desired, sizeof(desired));
    name = argv[2];
    if (arg_is(name, L"desktop")) {
        servers = L"time.cloudflare.com,0x8 pool.ntp.org,0x8 time.google.com,0x8";
        poll = 1024;
        wcscpy(desired.type, L"NTP");
        wcscpy(desired.ntp_server, servers);
        desired.special_poll_interval = poll;
        desired.has_special_poll_interval = 1;
    } else if (arg_is(name, L"windows-default")) {
        servers = L"time.windows.com,0x8";
        poll = 604800;
        wcscpy(desired.type, L"NTP");
        wcscpy(desired.ntp_server, servers);
        desired.special_poll_interval = poll;
        desired.has_special_poll_interval = 1;
    } else if (arg_is(name, L"domain")) {
        servers = L"(unchanged)";
        mode = L"NT5DS";
        poll = 0;
        wcscpy(desired.type, L"NT5DS");
    } else {
        fwprintf(stderr, L"Unknown preset: %ls\n", name);
        return 2;
    }

    wprintf(L"Preset plan: %ls\n\n", name);
    wprintf(L"Sync mode: %ls\n", mode);
    wprintf(L"Servers:   %ls\n", servers);
    if (poll != 0) {
        wprintf(L"Poll:      %lu sec\n", (unsigned long)poll);
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

    if (w32time_write_config(&desired) != 0) {
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

    if (arg_is(argv[1], L"gui")) {
        return gui_launch(GetModuleHandleW(NULL));
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
        fwprintf(stderr, L"       gw32time servers set <host...> [--dry-run] [--yes] [--no-sync]\n");
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
        return preset_dry_run(argc, argv);
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
