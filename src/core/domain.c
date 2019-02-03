#include "domain.h"

#include <lm.h>
#include <string.h>

int domain_query(domain_info_t *out)
{
    LPWSTR name = NULL;
    NETSETUP_JOIN_STATUS status = NetSetupUnknownStatus;
    NET_API_STATUS rc;

    if (out == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }

    ZeroMemory(out, sizeof(*out));

    rc = NetGetJoinInformation(NULL, &name, &status);
    if (rc != NERR_Success) {
        SetLastError((DWORD)rc);
        return -1;
    }

    out->joined = status == NetSetupDomainName;
    if (name != NULL) {
        wcsncpy(out->name, name, (sizeof(out->name) / sizeof(out->name[0])) - 1);
        out->name[(sizeof(out->name) / sizeof(out->name[0])) - 1] = L'\0';
        NetApiBufferFree(name);
    }

    return 0;
}
