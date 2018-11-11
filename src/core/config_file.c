#include "config_file.h"

#include <stdio.h>
#include <windows.h>

static void write_dword_or_empty(FILE *file, const char *name, int has_value, DWORD value)
{
    if (has_value) {
        fprintf(file, "%s=%lu\n", name, (unsigned long)value);
    } else {
        fprintf(file, "%s=\n", name);
    }
}

static int write_wide_value(FILE *file, const char *name, const wchar_t *value)
{
    char utf8[4096];
    int written;

    if (file == NULL || name == NULL || value == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }

    written = WideCharToMultiByte(
        CP_UTF8,
        0,
        value,
        -1,
        utf8,
        sizeof(utf8),
        NULL,
        NULL);
    if (written == 0) {
        return -1;
    }

    fprintf(file, "%s=%s\n", name, utf8);
    return 0;
}

int config_file_write(const wchar_t *path, const w32time_config_t *config)
{
    FILE *file;

    if (path == NULL || config == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }

    file = _wfopen(path, L"w");
    if (file == NULL) {
        SetLastError(ERROR_OPEN_FAILED);
        return -1;
    }

    fprintf(file, "[W32Time]\n");
    if (write_wide_value(file, "Type", config->type) != 0) {
        fclose(file);
        return -1;
    }
    if (write_wide_value(file, "NtpServer", config->ntp_server) != 0) {
        fclose(file);
        return -1;
    }
    write_dword_or_empty(file, "SpecialPollInterval", config->has_special_poll_interval, config->special_poll_interval);
    write_dword_or_empty(file, "NtpClientEnabled", config->has_ntp_client_enabled, config->ntp_client_enabled);
    fclose(file);
    return 0;
}
