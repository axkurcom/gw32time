#include "ntp_checker.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "ntp_packet.h"
#include "ntp_score.h"
#include "ntp_socket.h"
#include "ntp_stats.h"
#include "ntp_time.h"

static void gw_ntp_reference_id_to_text(uint32_t reference_id, char out[8])
{
    unsigned char b0 = (unsigned char)((reference_id >> 24) & 0xff);
    unsigned char b1 = (unsigned char)((reference_id >> 16) & 0xff);
    unsigned char b2 = (unsigned char)((reference_id >> 8) & 0xff);
    unsigned char b3 = (unsigned char)(reference_id & 0xff);

    if (out == NULL) {
        return;
    }

    out[0] = isprint((int)b0) ? (char)b0 : '.';
    out[1] = isprint((int)b1) ? (char)b1 : '.';
    out[2] = isprint((int)b2) ? (char)b2 : '.';
    out[3] = isprint((int)b3) ? (char)b3 : '.';
    out[4] = '\0';
}

static gw_ntp_error_t gw_ntp_error_from_socket_error(gw_ntp_socket_error_t err)
{
    if (err == GW_NTP_SOCKET_DNS) {
        return GW_NTP_ERR_DNS;
    }
    if (err == GW_NTP_SOCKET_TIMEOUT) {
        return GW_NTP_ERR_TIMEOUT;
    }
    return GW_NTP_ERR_SOCKET;
}

int gw_ntp_checker_sample(
    const char *host,
    int timeout_ms,
    int port,
    gw_ntp_sample_t *sample)
{
    gw_ntp_packet_t packet;
    gw_clock_sample_t t1;
    gw_clock_sample_t t4;
    gw_ntp_socket_error_t socket_error = GW_NTP_SOCKET_IO;
    int response_bytes = 0;
    double t1_ms;
    double t2_ms;
    double t3_ms;
    double t4_ms;

    if (host == NULL || host[0] == '\0' || timeout_ms <= 0 || port <= 0 || sample == NULL) {
        return -1;
    }

    memset(sample, 0, sizeof(*sample));
    sample->error = GW_NTP_ERR_SOCKET;

    if (gw_ntp_socket_send_checker(
            host,
            (uint16_t)port,
            timeout_ms,
            &packet,
            &t1,
            &t4,
            &response_bytes,
            &socket_error) != 0) {
        sample->error = gw_ntp_error_from_socket_error(socket_error);
        return 0;
    }

    if (response_bytes < GW_NTP_PACKET_SIZE) {
        sample->error = GW_NTP_ERR_SHORT_PACKET;
        return 0;
    }

    sample->mode = gw_ntp_packet_mode(&packet);
    sample->version = gw_ntp_packet_version(&packet);
    sample->leap = gw_ntp_packet_leap(&packet);
    sample->stratum = packet.stratum;
    gw_ntp_reference_id_to_text(packet.reference_id, sample->reference_id);

    if (sample->mode != GW_NTP_MODE_SERVER && sample->mode != GW_NTP_MODE_BROADCAST) {
        sample->error = GW_NTP_ERR_INVALID_RESPONSE;
        return 0;
    }
    if (sample->stratum == 0) {
        sample->error = GW_NTP_ERR_KISS_OF_DEATH;
        return 0;
    }

    t1_ms = gw_ntp_to_ms(t1.ntp_wall_timestamp);
    t2_ms = gw_ntp_to_ms(packet.receive_timestamp);
    t3_ms = gw_ntp_to_ms(packet.transmit_timestamp);
    t4_ms = gw_ntp_to_ms(t4.ntp_wall_timestamp);
    sample->offset_ms = ((t2_ms - t1_ms) + (t3_ms - t4_ms)) / 2.0;
    sample->delay_ms = (t4_ms - t1_ms) - (t3_ms - t2_ms);
    sample->rtt_ms = gw_mono_diff_ms(t4.mono_ms, t1.mono_ms);
    sample->server_processing_ms = t3_ms - t2_ms;
    sample->error = GW_NTP_OK;
    return 0;
}

