#include "w32tm.h"

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define PROCESS_CAPTURE_TIMEOUT_MS 30000

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
    HANDLE stdin_null = NULL;
    wchar_t *cmd_copy;
    size_t cmd_chars;
    char *raw;
    DWORD raw_capacity;
    DWORD raw_used = 0;
    DWORD last_error = ERROR_SUCCESS;
    DWORD start_tick = 0;
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

    stdin_null = CreateFileW(
        L"NUL",
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &sa,
        OPEN_EXISTING,
        0,
        NULL);
    if (stdin_null == INVALID_HANDLE_VALUE) {
        stdin_null = NULL;
        last_error = GetLastError();
        goto done;
    }

    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = stdin_null;
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

    start_tick = GetTickCount();
    for (;;) {
        char chunk[512];
        DWORD available = 0;
        DWORD bytes_read = 0;
        DWORD wait_rc;

        for (;;) {
            if (!PeekNamedPipe(read_pipe, NULL, 0, NULL, &available, NULL)) {
                DWORD peek_err = GetLastError();
                if (peek_err == ERROR_BROKEN_PIPE) {
                    available = 0;
                    break;
                }
                last_error = peek_err;
                goto done;
            }
            if (available == 0) {
                break;
            }
            if (!ReadFile(read_pipe, chunk, available < sizeof(chunk) ? available : sizeof(chunk), &bytes_read, NULL) ||
                bytes_read == 0) {
                DWORD read_err = GetLastError();
                if (read_err == ERROR_BROKEN_PIPE) {
                    break;
                }
                last_error = read_err;
                goto done;
            }
            append_bytes(raw, raw_capacity, &raw_used, chunk, bytes_read);
        }

        wait_rc = WaitForSingleObject(pi.hProcess, 0);
        if (wait_rc == WAIT_OBJECT_0 && available == 0) {
            break;
        }
        if (wait_rc == WAIT_FAILED) {
            last_error = GetLastError();
            goto done;
        }
        if (GetTickCount() - start_tick > PROCESS_CAPTURE_TIMEOUT_MS) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 2000);
            last_error = ERROR_TIMEOUT;
            goto done;
        }
        Sleep(20);
    }

    if (!GetExitCodeProcess(pi.hProcess, exit_code)) {
        last_error = GetLastError();
        goto done;
    }

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
    if (stdin_null != NULL) {
        CloseHandle(stdin_null);
    }
    if (pi.hThread != NULL) {
        CloseHandle(pi.hThread);
    }
    if (pi.hProcess != NULL) {
        CloseHandle(pi.hProcess);
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

static int copy_trimmed_value(wchar_t *dst, size_t dst_chars, const wchar_t *first, const wchar_t *last)
{
    size_t len;

    while (first < last && (*first == L' ' || *first == L'\t')) {
        first++;
    }

    while (last > first && (last[-1] == L' ' || last[-1] == L'\t' || last[-1] == L'\r')) {
        last--;
    }

    len = (size_t)(last - first);
    if (len >= dst_chars) {
        len = dst_chars - 1;
    }

    if (len > 0) {
        wmemcpy(dst, first, len);
    }
    dst[len] = L'\0';
    return len > 0 ? 1 : 0;
}

static int copy_line_value(const wchar_t *line, const wchar_t *name, wchar_t *dst, size_t dst_chars)
{
    size_t name_len = wcslen(name);
    const wchar_t *value;
    const wchar_t *end;

    if (_wcsnicmp(line, name, name_len) != 0 || line[name_len] != L':') {
        return 0;
    }

    value = line + name_len + 1;
    end = value;
    while (*end != L'\0' && *end != L'\n') {
        end++;
    }

    return copy_trimmed_value(dst, dst_chars, value, end);
}

int w32tm_parse_status_summary(const wchar_t *raw, w32tm_status_summary_t *out)
{
    const wchar_t *line;

    if (raw == NULL || out == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }

    memset(out, 0, sizeof(*out));
    line = raw;
    while (*line != L'\0') {
        out->has_source |= copy_line_value(line, L"Source", out->source, sizeof(out->source) / sizeof(out->source[0]));
        out->has_stratum |= copy_line_value(line, L"Stratum", out->stratum, sizeof(out->stratum) / sizeof(out->stratum[0]));
        out->has_last_sync |= copy_line_value(
            line,
            L"Last Successful Sync Time",
            out->last_sync,
            sizeof(out->last_sync) / sizeof(out->last_sync[0]));
        out->has_poll_interval |= copy_line_value(
            line,
            L"Poll Interval",
            out->poll_interval,
            sizeof(out->poll_interval) / sizeof(out->poll_interval[0]));

        while (*line != L'\0' && *line != L'\n') {
            line++;
        }
        if (*line == L'\n') {
            line++;
        }
    }

    return 0;
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

int w32tm_config_manual_peers_raw(const wchar_t *peerlist, w32tm_raw_result_t *out)
{
    wchar_t cmdline[1400];

    if (peerlist == NULL || peerlist[0] == L'\0' || out == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }

    if (wcschr(peerlist, L'"') != NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }

    _snwprintf(
        cmdline,
        sizeof(cmdline) / sizeof(cmdline[0]),
        L"w32tm /config /manualpeerlist:\"%ls\" /syncfromflags:manual /update",
        peerlist);
    cmdline[(sizeof(cmdline) / sizeof(cmdline[0])) - 1] = L'\0';
    return w32tm_query_raw(cmdline, out);
}

int w32tm_config_update_raw(w32tm_raw_result_t *out)
{
    return w32tm_query_raw(L"w32tm /config /update", out);
}
