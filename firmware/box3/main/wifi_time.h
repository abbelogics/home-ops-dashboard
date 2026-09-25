#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Connects to WiFi (credentials from Kconfig) and starts SNTP sync.
 * Non-blocking - call once at startup; the UI should not wait on this. */
void wifi_time_init(void);

/* Writes local time as "HH:MM" into buf and returns true, or returns false
 * (leaving buf untouched) if time hasn't synced yet. */
bool wifi_time_get_hhmm(char *buf, size_t buf_len);

/* True once SNTP has completed its first sync - anything that needs a
 * correct real-world clock (e.g. totp.c's time-based codes) should check
 * this before trusting time(NULL); before sync, the system clock is
 * whatever it powered on with (effectively the epoch), not real time. */
bool wifi_time_is_synced(void);

/* Night mode window, 23:00-07:00 local (explicit request 2026-09-24): no
 * sounds at all, screen forced to sleep (1%) at 23:00, motion wakes only
 * to DISPLAY_NIGHT_BRIGHTNESS_PCT. False until SNTP has synced, so an
 * unsynced clock never silences anything. */
bool wifi_time_is_night(void);
