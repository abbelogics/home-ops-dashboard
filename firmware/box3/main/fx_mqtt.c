#include "fx_mqtt.h"

#include "cJSON.h"
#include "esp_log.h"

#include "screens.h"

static const char *TAG = "fx_mqtt";

static void apply(cJSON *root, const char *key, fx_currency_t which)
{
    cJSON *obj = cJSON_GetObjectItem(root, key);
    cJSON *rate = cJSON_GetObjectItem(obj, "rate");
    if (!cJSON_IsNumber(rate)) {
        return;
    }
    cJSON *prev = cJSON_GetObjectItem(obj, "prev");
    cJSON *date = cJSON_GetObjectItem(obj, "date");
    fx_screen_set_rate(which, rate->valuedouble, cJSON_IsNumber(prev), cJSON_IsNumber(prev) ? prev->valuedouble : 0,
                       cJSON_IsString(date) ? date->valuestring : "");
}

void fx_mqtt_handle_message(const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (root == NULL) {
        ESP_LOGW(TAG, "Failed to parse FX state JSON");
        return;
    }
    apply(root, "cop", FX_COP);
    apply(root, "eur", FX_EUR);
    apply(root, "gbp", FX_GBP);
    cJSON_Delete(root);
}
