#include "systems_mqtt.h"

#include "cJSON.h"
#include "esp_log.h"

#include "screens.h"

static const char *TAG = "systems_mqtt";

/* Payload keys - must match systems_screen.c's row keys and the health
 * check workflow's Parse Into Rows code (see
 * ../n8n/rebuild_health_check_workflow.py). */
static const char *SYSTEM_KEYS[] = {"nas", "pilab", "abbe", "sky"};
#define SYSTEM_KEY_COUNT (sizeof(SYSTEM_KEYS) / sizeof(SYSTEM_KEYS[0]))

void systems_mqtt_handle_message(const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (root == NULL) {
        ESP_LOGW(TAG, "Failed to parse systems state JSON");
        return;
    }

    for (size_t i = 0; i < SYSTEM_KEY_COUNT; i++) {
        cJSON *entry = cJSON_GetObjectItem(root, SYSTEM_KEYS[i]);
        if (!cJSON_IsObject(entry)) {
            continue;
        }

        cJSON *ip_item = cJSON_GetObjectItem(entry, "ip");
        cJSON *temp_item = cJSON_GetObjectItem(entry, "temp_c");

        system_status_t status = {
            .online = cJSON_IsTrue(cJSON_GetObjectItem(entry, "online")),
            .ip = cJSON_IsString(ip_item) ? ip_item->valuestring : NULL,
            .has_temp = cJSON_IsNumber(temp_item),
            .temp_c = cJSON_IsNumber(temp_item) ? temp_item->valuedouble : 0.0,
        };
        systems_screen_set_status(SYSTEM_KEYS[i], &status);
    }

    cJSON_Delete(root);
}

