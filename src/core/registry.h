#ifndef GW32TIME_REGISTRY_H
#define GW32TIME_REGISTRY_H

#include <stddef.h>
#include <windows.h>

int reg_read_dword(HKEY root, const wchar_t *path, const wchar_t *name, DWORD *out);
int reg_read_string(HKEY root, const wchar_t *path, const wchar_t *name, wchar_t *buf, size_t chars);

#endif
