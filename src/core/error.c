#include "error.h"

#include <stdio.h>

void error_format_win32(DWORD code, wchar_t *buf, size_t chars)
{
    DWORD flags;
    DWORD written;

    if (buf == NULL || chars == 0) {
        return;
    }

    buf[0] = L'\0';
    flags = FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    written = FormatMessageW(
        flags,
        NULL,
        code,
        0,
        buf,
        (DWORD)chars,
        NULL);

    if (written == 0) {
        _snwprintf(buf, chars, L"Win32 error %lu", (unsigned long)code);
        buf[chars - 1] = L'\0';
    }
}

void error_print_last(const wchar_t *prefix)
{
    wchar_t message[512];
    DWORD code = GetLastError();

    error_format_win32(code, message, sizeof(message) / sizeof(message[0]));

    if (prefix != NULL && prefix[0] != L'\0') {
        fwprintf(stderr, L"%ls: %ls\n", prefix, message);
        return;
    }

    fwprintf(stderr, L"%ls\n", message);
}
