#include "ntp_score.h"

#include <math.h>

double gw_ntp_score(const gw_ntp_checker_result_t *result)
{
    double score = 1.0;

    if (result == NULL) {
        return 0.0;
    }

    score *= result->reachability;

    if (result->delay_min_ms > 100.0) {
        score -= 0.15;
    }
    if (result->jitter_ms > 10.0) {
        score -= 0.20;
    }
    if (fabs(result->offset_median_ms) > 100.0) {
        score -= 0.25;
    }
    if (result->stratum >= 8) {
        score -= 0.10;
    }
    if (result->success_samples < result->total_samples) {
        score -= 0.05;
    }
    if (score < 0.0) {
        score = 0.0;
    }
    if (score > 1.0) {
        score = 1.0;
    }
    return score;
}
