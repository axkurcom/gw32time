#ifndef GW32TIME_FORMAT_H
#define GW32TIME_FORMAT_H

#include <windows.h>

void format_title(void);
void format_section(const wchar_t *name);
void format_field(const wchar_t *name, const wchar_t *value);
void format_field_dword(const wchar_t *name, DWORD value, const wchar_t *suffix);

#endif
