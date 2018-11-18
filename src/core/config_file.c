#include "config_file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static char *trim_ascii(char *text)
{
    char *end;

    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') {
        text++;
    }

    end = text + strlen(text);
    while (end > text && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
        end--;
    }
    *end = '\0';
    return text;
}

static int read_wide_value(const char *value, wchar_t *out, size_t out_chars)
{
    int written;

    if (value == NULL || out == NULL || out_chars == 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }

    written = MultiByteToWideChar(CP_UTF8, 0, value, -1, out, (int)out_chars);
    if (written == 0) {
        return -1;
    }

    out[out_chars - 1] = L'\0';
    return 0;
}

static int read_dword_value(const char *value, DWORD *out, int *has_value)
{
    char *end;
    unsigned long parsed;

    if (value == NULL || out == NULL || has_value == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }

    if (value[0] == '\0') {
        *has_value = 0;
        *out = 0;
        return 0;
    }

    parsed = strtoul(value, &end, 10);
    if (*end != '\0' || parsed > 0xffffffffUL) {
        SetLastError(ERROR_INVALID_DATA);
        return -1;
    }

    *out = (DWORD)parsed;
    *has_value = 1;
    return 0;
}

int config_file_read(const wchar_t *path, w32time_config_t *config)
{
    FILE *file;
    char line[4096];

    if (path == NULL || config == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }

    ZeroMemory(config, sizeof(*config));

    file = _wfopen(path, L"r");
    if (file == NULL) {
        SetLastError(ERROR_OPEN_FAILED);
        return -1;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        char *text = trim_ascii(line);
        char *equals;
        char *name;
        char *value;

        if (text[0] == '\0' || text[0] == ';' || text[0] == '#') {
            continue;
        }

        if (strcmp(text, "[W32Time]") == 0) {
            continue;
        }

        equals = strchr(text, '=');
        if (equals == NULL) {
            fclose(file);
            SetLastError(ERROR_INVALID_DATA);
            return -1;
        }

        *equals = '\0';
        name = trim_ascii(text);
        value = trim_ascii(equals + 1);

        if (strcmp(name, "Type") == 0) {
            if (read_wide_value(value, config->type, sizeof(config->type) / sizeof(config->type[0])) != 0) {
                fclose(file);
                return -1;
            }
        } else if (strcmp(name, "NtpServer") == 0) {
            if (read_wide_value(value, config->ntp_server, sizeof(config->ntp_server) / sizeof(config->ntp_server[0])) != 0) {
                fclose(file);
                return -1;
            }
        } else if (strcmp(name, "SpecialPollInterval") == 0) {
            if (read_dword_value(value, &config->special_poll_interval, &config->has_special_poll_interval) != 0) {
                fclose(file);
                return -1;
            }
        } else if (strcmp(name, "NtpClientEnabled") == 0) {
            if (read_dword_value(value, &config->ntp_client_enabled, &config->has_ntp_client_enabled) != 0) {
                fclose(file);
                return -1;
            }
        }
    }

    fclose(file);
    return 0;
}
