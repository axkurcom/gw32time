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
    ok = AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), NULL, NULL);
    CloseHandle(token);
    return ok ? 0 : -1;
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