int gw_ntp_checker_server(
    const char *host,
    const gw_ntp_checker_config_t *config,
    gw_ntp_checker_result_t *result)
{
    gw_ntp_sample_t sample;
    gw_ntp_stats_result_t offset_stats;
    gw_ntp_stats_result_t delay_stats;
    double offsets[32];
    double filtered_offsets[32];
    double delays[32];
    int filtered_count = 0;
    int i;
    int samples;
    int timeout_ms;
    int interval_ms;
    int port;

    if (host == NULL || host[0] == '\0' || result == NULL) {
        return -1;
    }

    memset(result, 0, sizeof(*result));
    result->last_error = GW_NTP_ERR_SOCKET;
    timeout_ms = (config != NULL && config->timeout_ms > 0) ? config->timeout_ms : 1000;
    interval_ms = (config != NULL && config->interval_ms >= 0) ? config->interval_ms : 150;
    port = (config != NULL && config->port > 0) ? config->port : 123;
    samples = (config != NULL && config->samples > 0) ? config->samples : 5;
    if (samples > (int)(sizeof(offsets) / sizeof(offsets[0]))) {
        samples = (int)(sizeof(offsets) / sizeof(offsets[0]));
    }
    result->total_samples = samples;
    strncpy(result->host, host, sizeof(result->host) - 1);
    result->host[sizeof(result->host) - 1] = '\0';

    for (i = 0; i < samples; i++) {
        if (gw_ntp_checker_sample(host, timeout_ms, port, &sample) != 0) {
            return -1;
        }
        result->last_error = sample.error;
        if (sample.error == GW_NTP_OK) {
            offsets[result->success_samples] = sample.offset_ms;
            delays[result->success_samples] = sample.delay_ms;
            result->success_samples++;
            result->stratum = sample.stratum;
        }
        if (i + 1 < samples && interval_ms > 0) {
            Sleep((DWORD)interval_ms);
        }
    }

    if (result->success_samples == 0) {
        result->reachability = 0.0;
        result->score = 0.0;
        return 0;
    }
    result->reachability = (double)result->success_samples / (double)result->total_samples;
    if (gw_ntp_stats_filter_outliers_median(
            offsets,
            result->success_samples,
            50.0,
            filtered_offsets,
            (int)(sizeof(filtered_offsets) / sizeof(filtered_offsets[0])),
            &filtered_count) != 0 ||
        filtered_count <= 0) {
        filtered_count = result->success_samples;
        memcpy(filtered_offsets, offsets, (size_t)filtered_count * sizeof(double));
    }
    if (gw_ntp_stats_calculate(filtered_offsets, filtered_count, &offset_stats) != 0) {
        return -1;
    }
    if (gw_ntp_stats_calculate(delays, result->success_samples, &delay_stats) != 0) {
        return -1;
    }

    result->offset_median_ms = offset_stats.median;
    result->offset_mean_ms = offset_stats.mean;
    result->offset_stddev_ms = offset_stats.stddev;
    result->delay_min_ms = delay_stats.min;
    result->delay_mean_ms = delay_stats.mean;
    result->jitter_ms = offset_stats.stddev;
    result->score = gw_ntp_score(result);
    return 0;
}

int gw_ntp_checker_explain(
    const gw_ntp_checker_result_t *result,
    gw_ntp_explain_t *explain)
{
    if (result == NULL || explain == NULL) {
        return -1;
    }
    memset(explain, 0, sizeof(*explain));

    snprintf(
        explain->lines[explain->count++],
        sizeof(explain->lines[0]),
        "+ reachability %.0f%% (%d/%d)",
        result->reachability * 100.0,
        result->success_samples,
        result->total_samples);

    if (result->jitter_ms <= 10.0) {
        snprintf(explain->lines[explain->count++], sizeof(explain->lines[0]), "+ low jitter %.2fms", result->jitter_ms);
    } else {
        snprintf(explain->lines[explain->count++], sizeof(explain->lines[0]), "- high jitter %.2fms", result->jitter_ms);
    }

    if (result->delay_min_ms <= 100.0) {
        snprintf(explain->lines[explain->count++], sizeof(explain->lines[0]), "+ low delay %.2fms", result->delay_min_ms);
    } else {
        snprintf(explain->lines[explain->count++], sizeof(explain->lines[0]), "- high delay %.2fms", result->delay_min_ms);
    }

    snprintf(
        explain->lines[explain->count++],
        sizeof(explain->lines[0]),
        "offset median %.2fms",
        result->offset_median_ms);
    return 0;
}
