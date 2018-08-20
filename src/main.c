#include "cli/cli.h"

int wmain(int argc, wchar_t **argv)
{
    return cli_dispatch(argc, argv);
}
