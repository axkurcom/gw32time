#ifndef GW32TIME_NTP_STATS_H
#define GW32TIME_NTP_STATS_H

typedef struct gw_ntp_stats_result {
    double min;
    double max;
    double mean;
    double median;
    double stddev;
} gw_ntp_stats_result_t;

int gw_ntp_stats_calculate(const double *values, int count, gw_ntp_stats_result_t *out);
int gw_ntp_stats_filter_outliers_median(
    const double *values,
    int count,
    double threshold_ms,
    double *filtered,
    int filtered_capacity,
    int *filtered_count);

int gw_ntp_stats_mad(const double *values, int count, double *median_out, double *mad_out);

#endif
