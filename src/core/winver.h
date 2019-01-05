#ifndef GW32TIME_WINVER_H
#define GW32TIME_WINVER_H

#include <windows.h>

typedef enum {
    OS_ARCH_UNKNOWN = 0,
    OS_ARCH_X86,
    OS_ARCH_X64,
    OS_ARCH_ARM64
} os_arch_t;

typedef struct {
    DWORD major;
    DWORD minor;
    DWORD build;
    os_arch_t arch;
} os_info_t;

int winver_query(os_info_t *out);
const wchar_t *os_arch_name(os_arch_t arch);

#endif
