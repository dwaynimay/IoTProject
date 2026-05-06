// =============================================================================
// Watchdog.cpp
// =============================================================================
#include "Watchdog.h"

// Definisi variabel global RTC
RTC_DATA_ATTR RestartLog g_restartLog;

// Instance global — dipakai di main.cpp dan semua task
SystemWatchdog g_watchdog;