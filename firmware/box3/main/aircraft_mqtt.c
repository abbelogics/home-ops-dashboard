#include "aircraft_mqtt.h"

#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#include "aircraft_alert.h"
#include "screens.h"

static const char *TAG = "aircraft_mqtt";

/* Last hex we already fired the alert chime for, so a continuously-visible
 * qualifying aircraft (nearly every 3s poll cycle while it's in range)
 * triggers the sound once, not on every single MQTT message. Cleared once
 * special_aircraft goes back to null, so the same aircraft re-appearing
 * later (or a different one right after) triggers again. */
static char s_last_alert_hex[8] = {0};

static const char *get_string(const cJSON *obj, const char *key)
{
    cJSON *item = cJSON_GetObjectItem(obj, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static double get_number(const cJSON *obj, const char *key, double fallback)
{
    cJSON *item = cJSON_GetObjectItem(obj, key);
    return cJSON_IsNumber(item) ? item->valuedouble : fallback;
}

static int get_int_or_unknown(const cJSON *obj, const char *key)
{
    cJSON *item = cJSON_GetObjectItem(obj, key);
    return cJSON_IsNumber(item) ? item->valueint : -1;
}

/* Payload is up to ~800 bytes and must arrive as a single MQTT_EVENT_DATA
 * event - homeops_mqtt.c sizes the rx buffer for that and drops anything
 * still fragmented, so no reassembly here. */
void aircraft_mqtt_handle_message(const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (root == NULL) {
        ESP_LOGW(TAG, "Failed to parse aircraft state JSON");
        return;
    }

    cJSON *nearest = cJSON_GetObjectItem(root, "nearest");
    cJSON *watchlist = cJSON_GetObjectItem(root, "watchlist_match");
    bool overhead = cJSON_IsTrue(cJSON_GetObjectItem(root, "overhead"));

    const cJSON *source = cJSON_IsObject(watchlist) ? watchlist : nearest;

    /* special_aircraft can be any visible match, not necessarily the one
     * on screen - only treat the displayed plane as special if it's the
     * same airframe (hex). */
    cJSON *special = cJSON_GetObjectItem(root, "special_aircraft");
    const char *special_hex = cJSON_IsObject(special) ? get_string(special, "hex") : NULL;
    const char *source_hex = cJSON_IsObject(source) ? get_string(source, "hex") : NULL;
    const char *display_special_kind =
        (special_hex && source_hex && strcmp(special_hex, source_hex) == 0) ? get_string(special, "kind") : NULL;

    if (cJSON_IsObject(source)) {
        aircraft_info_t info = {
            .is_watchlist = (source == watchlist),
            .label = get_string(source, "label"),
            .airline = get_string(source, "airline"),
            .flight_number = get_string(source, "flight_number"),
            .raw_flight = get_string(source, "flight"),
            .type = get_string(source, "type"),
            .type_desc = get_string(source, "desc"),
            .origin_iata = get_string(source, "origin_iata"),
            .destination_iata = get_string(source, "destination_iata"),
            .route_source = get_string(source, "route_source"),
            .alt_ft = get_int_or_unknown(source, "alt_ft"),
            .heading_deg = get_int_or_unknown(source, "heading_deg"),
            .distance_mi = get_number(source, "distance_mi", 0),
            .bearing_deg = (int)get_number(source, "bearing_deg", 0),
            .look_angle_deg = (int)get_number(source, "look_angle_deg", 0),
            .overhead = overhead,
            .special_kind = display_special_kind,
        };
        home_screen_set_aircraft(&info);
    } else {
        home_screen_clear_aircraft();
    }

    /* Piggybacked onto this same retained topic rather than a separate
     * one (see n8n/rebuild_aircraft_poll_workflow.py) - real numbers from
     * FlightAware's own free GET /account/usage endpoint, not an estimate
     * this firmware computes itself. May be absent/null before the n8n
     * poll workflow's own cache has populated - not an error. */
    cJSON *fa_usage = cJSON_GetObjectItem(root, "fa_usage");
    if (cJSON_IsObject(fa_usage)) {
        cJSON *calls = cJSON_GetObjectItem(fa_usage, "total_calls");
        cJSON *cost = cJSON_GetObjectItem(fa_usage, "total_cost");
        if (cJSON_IsNumber(calls) && cJSON_IsNumber(cost)) {
            flightaware_screen_set_usage(calls->valueint, cost->valuedouble);
        }
    }

    if (special_hex && strcmp(special_hex, s_last_alert_hex) != 0) {
        strncpy(s_last_alert_hex, special_hex, sizeof(s_last_alert_hex) - 1);
        aircraft_alert_notify(get_string(special, "kind"));
    } else if (!special_hex) {
        s_last_alert_hex[0] = '\0';
    }

    cJSON_Delete(root);
}

