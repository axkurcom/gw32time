#include "ntp_socket.h"

#include <stdio.h>
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

static int try_endpoint(
    const ADDRINFOA *endpoint,
    int timeout_ms,
    const gw_ntp_packet_t *request_template,
    unsigned char response_raw[GW_NTP_PACKET_SIZE],
    gw_clock_sample_t *t1,
    gw_clock_sample_t *t4,
    int *response_bytes,
    gw_ntp_socket_error_t *error_out)
{
    SOCKET sock;
    int sent;
    int received;
    unsigned char request_raw[GW_NTP_PACKET_SIZE];
    gw_ntp_packet_t request_packet;
    struct sockaddr_storage from_addr;
    int from_len = sizeof(from_addr);

    sock = socket(endpoint->ai_family, endpoint->ai_socktype, endpoint->ai_protocol);
    if (sock == INVALID_SOCKET) {
        if (error_out != NULL) {
            *error_out = GW_NTP_SOCKET_CREATE;
        }
        return -1;
    }

    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms, sizeof(timeout_ms));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout_ms, sizeof(timeout_ms));

    if (gw_clock_sample_now(t1) != 0) {
        if (error_out != NULL) {
            *error_out = GW_NTP_SOCKET_IO;
        }
        closesocket(sock);
        return -1;
    }
    request_packet = *request_template;
    request_packet.transmit_timestamp = t1->ntp_wall_timestamp;
    gw_ntp_packet_write(&request_packet, request_raw);

    sent = sendto(sock, (const char *)request_raw, GW_NTP_PACKET_SIZE, 0, endpoint->ai_addr, (int)endpoint->ai_addrlen);
    if (sent != GW_NTP_PACKET_SIZE) {
        if (error_out != NULL) {
            *error_out = map_wsa_timeout(sent) ? GW_NTP_SOCKET_TIMEOUT : GW_NTP_SOCKET_IO;
        }
        closesocket(sock);
        return -1;
    }

    memset(&from_addr, 0, sizeof(from_addr));
    received = recvfrom(sock, (char *)response_raw, GW_NTP_PACKET_SIZE, 0, (struct sockaddr *)&from_addr, &from_len);
    if (gw_clock_sample_now(t4) != 0) {
        if (error_out != NULL) {
            *error_out = GW_NTP_SOCKET_IO;
        }
        closesocket(sock);
        return -1;
    }
    closesocket(sock);

    if (received <= 0) {
        if (error_out != NULL) {
            *error_out = map_wsa_timeout(received) ? GW_NTP_SOCKET_TIMEOUT : GW_NTP_SOCKET_IO;
        }
        return -1;
    }
    if (from_addr.ss_family != endpoint->ai_family) {
        if (error_out != NULL) {
            *error_out = GW_NTP_SOCKET_IO;
        }
        return -1;
    }
    if (response_bytes != NULL) {
        *response_bytes = received;
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
    ADDRINFOA *it = NULL;
    char port_text[16];
    char normalized_host[256];
    unsigned char response_raw[GW_NTP_PACKET_SIZE];
    gw_ntp_packet_t request_packet;
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
    if (host[0] == '[') {
        size_t n = strlen(host);
        if (n > 2 && host[n - 1] == ']') {
            size_t copy_len = n - 2;
            if (copy_len >= sizeof(normalized_host)) {
                copy_len = sizeof(normalized_host) - 1;
            }
            memcpy(normalized_host, host + 1, copy_len);
            normalized_host[copy_len] = '\0';
            host = normalized_host;
        }
    }

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        if (error_out != NULL) {
            *error_out = GW_NTP_SOCKET_IO;
        }
        return -1;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    snprintf(port_text, sizeof(port_text), "%u", (unsigned)port);
    port_text[sizeof(port_text) - 1] = '\0';

    if (GetAddrInfoA(host, port_text, &hints, &resolved) != 0 || resolved == NULL) {
        if (error_out != NULL) {
            *error_out = GW_NTP_SOCKET_DNS;
        }
        goto cleanup;
    }

    gw_ntp_packet_init_request(&request_packet);

    for (it = resolved; it != NULL; it = it->ai_next) {
        if (it->ai_family != AF_INET && it->ai_family != AF_INET6) {
            continue;
        }
        if (try_endpoint(it, timeout_ms, &request_packet, response_raw, t1, t4, response_bytes, error_out) == 0) {
            break;
        }
    }
    if (it == NULL) {
        goto cleanup;
    }
    if (gw_ntp_packet_read(response_raw, response_bytes != NULL ? *response_bytes : GW_NTP_PACKET_SIZE, response) != 0) {
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
    if (resolved != NULL) {
        FreeAddrInfoA(resolved);
    }
    WSACleanup();
    return ok;
}
