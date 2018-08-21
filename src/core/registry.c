#include "registry.h"

int reg_read_dword(HKEY root, const wchar_t *path, const wchar_t *name, DWORD *out)
{
    HKEY key;
    DWORD type = 0;
    DWORD value = 0;
    DWORD size = sizeof(value);
    LONG rc;

    if (path == NULL || name == NULL || out == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }

    rc = RegOpenKeyExW(root, path, 0, KEY_READ, &key);
    if (rc != ERROR_SUCCESS) {
        SetLastError((DWORD)rc);
        return -1;
    }

    rc = RegQueryValueExW(key, name, NULL, &type, (LPBYTE)&value, &size);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS) {
        SetLastError((DWORD)rc);
        return -1;
    }

    if (type != REG_DWORD || size != sizeof(value)) {
        SetLastError(ERROR_INVALID_DATATYPE);
        return -1;
    }

    *out = value;
    return 0;
}

int reg_read_string(HKEY root, const wchar_t *path, const wchar_t *name, wchar_t *buf, size_t chars)
{
    HKEY key;
    DWORD type = 0;
    DWORD size;
    LONG rc;

    if (path == NULL || name == NULL || buf == NULL || chars == 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }

    buf[0] = L'\0';
    if (chars > ((DWORD)-1) / sizeof(wchar_t)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }

    size = (DWORD)(chars * sizeof(wchar_t));
    rc = RegOpenKeyExW(root, path, 0, KEY_READ, &key);
    if (rc != ERROR_SUCCESS) {
        SetLastError((DWORD)rc);
        return -1;
    }

    rc = RegQueryValueExW(key, name, NULL, &type, (LPBYTE)buf, &size);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS) {
        SetLastError((DWORD)rc);
        buf[0] = L'\0';
        return -1;
    }

    if (type != REG_SZ && type != REG_EXPAND_SZ) {
        SetLastError(ERROR_INVALID_DATATYPE);
        buf[0] = L'\0';
        return -1;
    }

    buf[chars - 1] = L'\0';
    return 0;
}
