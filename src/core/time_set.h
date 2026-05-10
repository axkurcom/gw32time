#ifndef GW32TIME_TIME_SET_H
#define GW32TIME_TIME_SET_H

#include <windows.h>

int time_set_can_adjust(void);
int time_set_local(const SYSTEMTIME *st);

#endif
