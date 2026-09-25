#include "claude_usage_mqtt.h"

#include <stdio.h>

#include "cJSON.h"
#include "esp_log.h"

#include "screens.h"

static const char *TAG = "claude_usage_mqtt";

#define NOTIFICATION_SOURCE "claude_usage"

/* Rounds down to the nearest checkpoint band rather than using the raw
 * live percentage - live bug report 2026-09-16: with the raw percentage
 * in the message text, an actively-climbing session (e.g. this very
 * conversation burning tokens) generated a "fresh occurrence" on
 * home_screen_set_notification()'s own dedup check on nearly every
 * ~2min poll ("81%" -> "82%" -> "83%" is a text change each time), so it
 * kept re-notifying/re-beeping well past the point it had already told
 * you usage was high. Banding to 70/80/90/95/100 means the message text
 * - and therefore the beep - only changes when a real checkpoint is
 * crossed, once per band, no matter how the live number ticks within
 * it. Returns 0 for anything under the lowest band. */
static int checkpoint_band(int pct)
{
    static const int checkpoints[] = {100, 95, 90, 80, 70};
    for (size_t i = 0; i < sizeof(checkpoints) / sizeof(checkpoints[0]); i++) {
        if (pct >= checkpoints[i]) {
            return checkpoints[i];
        }
    }
    return 0;
}

/* Checked directly against the home screen's shared notification strip
 * (screens.h) on every message - no new MQTT topic needed here, this
 * data's already arriving over MQTT and the firmware itself can decide
 * whether it's notification-worthy. Bands are the higher of the two
 * percentages seen, so hitting either session or weekly triggers it.
 * Clears itself once both drop back under the lowest band - see
 * home_screen_clear_notification()'s own comment for why that's safe
 * even though this runs on a ~2 min poll interval and could otherwise
 * stomp on an unrelated notification.
 *
 * NOTE: a session/week SPLIT version of this (two separate sources,
 * "Claude current usage" 80/90/95/100 + "Claude all models" 80/90/100,
 * per the explicit checklist 2026-09-16) was tried here and caused the
 * device to silently stall at boot (stuck right after "cpu_start:
 * Multicore app", before any app_main log at all - "everything is
 * empty" live report) even with NOTIFICATION_MAX_SOURCES independently
 * confirmed stable at 10. Reverted back to this single-source version
 * without isolating the real cause - the split logic itself looked
 * benign (plain int comparisons, no new static storage), so the true
 * cause is still unknown. Re-attempting the split needs to happen far
 * more cautiously than this one attempt was. */
static void check_usage_notification(int session_pct, int week_pct)
{
    int pct = session_pct > week_pct ? session_pct : week_pct;
    int band = checkpoint_band(pct);
    if (band == 0) {
        home_screen_clear_notification(NOTIFICATION_SOURCE);
        return;
    }
    char message[48];
    if (band >= 100) {
        snprintf(message, sizeof(message), "Claude usage at 100%% - limit reached");
    } else if (band >= 90) {
        snprintf(message, sizeof(message), "Claude usage at %d%%+ - near limit", band);
    } else {
        snprintf(message, sizeof(message), "Claude usage at %d%%+", band);
    }
    home_screen_set_notification(NOTIFICATION_SOURCE, message, band >= 90 ? "red" : "orange", 0);
}

void claude_usage_mqtt_handle_message(const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (root == NULL) {
        ESP_LOGW(TAG, "Failed to parse claude usage state JSON");
        return;
    }

    cJSON *session_pct = cJSON_GetObjectItem(root, "session_pct");
    cJSON *session_reset_min = cJSON_GetObjectItem(root, "session_reset_min");
    cJSON *week_pct = cJSON_GetObjectItem(root, "week_pct");
    cJSON *week_reset_min = cJSON_GetObjectItem(root, "week_reset_min");

    if (cJSON_IsNumber(session_pct) && cJSON_IsNumber(session_reset_min) && cJSON_IsNumber(week_pct) &&
        cJSON_IsNumber(week_reset_min)) {
        claude_usage_screen_set_usage(session_pct->valueint, session_reset_min->valueint, week_pct->valueint,
                                       week_reset_min->valueint);
        check_usage_notification(session_pct->valueint, week_pct->valueint);
    }

    cJSON_Delete(root);
}
