#include "cli.h"

#include <stdio.h>
#include <wchar.h>

#include "../core/error.h"
#include "../core/privilege.h"
#include "../core/service.h"

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
    wprintf(L"  gw32time service status\n");
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

    if (arg_is(argv[1], L"service")) {
        if (argc >= 3 && arg_is(argv[2], L"status")) {
            return print_service_status();
        }

        fwprintf(stderr, L"Usage: gw32time service status\n");
        return 2;
    }

    fwprintf(stderr, L"Unknown command: %ls\n", argv[1]);
    fwprintf(stderr, L"Run 'gw32time --help' for usage.\n");
    return 2;
}
