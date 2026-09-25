#include "totp.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "mbedtls/md.h"

#include "wifi_time.h"

static const char *TAG = "totp";

#define TOTP_STEP_SECONDS 30
#define TOTP_DIGITS       6

/* RFC 4648 base32 (no padding needed - Google Authenticator-style
 * secrets, which is what TOTP enrollment QR codes/manual-entry strings
 * always use, never pad). Case-insensitive, and silently skips any '='
 * padding or whitespace a user might paste in by hand. */
static int base32_decode(const char *in, uint8_t *out, int out_cap)
{
    static const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    int out_len = 0;
    uint32_t buffer = 0;
    int bits = 0;

    for (const char *p = in; *p; p++) {
        char c = *p;
        if (c == '=' || c == ' ' || c == '-') {
            continue;
        }
        if (c >= 'a' && c <= 'z') {
            c -= ('a' - 'A');
        }
        const char *pos = strchr(alphabet, c);
        if (!pos) {
            ESP_LOGE(TAG, "Invalid base32 character in TOTP secret: '%c'", *p);
            return -1;
        }

        buffer = (buffer << 5) | (uint32_t)(pos - alphabet);
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            if (out_len >= out_cap) {
                ESP_LOGE(TAG, "TOTP secret longer than expected buffer");
                return -1;
            }
            out[out_len++] = (uint8_t)((buffer >> bits) & 0xFF);
        }
    }

    return out_len;
}

/* Cached across calls within this file - the secret doesn't change at
 * runtime, and re-decoding base32 on every call (up to once/second from
 * the screen's own refresh timer) is pointless work. -1 = not yet
 * decoded, 0 = decoded and empty/invalid, >0 = decoded length. */
static uint8_t s_key[32];
static int s_key_len = -1;

static bool ensure_key_decoded(void)
{
    if (s_key_len >= 0) {
        return s_key_len > 0;
    }

    if (CONFIG_TOTP_SECRET_BASE32[0] == '\0') {
        ESP_LOGW(TAG, "CONFIG_TOTP_SECRET_BASE32 is empty - TOTP screen disabled");
        s_key_len = 0;
        return false;
    }

    int len = base32_decode(CONFIG_TOTP_SECRET_BASE32, s_key, sizeof(s_key));
    if (len <= 0) {
        s_key_len = 0;
        return false;
    }

    s_key_len = len;
    return true;
}

static bool compute_code(time_t now, char *buf, int buf_len)
{
    if (!ensure_key_decoded()) {
        return false;
    }

    uint64_t counter = (uint64_t)now / TOTP_STEP_SECONDS;
    uint8_t counter_be[8];
    for (int i = 7; i >= 0; i--) {
        counter_be[i] = (uint8_t)(counter & 0xFF);
        counter >>= 8;
    }

    uint8_t digest[20]; /* SHA1 output size */
    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
    if (mbedtls_md_hmac(md_info, s_key, (size_t)s_key_len, counter_be, sizeof(counter_be), digest) != 0) {
        ESP_LOGE(TAG, "HMAC-SHA1 failed");
        return false;
    }

    int offset = digest[19] & 0x0F;
    uint32_t binary = ((uint32_t)(digest[offset] & 0x7F) << 24) | ((uint32_t)digest[offset + 1] << 16) |
                       ((uint32_t)digest[offset + 2] << 8) | (uint32_t)digest[offset + 3];

    uint32_t divisor = 1;
    for (int i = 0; i < TOTP_DIGITS; i++) {
        divisor *= 10;
    }
    snprintf(buf, buf_len, "%0*u", TOTP_DIGITS, (unsigned)(binary % divisor));
    return true;
}

bool totp_get_code(char *buf, int buf_len)
{
    if (!wifi_time_is_synced()) {
        return false;
    }
    return compute_code(time(NULL), buf, buf_len);
}

int totp_seconds_remaining(void)
{
    if (!wifi_time_is_synced()) {
        return 0;
    }
    return TOTP_STEP_SECONDS - (int)(time(NULL) % TOTP_STEP_SECONDS);
}
