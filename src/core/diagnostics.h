#ifndef GW32TIME_DIAGNOSTICS_H
#define GW32TIME_DIAGNOSTICS_H

#include <windows.h>

typedef enum {
    HEALTH_OK = 0,
    HEALTH_WARNING,
    HEALTH_BROKEN,
    HEALTH_UNKNOWN
} health_state_t;

typedef struct {
    health_state_t state;
    wchar_t reason[256];
} health_t;

int diagnostics_evaluate_health(health_t *out);
const wchar_t *health_state_name(health_state_t state);

#endif
