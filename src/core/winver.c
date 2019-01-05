#include "winver.h"

typedef LONG (WINAPI *rtl_get_version_fn)(PRTL_OSVERSIONINFOW);

static os_arch_t map_arch(WORD arch)
{
    switch (arch) {
    case PROCESSOR_ARCHITECTURE_INTEL:
        return OS_ARCH_X86;
    case PROCESSOR_ARCHITECTURE_AMD64:
        return OS_ARCH_X64;
    case PROCESSOR_ARCHITECTURE_ARM64:
        return OS_ARCH_ARM64;
    default:
        return OS_ARCH_UNKNOWN;
    }
}

int winver_query(os_info_t *out)
{
    HMODULE ntdll;
    rtl_get_version_fn rtl_get_version;
    RTL_OSVERSIONINFOW version;
    SYSTEM_INFO system_info;

    if (out == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }

    ZeroMemory(out, sizeof(*out));
    ZeroMemory(&version, sizeof(version));
    version.dwOSVersionInfoSize = sizeof(version);

    ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == NULL) {
        return -1;
    }

    rtl_get_version = (rtl_get_version_fn)GetProcAddress(ntdll, "RtlGetVersion");
    if (rtl_get_version == NULL || rtl_get_version(&version) != 0) {
        SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
        return -1;
    }

    GetNativeSystemInfo(&system_info);
    out->major = version.dwMajorVersion;
    out->minor = version.dwMinorVersion;
    out->build = version.dwBuildNumber;
    out->arch = map_arch(system_info.wProcessorArchitecture);
    return 0;
}

const wchar_t *os_arch_name(os_arch_t arch)
{
    switch (arch) {
    case OS_ARCH_X86:
        return L"x86";
    case OS_ARCH_X64:
        return L"x64";
    case OS_ARCH_ARM64:
        return L"arm64";
    default:
        return L"unknown";
    }
}
