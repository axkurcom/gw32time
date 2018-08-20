#include "privilege.h"

#include <windows.h>

#include "error.h"

int privilege_is_admin(int *out)
{
    BOOL is_member = FALSE;
    SID_IDENTIFIER_AUTHORITY nt_authority = SECURITY_NT_AUTHORITY;
    PSID admins = NULL;

    if (out == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }

    *out = 0;

    if (!AllocateAndInitializeSid(
            &nt_authority,
            2,
            SECURITY_BUILTIN_DOMAIN_RID,
            DOMAIN_ALIAS_RID_ADMINS,
            0,
            0,
            0,
            0,
            0,
            0,
            &admins)) {
        error_print_last(L"AllocateAndInitializeSid");
        return -1;
    }

    if (!CheckTokenMembership(NULL, admins, &is_member)) {
        error_print_last(L"CheckTokenMembership");
        FreeSid(admins);
        return -1;
    }

    FreeSid(admins);
    *out = is_member ? 1 : 0;
    return 0;
}
