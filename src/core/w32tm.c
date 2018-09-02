#include "w32tm.h"

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static int append_bytes(char *dst, DWORD capacity, DWORD *used, const char *src, DWORD src_len)
{
    DWORD room;
    DWORD copy;

    if (*used >= capacity) {
        return 0;
    }

    room = capacity - *used - 1;
    copy = src_len < room ? src_len : room;
    if (copy > 0) {
        memcpy(dst + *used, src, copy);
        *used += copy;
        dst[*used] = '\0';
    }

    return 0;
}

static int convert_process_output(const char *raw, DWORD raw_len, wchar_t *out, size_t out_chars)
{
    int written;

    if (out == NULL || out_chars == 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }

    out[0] = L'\0';
    if (raw_len == 0) {
        return 0;
    }

    if (raw_len > 0x7fffffffUL || out_chars > 0x7fffffffUL) {
        SetLastError(ERROR_BUFFER_OVERFLOW);
        return -1;
    }

    written = MultiByteToWideChar(
        CP_OEMCP,
        0,
        raw,
        (int)raw_len,
        out,
        (int)(out_chars - 1));
    if (written == 0) {
        written = MultiByteToWideChar(
            CP_ACP,
            0,
            raw,
            (int)raw_len,
            out,
            (int)(out_chars - 1));
    }

    if (written == 0) {
        return -1;
    }

    out[written] = L'\0';
    return 0;
}

int run_process_capture(const wchar_t *cmdline, wchar_t *stdout_buf, size_t stdout_chars, DWORD *exit_code)
{
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    HANDLE read_pipe = NULL;
    HANDLE write_pipe = NULL;
    wchar_t *cmd_copy;
    size_t cmd_chars;
    char *raw;
    DWORD raw_capacity;
    DWORD raw_used = 0;
    DWORD last_error = ERROR_SUCCESS;
    int result = -1;

    if (cmdline == NULL || stdout_buf == NULL || stdout_chars == 0 || exit_code == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }

    stdout_buf[0] = L'\0';
    *exit_code = 0;

    if (stdout_chars > 4096) {
        raw_capacity = 16384;
    } else {
        raw_capacity = (DWORD)(stdout_chars * 4);
    }

    raw = (char *)malloc(raw_capacity);
    if (raw == NULL) {
        SetLastError(ERROR_OUTOFMEMORY);
        return -1;
    }
    raw[0] = '\0';

    cmd_chars = wcslen(cmdline) + 1;
    cmd_copy = (wchar_t *)malloc(cmd_chars * sizeof(wchar_t));
    if (cmd_copy == NULL) {
        free(raw);
        SetLastError(ERROR_OUTOFMEMORY);
        return -1;
    }
    wcscpy(cmd_copy, cmdline);

    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) {
        last_error = GetLastError();
        goto done;
    }

    if (!SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0)) {
        last_error = GetLastError();
        goto done;
    }

    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = write_pipe;
    si.hStdError = write_pipe;

    if (!CreateProcessW(
            NULL,
            cmd_copy,
            NULL,
            NULL,
            TRUE,
            CREATE_NO_WINDOW,
            NULL,
            NULL,
            &si,
            &pi)) {
        last_error = GetLastError();
        goto done;
    }

    CloseHandle(write_pipe);
    write_pipe = NULL;

    for (;;) {
        char chunk[512];
        DWORD bytes_read = 0;
        BOOL ok = ReadFile(read_pipe, chunk, sizeof(chunk), &bytes_read, NULL);

        if (!ok || bytes_read == 0) {
            break;
        }

        append_bytes(raw, raw_capacity, &raw_used, chunk, bytes_read);
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    if (!GetExitCodeProcess(pi.hProcess, exit_code)) {
        last_error = GetLastError();
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        goto done;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (convert_process_output(raw, raw_used, stdout_buf, stdout_chars) != 0) {
        last_error = GetLastError();
        goto done;
    }

    result = 0;

done:
    if (read_pipe != NULL) {
        CloseHandle(read_pipe);
    }
    if (write_pipe != NULL) {
        CloseHandle(write_pipe);
    }
    free(cmd_copy);
    free(raw);
    if (result != 0) {
        SetLastError(last_error);
    }
    return result;
}

static int w32tm_query_raw(const wchar_t *cmdline, w32tm_raw_result_t *out)
{
    if (out == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }

    memset(out, 0, sizeof(*out));
    return run_process_capture(
        cmdline,
        out->raw,
        sizeof(out->raw) / sizeof(out->raw[0]),
        &out->exit_code);
}

int w32tm_query_status_raw(w32tm_raw_result_t *out)
{
    return w32tm_query_raw(L"w32tm /query /status", out);
}

int w32tm_query_peers_raw(w32tm_raw_result_t *out)
{
    return w32tm_query_raw(L"w32tm /query /peers", out);
}

int w32tm_query_configuration_raw(w32tm_raw_result_t *out)
{
    return w32tm_query_raw(L"w32tm /query /configuration", out);
}

int w32tm_resync_raw(w32tm_raw_result_t *out)
{
    return w32tm_query_raw(L"w32tm /resync", out);
}
