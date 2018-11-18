#ifndef GW32TIME_CONFIG_FILE_H
#define GW32TIME_CONFIG_FILE_H

#include "w32time.h"

int config_file_write(const wchar_t *path, const w32time_config_t *config);
int config_file_read(const wchar_t *path, w32time_config_t *config);

#endif
