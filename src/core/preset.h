#ifndef GW32TIME_PRESET_H
#define GW32TIME_PRESET_H

#include "w32time.h"

typedef struct {
    const wchar_t *name;
    const wchar_t *display_mode;
    const wchar_t *display_servers;
    w32time_config_t config;
} preset_t;

int preset_lookup(const wchar_t *name, preset_t *out);
int preset_count(void);
const wchar_t *preset_name_at(int index);

#endif
