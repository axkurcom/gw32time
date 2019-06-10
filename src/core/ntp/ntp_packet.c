#include "ntp_packet.h"

#include <string.h>

static uint32_t read_be32(const unsigned char *buf)
{
    return ((uint32_t)buf[0] << 24) |
           ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] << 8) |
           (uint32_t)buf[3];
}

static uint64_t read_be64(const unsigned char *buf)
{
    uint32_t hi = read_be32(buf);
    uint32_t lo = read_be32(buf + 4);

    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static void write_be32(unsigned char *buf, uint32_t value)
{
    buf[0] = (unsigned char)((value >> 24) & 0xff);
    buf[1] = (unsigned char)((value >> 16) & 0xff);
    buf[2] = (unsigned char)((value >> 8) & 0xff);
    buf[3] = (unsigned char)(value & 0xff);
}

static void write_be64(unsigned char *buf, uint64_t value)
{
    write_be32(buf, (uint32_t)((value >> 32) & 0xffffffffULL));
    write_be32(buf + 4, (uint32_t)(value & 0xffffffffULL));
}

void gw_ntp_packet_init_request(gw_ntp_packet_t *packet)
{
    if (packet == NULL) {
        return;
    }

    memset(packet, 0, sizeof(*packet));
    packet->li_vn_mode = (uint8_t)((0u << 6) | (4u << 3) | GW_NTP_MODE_CLIENT);
}

int gw_ntp_packet_read(const unsigned char *raw, int raw_len, gw_ntp_packet_t *packet)
{
    if (raw == NULL || packet == NULL || raw_len < GW_NTP_PACKET_SIZE) {
        return -1;
    }

    memset(packet, 0, sizeof(*packet));
    packet->li_vn_mode = raw[0];
    packet->stratum = raw[1];
    packet->poll = raw[2];
    packet->precision = (int8_t)raw[3];
    packet->root_delay = read_be32(raw + 4);
    packet->root_dispersion = read_be32(raw + 8);
    packet->reference_id = read_be32(raw + 12);
    packet->reference_timestamp = read_be64(raw + 16);
    packet->originate_timestamp = read_be64(raw + 24);
    packet->receive_timestamp = read_be64(raw + 32);
    packet->transmit_timestamp = read_be64(raw + 40);
    return 0;
}

void gw_ntp_packet_write(const gw_ntp_packet_t *packet, unsigned char raw[GW_NTP_PACKET_SIZE])
{
    if (packet == NULL || raw == NULL) {
        return;
    }

    memset(raw, 0, GW_NTP_PACKET_SIZE);
    raw[0] = packet->li_vn_mode;
    raw[1] = packet->stratum;
    raw[2] = packet->poll;
    raw[3] = (unsigned char)packet->precision;
    write_be32(raw + 4, packet->root_delay);
    write_be32(raw + 8, packet->root_dispersion);
    write_be32(raw + 12, packet->reference_id);
    write_be64(raw + 16, packet->reference_timestamp);
    write_be64(raw + 24, packet->originate_timestamp);
    write_be64(raw + 32, packet->receive_timestamp);
    write_be64(raw + 40, packet->transmit_timestamp);
}

uint8_t gw_ntp_packet_leap(const gw_ntp_packet_t *packet)
{
    if (packet == NULL) {
        return 0;
    }
    return (uint8_t)((packet->li_vn_mode >> 6) & 0x3u);
}

uint8_t gw_ntp_packet_version(const gw_ntp_packet_t *packet)
{
    if (packet == NULL) {
        return 0;
    }
    return (uint8_t)((packet->li_vn_mode >> 3) & 0x7u);
}

uint8_t gw_ntp_packet_mode(const gw_ntp_packet_t *packet)
{
    if (packet == NULL) {
        return 0;
    }
    return (uint8_t)(packet->li_vn_mode & 0x7u);
}
