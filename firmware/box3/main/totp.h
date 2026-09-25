#pragma once

#include <stdbool.h>

/* RFC 6238 TOTP (30s step, SHA1, 6 digits) - the same algorithm Microsoft
 * Authenticator/Google Authenticator/etc. all use, keyed off
 * CONFIG_TOTP_SECRET_BASE32 (see Kconfig.projbuild - real value lives in
 * sdkconfig.local, gitignored). A TOTP secret can be enrolled in more
 * than one authenticator at once - this is a second, independent
 * enrollment of the same account, not a copy of Microsoft Authenticator's
 * own stored secret (no TOTP app exposes that once entered). Needs a
 * synced real-world clock (see wifi_time_is_synced()) - the algorithm is
 * time-keyed, so a wrong clock produces a wrong code with no other
 * symptom. */

/* Computes the current 6-digit code into buf (must be at least 7 bytes,
 * for "123456" + NUL). Returns false (leaving buf untouched) if
 * CONFIG_TOTP_SECRET_BASE32 is empty, isn't valid base32, or time hasn't
 * synced yet (see wifi_time_is_synced()). */
bool totp_get_code(char *buf, int buf_len);

/* Seconds remaining in the current 30s step (0-29) - for a countdown
 * indicator alongside the code. Meaningless (returns 0) if time hasn't
 * synced. */
int totp_seconds_remaining(void);
