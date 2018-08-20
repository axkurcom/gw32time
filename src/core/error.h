#ifndef GW32TIME_ERROR_H
#define GW32TIME_ERROR_H

#include <stddef.h>
#include <windows.h>

void error_format_win32(DWORD code, wchar_t *buf, size_t chars);
void error_print_last(const wchar_t *prefix);

#endif
