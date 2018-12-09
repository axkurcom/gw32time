#include "ntp_probe.h"

#include <stdio.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#define NTP_UNIX_EPOCH_DELTA 2208988800UL

static void set_probe_error(ntp_probe_result_t *out, const wchar_t *message)
{
    if (out == NULL) {
        return;
    }

    _snwprintf(out->error, sizeof(out->error) / sizeof(out->error[0]), L"%ls", message);
    out->error[(sizeof(out->error) / sizeof(out->error[0])) - 1] = L'\0';
}

static unsigned long read_be32(const unsigned char *buf)
{
    return ((unsigned long)buf[0] << 24) |
        ((unsigned long)buf[1] << 16) |
        ((unsigned long)buf[2] << 8) |
        (unsigned long)buf[3];
}

static double ntp_timestamp_to_ms(const unsigned char *buf)
{
    unsigned long seconds = read_be32(buf);
    unsigned long fraction = read_be32(buf + 4);
    double unix_seconds = (double)seconds - (double)NTP_UNIX_EPOCH_DELTA;
    double fraction_seconds = (double)fraction / 4294967296.0;

    return (unix_seconds + fraction_seconds) * 1000.0;
}

static void write_ntp_timestamp(unsigned char *buf, DWORD tick_ms)
{
    unsigned long seconds = (tick_ms / 1000UL) + NTP_UNIX_EPOCH_DELTA;
    unsigned long fraction = (unsigned long)(((double)(tick_ms % 1000UL) / 1000.0) * 4294967296.0);

    buf[0] = (unsigned char)((seconds >> 24) & 0xff);
    buf[1] = (unsigned char)((seconds >> 16) & 0xff);
    buf[2] = (unsigned char)((seconds >> 8) & 0xff);
    buf[3] = (unsigned char)(seconds & 0xff);
    buf[4] = (unsigned char)((fraction >> 24) & 0xff);
    buf[5] = (unsigned char)((fraction >> 16) & 0xff);
    buf[6] = (unsigned char)((fraction >> 8) & 0xff);
    buf[7] = (unsigned char)(fraction & 0xff);
}

int ntp_probe(const wchar_t *host, int timeout_ms, ntp_probe_result_t *out)
{
    WSADATA wsa;
    ADDRINFOW hints;
    ADDRINFOW *resolved = NULL;
    SOCKET sock = INVALID_SOCKET;
    unsigned char request[48];
    unsigned char response[48];
    DWORD start_ms;
    DWORD end_ms;
    int rc;
    int received;
    double t1;
    double t2;
    double t3;
    double t4;

    if (host == NULL || host[0] == L'\0' || timeout_ms <= 0 || out == NULL) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return -1;
    }

    ZeroMemory(out, sizeof(*out));
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        set_probe_error(out, L"Winsock startup failed.");
        return 0;
    }

    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    rc = GetAddrInfoW(host, L"123", &hints, &resolved);
    if (rc != 0 || resolved == NULL) {
        set_probe_error(out, L"DNS lookup failed.");
        WSACleanup();
        return 0;
    }
    out->dns_ok = 1;

    sock = socket(resolved->ai_family, resolved->ai_socktype, resolved->ai_protocol);
    if (sock == INVALID_SOCKET) {
        set_probe_error(out, L"UDP socket creation failed.");
        FreeAddrInfoW(resolved);
        WSACleanup();
        return 0;
    }

    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms, sizeof(timeout_ms));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout_ms, sizeof(timeout_ms));

    ZeroMemory(request, sizeof(request));
    request[0] = 0x1b;
    start_ms = GetTickCount();
    write_ntp_timestamp(request + 40, start_ms);

    rc = sendto(sock, (const char *)request, sizeof(request), 0, resolved->ai_addr, (int)resolved->ai_addrlen);
    if (rc != sizeof(request)) {
        set_probe_error(out, L"Failed to send SNTP request.");
        closesocket(sock);
        FreeAddrInfoW(resolved);
        WSACleanup();
        return 0;
    }

    received = recvfrom(sock, (char *)response, sizeof(response), 0, NULL, NULL);
    end_ms = GetTickCount();
    closesocket(sock);
    FreeAddrInfoW(resolved);
    WSACleanup();

    if (received < 48) {
        set_probe_error(out, L"No valid SNTP response received.");
        return 0;
    }

    if ((response[0] & 0x7) != 4 && (response[0] & 0x7) != 5) {
        set_probe_error(out, L"SNTP response mode is invalid.");
        return 0;
    }

    out->stratum = response[1];
    if (out->stratum == 0) {
        set_probe_error(out, L"SNTP server returned kiss-of-death stratum.");
        return 0;
    }

    t1 = (double)start_ms;
    t2 = ntp_timestamp_to_ms(response + 32);
    t3 = ntp_timestamp_to_ms(response + 40);
    t4 = (double)end_ms;

    out->rtt_ms = end_ms - start_ms;
    out->offset_ms = ((t2 - t1) + (t3 - t4)) / 2.0;
    out->ok = 1;
    return 0;
}
