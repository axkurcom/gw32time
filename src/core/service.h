#ifndef GW32TIME_SERVICE_H
#define GW32TIME_SERVICE_H

#include <windows.h>

typedef enum {
    SVC_STATE_UNKNOWN = 0,
    SVC_STATE_STOPPED,
    SVC_STATE_START_PENDING,
    SVC_STATE_STOP_PENDING,
    SVC_STATE_RUNNING,
    SVC_STATE_CONTINUE_PENDING,
    SVC_STATE_PAUSE_PENDING,
    SVC_STATE_PAUSED
} svc_state_t;

typedef enum {
    SVC_START_UNKNOWN = 0,
    SVC_START_BOOT,
    SVC_START_SYSTEM,
    SVC_START_AUTO,
    SVC_START_MANUAL,
    SVC_START_DISABLED
} svc_start_type_t;

int svc_query_state(const wchar_t *name, svc_state_t *out);
int svc_query_start_type(const wchar_t *name, svc_start_type_t *out);

const wchar_t *svc_state_name(svc_state_t state);
const wchar_t *svc_start_type_name(svc_start_type_t start_type);

#endif
