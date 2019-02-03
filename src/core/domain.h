#ifndef GW32TIME_DOMAIN_H
#define GW32TIME_DOMAIN_H

#include <windows.h>

typedef struct {
    int joined;
    wchar_t name[256];
} domain_info_t;

int domain_query(domain_info_t *out);

#endif
