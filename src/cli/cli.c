#include "cli.h"

#include <stdio.h>
#include <wchar.h>

#include "../core/privilege.h"

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
    wprintf(L"  gw32time --version\n\n");
    wprintf(L"Commands will be added in small milestones.\n");
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

    fwprintf(stderr, L"Unknown command: %ls\n", argv[1]);
    fwprintf(stderr, L"Run 'gw32time --help' for usage.\n");
    return 2;
}
