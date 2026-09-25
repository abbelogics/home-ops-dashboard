#include "weather_mqtt.h"

#include "cJSON.h"
#include "esp_log.h"

#include "screens.h"
#include "weather_screen.h"

static const char *TAG = "weather_mqtt";

static const char *get_string(const cJSON *obj, const char *key)
{
    cJSON *item = cJSON_GetObjectItem(obj, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static int get_int_or_unknown(const cJSON *obj, const char *key)
{
    cJSON *item = cJSON_GetObjectItem(obj, key);
    return cJSON_IsNumber(item) ? item->valueint : -1;
}

/* temp_f only - added 2026-09-14 alongside the weather redesign, which
 * shows one decimal place ("55.9°F", matching the reference) instead of
 * the previously-rounded whole degree every other field still uses. */
static double get_double_or_zero(const cJSON *obj, const char *key)
{
    cJSON *item = cJSON_GetObjectItem(obj, key);
    return cJSON_IsNumber(item) ? item->valuedouble : 0.0;
}

/* Payload is small and always arrives as a single MQTT_EVENT_DATA event in
 * practice, so no reassembly across fragments. */
void weather_mqtt_handle_message(const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (root == NULL) {
        ESP_LOGW(TAG, "Failed to parse weather state JSON");
        return;
    }

    /* icon_v2 (2026-09-24) carries the detailed icon set (light/heavy rain,
     * sun shower, mostly cloudy, night variants); "icon" stays limited to
     * the original 9 keys for older firmware. Prefer v2 when present. */
    const char *icon = get_string(root, "icon_v2");
    if (!icon) {
        icon = get_string(root, "icon");
    }
    if (icon) {
        weather_info_t info = {
            .icon = icon,
            .description = get_string(root, "description"),
            .temp_f = get_double_or_zero(root, "temp_f"),
            .feels_like_f = get_int_or_unknown(root, "feels_like_f"),
            .humidity_pct = get_int_or_unknown(root, "humidity_pct"),
            .wind_mph = get_int_or_unknown(root, "wind_mph"),
            .wind_dir_deg = get_int_or_unknown(root, "wind_dir_deg"),
            .sunrise = get_string(root, "sunrise"),
            .sunset = get_string(root, "sunset"),
            .sunrise_tomorrow = get_string(root, "sunrise_tomorrow"),
            .sunrise_24h = get_string(root, "sunrise_24h"),
            .sunset_24h = get_string(root, "sunset_24h"),
        };
        cJSON *outlook = cJSON_GetObjectItem(root, "outlook");
        if (cJSON_IsObject(outlook)) {
            info.outlook_text = get_string(outlook, "text");
            info.outlook_color = get_string(outlook, "color");
            info.outlook_icon = get_string(outlook, "icon");
        }
        home_screen_set_weather(&info);
    }
    /* The Weather page reads the same payload (hourly, radar, forecast...). */
    weather_screen_update(root);

    cJSON_Delete(root);
}

