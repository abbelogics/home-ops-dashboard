#include "codex_usage_mqtt.h"

#include <stdio.h>

#include "cJSON.h"
#include "esp_log.h"

#include "screens.h"

static const char *TAG = "codex_usage_mqtt";

#define NOTIFICATION_SOURCE "codex_usage"

/* Same banding as claude_usage_mqtt.c's checkpoint_band() (see that
 * file's own comment for why - a raw live percentage re-fires the
 * notification on nearly every poll). Duplicated rather than shared
 * because the two are independent single-purpose functions already this
 * small - not worth a shared header for two copies of a 5-line table. */
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

/* Deliberately ONE combined notification source, not split into
 * separate 5h/weekly ones - claude_usage_mqtt.c's own history notes a
 * split version of this exact pattern once stalled the device silently
 * at boot (root cause never isolated). No reason to re-risk that here
 * when a single source already covers both windows fine (bands are the
 * higher of the two percentages, same as the Claude version). */
static void check_usage_notification(int pct_5h, int pct_weekly)
{
    int pct = pct_5h > pct_weekly ? pct_5h : pct_weekly;
    int band = checkpoint_band(pct);
    if (band == 0) {
        home_screen_clear_notification(NOTIFICATION_SOURCE);
        return;
    }
    char message[48];
    if (band >= 100) {
        snprintf(message, sizeof(message), "Codex usage at 100%% - limit reached");
    } else if (band >= 90) {
        snprintf(message, sizeof(message), "Codex usage at %d%%+ - near limit", band);
    } else {
        snprintf(message, sizeof(message), "Codex usage at %d%%+", band);
    }
    home_screen_set_notification(NOTIFICATION_SOURCE, message, band >= 90 ? "red" : "orange", 0);
}

void codex_usage_mqtt_handle_message(const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (root == NULL) {
        ESP_LOGW(TAG, "Failed to parse codex usage state JSON");
        return;
    }

    cJSON *pct_5h = cJSON_GetObjectItem(root, "pct_5h");
    cJSON *reset_min_5h = cJSON_GetObjectItem(root, "reset_min_5h");
    cJSON *pct_weekly = cJSON_GetObjectItem(root, "pct_weekly");
    cJSON *reset_min_weekly = cJSON_GetObjectItem(root, "reset_min_weekly");

    if (cJSON_IsNumber(pct_5h) && cJSON_IsNumber(reset_min_5h) && cJSON_IsNumber(pct_weekly) &&
        cJSON_IsNumber(reset_min_weekly)) {
        codex_usage_screen_set_usage(pct_5h->valueint, reset_min_5h->valueint, pct_weekly->valueint,
                                      reset_min_weekly->valueint);
        check_usage_notification(pct_5h->valueint, pct_weekly->valueint);
    }

    cJSON_Delete(root);
}
