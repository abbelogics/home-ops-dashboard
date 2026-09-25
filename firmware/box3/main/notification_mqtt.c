#include "notification_mqtt.h"

#include "cJSON.h"
#include "esp_log.h"

#include "screens.h"

static const char *TAG = "notification_mqtt";

void notification_mqtt_handle_message(const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (root == NULL) {
        ESP_LOGW(TAG, "Failed to parse notification state JSON");
        return;
    }

    cJSON *source_item = cJSON_GetObjectItem(root, "source");
    cJSON *message_item = cJSON_GetObjectItem(root, "message");
    cJSON *severity_item = cJSON_GetObjectItem(root, "severity");
    cJSON *ttl_item = cJSON_GetObjectItem(root, "ttl_min");

    if (!cJSON_IsString(source_item)) {
        ESP_LOGW(TAG, "notification message missing \"source\"");
        cJSON_Delete(root);
        return;
    }

    if (!cJSON_IsString(message_item) || message_item->valuestring[0] == '\0') {
        home_screen_clear_notification(source_item->valuestring);
        cJSON_Delete(root);
        return;
    }

    if (!cJSON_IsString(severity_item)) {
        ESP_LOGW(TAG, "notification message missing \"severity\"");
        cJSON_Delete(root);
        return;
    }

    int ttl_min = cJSON_IsNumber(ttl_item) ? ttl_item->valueint : 0;
    /* Optional, added 2026-09-24 for F1 session alerts - absent on every
     * older source, which behaves exactly as before. */
    cJSON *icon_item = cJSON_GetObjectItem(root, "icon");
    const char *icon = cJSON_IsString(icon_item) ? icon_item->valuestring : NULL;
    bool play_sound = cJSON_IsTrue(cJSON_GetObjectItem(root, "sound"));
    home_screen_set_notification_ex(source_item->valuestring, message_item->valuestring, severity_item->valuestring,
                                     ttl_min, icon, play_sound);

    cJSON_Delete(root);
}

void f1_session_mqtt_handle_message(const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (root == NULL) {
        ESP_LOGW(TAG, "Failed to parse F1 session JSON");
        return;
    }
    cJSON *code_item = cJSON_GetObjectItem(root, "code");
    cJSON *ends_item = cJSON_GetObjectItem(root, "ends_at");
    const char *code = cJSON_IsString(code_item) ? code_item->valuestring : "";
    cJSON *gp_item = cJSON_GetObjectItem(root, "gp");
    const char *gp = cJSON_IsString(gp_item) ? gp_item->valuestring : "";
    long long ends_at = cJSON_IsNumber(ends_item) ? (long long)ends_item->valuedouble : 0;
    home_screen_set_f1_session(code, gp, ends_at);
    cJSON_Delete(root);
}
