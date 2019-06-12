#include "ntp_socket.h"

#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>

static int map_wsa_timeout(int rc)
{
    if (rc == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err == WSAETIMEDOUT || err == WSAEWOULDBLOCK) {
            return 1;
        }
    }
    return 0;
}

int gw_ntp_socket_send_checker(
    const char *host,
    uint16_t port,
    int timeout_ms,
    gw_ntp_packet_t *response,
    gw_clock_sample_t *t1,
    gw_clock_sample_t *t4,
    int *response_bytes,
    gw_ntp_socket_error_t *error_out)
{
    WSADATA wsa;
    ADDRINFOA hints;
    ADDRINFOA *resolved = NULL;
    SOCKET sock = INVALID_SOCKET;
    char port_text[16];
    unsigned char request_raw[GW_NTP_PACKET_SIZE];
    unsigned char response_raw[GW_NTP_PACKET_SIZE];
    gw_ntp_packet_t request_packet;
    int sent;
    int received = 0;
    int ok = -1;

    if (response_bytes != NULL) {
        *response_bytes = 0;
    }
    if (error_out != NULL) {
        *error_out = GW_NTP_SOCKET_IO;
    }
    if (host == NULL || host[0] == '\0' || timeout_ms <= 0 || response == NULL || t1 == NULL || t4 == NULL) {
        return -1;
    }

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        if (error_out != NULL) {
            *error_out = GW_NTP_SOCKET_IO;
        }
        return -1;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    _snprintf(port_text, sizeof(port_text), "%u", (unsigned)port);
    port_text[sizeof(port_text) - 1] = '\0';

    if (GetAddrInfoA(host, port_text, &hints, &resolved) != 0 || resolved == NULL) {
        if (error_out != NULL) {
            *error_out = GW_NTP_SOCKET_DNS;
        }
        goto cleanup;
    }

    sock = socket(resolved->ai_family, resolved->ai_socktype, resolved->ai_protocol);
    if (sock == INVALID_SOCKET) {
        if (error_out != NULL) {
            *error_out = GW_NTP_SOCKET_CREATE;
        }
        goto cleanup;
    }

    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms, sizeof(timeout_ms));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout_ms, sizeof(timeout_ms));

    gw_ntp_packet_init_request(&request_packet);
    if (gw_clock_sample_now(t1) != 0) {
        if (error_out != NULL) {
            *error_out = GW_NTP_SOCKET_IO;
        }
        goto cleanup;
    }
    request_packet.transmit_timestamp = t1->ntp_wall_timestamp;
    gw_ntp_packet_write(&request_packet, request_raw);

    sent = sendto(sock, (const char *)request_raw, GW_NTP_PACKET_SIZE, 0, resolved->ai_addr, (int)resolved->ai_addrlen);
    if (sent != GW_NTP_PACKET_SIZE) {
        if (error_out != NULL) {
            *error_out = map_wsa_timeout(sent) ? GW_NTP_SOCKET_TIMEOUT : GW_NTP_SOCKET_IO;
        }
        goto cleanup;
    }

    received = recvfrom(sock, (char *)response_raw, sizeof(response_raw), 0, NULL, NULL);
    if (gw_clock_sample_now(t4) != 0) {
        if (error_out != NULL) {
            *error_out = GW_NTP_SOCKET_IO;
        }
        goto cleanup;
    }
    if (received <= 0) {
        if (error_out != NULL) {
            *error_out = map_wsa_timeout(received) ? GW_NTP_SOCKET_TIMEOUT : GW_NTP_SOCKET_IO;
        }
        goto cleanup;
    }
    if (response_bytes != NULL) {
        *response_bytes = received;
    }
    if (gw_ntp_packet_read(response_raw, received, response) != 0) {
        if (error_out != NULL) {
            *error_out = GW_NTP_SOCKET_IO;
        }
        goto cleanup;
    }
    if (error_out != NULL) {
        *error_out = GW_NTP_SOCKET_OK;
    }
    ok = 0;

cleanup:
    if (sock != INVALID_SOCKET) {
        closesocket(sock);
    }
    if (resolved != NULL) {
        FreeAddrInfoA(resolved);
    }
    WSACleanup();
    return ok;
}
