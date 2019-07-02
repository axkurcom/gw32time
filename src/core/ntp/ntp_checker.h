#ifndef GW32TIME_NTP_CHECKER_H
#define GW32TIME_NTP_CHECKER_H

#include <stdint.h>

typedef enum gw_ntp_error {
    GW_NTP_OK = 0,
    GW_NTP_ERR_DNS,
    GW_NTP_ERR_SOCKET,
    GW_NTP_ERR_TIMEOUT,
    GW_NTP_ERR_SHORT_PACKET,
    GW_NTP_ERR_INVALID_RESPONSE,
    GW_NTP_ERR_KISS_OF_DEATH
} gw_ntp_error_t;

typedef struct gw_ntp_sample {
    gw_ntp_error_t error;
    double offset_ms;
    double delay_ms;
    double rtt_ms;
    double server_processing_ms;
    uint8_t stratum;
    uint8_t leap;
    uint8_t version;
    uint8_t mode;
    char reference_id[8];
} gw_ntp_sample_t;

typedef struct gw_ntp_checker_config {
    int samples;
    int timeout_ms;
    int interval_ms;
    int port;
    double outlier_threshold_ms;
} gw_ntp_checker_config_t;

typedef struct gw_ntp_checker_result {
    char host[256];
    int total_samples;
    int success_samples;
    double reachability;
    double offset_median_ms;
    double offset_mean_ms;
    double offset_stddev_ms;
    double delay_min_ms;
    double delay_mean_ms;
    double jitter_ms;
    double score;
    gw_ntp_error_t last_error;
    uint8_t stratum;
} gw_ntp_checker_result_t;

typedef struct gw_ntp_explain {
    char lines[16][128];
    int count;
} gw_ntp_explain_t;

int gw_ntp_checker_sample(
    const char *host,
    int timeout_ms,
    int port,
    gw_ntp_sample_t *sample);

int gw_ntp_checker_server(
    const char *host,
    const gw_ntp_checker_config_t *config,
    gw_ntp_checker_result_t *result);

int gw_ntp_checker_explain(
    const gw_ntp_checker_result_t *result,
    gw_ntp_explain_t *explain);

#endif
