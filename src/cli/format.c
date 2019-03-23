#include "format.h"

#include <stdio.h>

void format_title(void)
{
    wprintf(L"GW32TIME\n");
    wprintf(L"Graphical UI for Windows Time Service\n");
}

void format_section(const wchar_t *name)
{
    wprintf(L"\n%ls:\n", name);
}

void format_field(const wchar_t *name, const wchar_t *value)
{
    wprintf(L"  %-10ls %ls\n", name, value != NULL && value[0] != L'\0' ? value : L"unknown");
}

void format_field_dword(const wchar_t *name, DWORD value, const wchar_t *suffix)
{
    wprintf(L"  %-10ls %lu", name, (unsigned long)value);
    if (suffix != NULL && suffix[0] != L'\0') {
        wprintf(L" %ls", suffix);
    }
    wprintf(L"\n");
}
