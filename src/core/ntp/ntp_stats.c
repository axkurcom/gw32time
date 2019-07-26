#include "ntp_stats.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static int compare_double_asc(const void *left, const void *right)
{
    double a = *(const double *)left;
    double b = *(const double *)right;

    if (a < b) {
        return -1;
    }
    if (a > b) {
        return 1;
    }
    return 0;
}

int gw_ntp_stats_calculate(const double *values, int count, gw_ntp_stats_result_t *out)
{
    double *sorted;
    double sum = 0.0;
    double variance_sum = 0.0;
    int i;

    if (values == NULL || out == NULL || count <= 0) {
        return -1;
    }

    sorted = (double *)malloc((size_t)count * sizeof(double));
    if (sorted == NULL) {
        return -1;
    }
    memcpy(sorted, values, (size_t)count * sizeof(double));
    qsort(sorted, (size_t)count, sizeof(double), compare_double_asc);

    out->min = sorted[0];
    out->max = sorted[count - 1];
    for (i = 0; i < count; i++) {
        sum += sorted[i];
    }
    out->mean = sum / (double)count;

    if ((count % 2) == 0) {
        out->median = (sorted[(count / 2) - 1] + sorted[count / 2]) / 2.0;
    } else {
        out->median = sorted[count / 2];
    }

    if (count <= 1) {
        out->stddev = 0.0;
    } else {
        for (i = 0; i < count; i++) {
            double d = sorted[i] - out->mean;
            variance_sum += d * d;
        }
        out->stddev = sqrt(variance_sum / (double)(count - 1));
    }

    free(sorted);
    return 0;
}

int gw_ntp_stats_filter_outliers_median(
    const double *values,
    int count,
    double threshold_ms,
    double *filtered,
    int filtered_capacity,
    int *filtered_count)
{
    gw_ntp_stats_result_t stats;
    int i;
    int out_count = 0;

    if (values == NULL || filtered == NULL || filtered_count == NULL || count <= 0 || filtered_capacity <= 0 || threshold_ms < 0.0) {
        return -1;
    }
    if (gw_ntp_stats_calculate(values, count, &stats) != 0) {
        return -1;
    }

    for (i = 0; i < count && out_count < filtered_capacity; i++) {
        double delta = fabs(values[i] - stats.median);
        if (delta <= threshold_ms) {
            filtered[out_count++] = values[i];
        }
    }
    *filtered_count = out_count;
    return 0;
}

int gw_ntp_stats_mad(const double *values, int count, double *median_out, double *mad_out)
{
    gw_ntp_stats_result_t stats;
    double *deviations;
    gw_ntp_stats_result_t dev_stats;
    int i;

    if (values == NULL || median_out == NULL || mad_out == NULL || count <= 0) {
        return -1;
    }
    if (gw_ntp_stats_calculate(values, count, &stats) != 0) {
        return -1;
    }

    deviations = (double *)malloc((size_t)count * sizeof(double));
    if (deviations == NULL) {
        return -1;
    }
    for (i = 0; i < count; i++) {
        deviations[i] = fabs(values[i] - stats.median);
    }
    if (gw_ntp_stats_calculate(deviations, count, &dev_stats) != 0) {
        free(deviations);
        return -1;
    }
    free(deviations);

    *median_out = stats.median;
    *mad_out = dev_stats.median;
    return 0;
}
