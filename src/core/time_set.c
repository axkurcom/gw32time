#include "time_set.h"

#include <windows.h>

static int with_systemtime_privilege(BOOL enable)
{
    HANDLE token = NULL;
    TOKEN_PRIVILEGES tp;
    LUID luid;
    BOOL ok = FALSE;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        return -1;
    }
    if (!LookupPrivilegeValueW(NULL, SE_SYSTEMTIME_NAME, &luid)) {
        CloseHandle(token);
        return -1;
    }

    ZeroMemory(&tp, sizeof(tp));
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = enable ? SE_PRIVILEGE_ENABLED : 0;
    SetLastError(ERROR_SUCCESS);
    ok = AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), NULL, NULL);
    if (ok && GetLastError() == ERROR_NOT_ALL_ASSIGNED) {
        ok = FALSE;
    }
    CloseHandle(token);
    return ok ? 0 : -1;
}

int time_set_can_adjust(void)
{
    DWORD err = ERROR_SUCCESS;

    if (with_systemtime_privilege(TRUE) != 0) {
        return -1;
    }
    if (with_systemtime_privilege(FALSE) != 0) {
        err = GetLastError();
    }
    SetLastError(err);
    return 0;
}

int time_set_local(const SYSTEMTIME *st)
{
    if (st == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }
    if (with_systemtime_privilege(TRUE) != 0) {
        return -1;
    }
    if (!SetLocalTime(st)) {
        with_systemtime_privilege(FALSE);
        return -1;
    }
    with_systemtime_privilege(FALSE);
    return 0;
}
