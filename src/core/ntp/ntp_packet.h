#ifndef GW32TIME_NTP_PACKET_H
#define GW32TIME_NTP_PACKET_H

#include <stdint.h>

#pragma pack(push, 1)
typedef struct gw_ntp_packet {
    uint8_t li_vn_mode;
    uint8_t stratum;
    uint8_t poll;
    int8_t precision;
    uint32_t root_delay;
    uint32_t root_dispersion;
    uint32_t reference_id;
    uint64_t reference_timestamp;
    uint64_t originate_timestamp;
    uint64_t receive_timestamp;
    uint64_t transmit_timestamp;
} gw_ntp_packet_t;
#pragma pack(pop)

#define GW_NTP_PACKET_SIZE 48
#define GW_NTP_MODE_CLIENT 3
#define GW_NTP_MODE_SERVER 4
#define GW_NTP_MODE_BROADCAST 5

void gw_ntp_packet_init_request(gw_ntp_packet_t *packet);
int gw_ntp_packet_read(const unsigned char *raw, int raw_len, gw_ntp_packet_t *packet);
void gw_ntp_packet_write(const gw_ntp_packet_t *packet, unsigned char raw[GW_NTP_PACKET_SIZE]);

uint8_t gw_ntp_packet_leap(const gw_ntp_packet_t *packet);
uint8_t gw_ntp_packet_version(const gw_ntp_packet_t *packet);
uint8_t gw_ntp_packet_mode(const gw_ntp_packet_t *packet);

#endif
