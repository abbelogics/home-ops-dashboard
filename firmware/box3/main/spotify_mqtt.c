#include "spotify_mqtt.h"

#include <stdio.h>

#include "cJSON.h"
#include "esp_log.h"

#include "screens.h"
#include "webhook_post.h"

static const char *TAG = "spotify_mqtt";

/* Plain HTTP, not TLS - same reasoning as lights_mqtt.c's own
 * SET_WEBHOOK_URL: n8n's webhook still answers on this same LAN port
 * regardless of the HTTPS domain now in front of it for OAuth, and a
 * same-LAN POST from the ESP32 doesn't need that overhead. */
#define CONTROL_WEBHOOK_URL "http://pilab.local:5678/webhook/spotify-control"

void spotify_mqtt_handle_message(const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (root == NULL) {
        ESP_LOGW(TAG, "Failed to parse spotify state JSON");
        return;
    }

    cJSON *is_playing_item = cJSON_GetObjectItem(root, "is_playing");
    cJSON *track_item = cJSON_GetObjectItem(root, "track");
    cJSON *artist_item = cJSON_GetObjectItem(root, "artist");
    cJSON *device_item = cJSON_GetObjectItem(root, "device");
    cJSON *volume_item = cJSON_GetObjectItem(root, "volume_percent");

    bool is_playing = cJSON_IsBool(is_playing_item) && cJSON_IsTrue(is_playing_item);
    const char *track = cJSON_IsString(track_item) ? track_item->valuestring : "";
    const char *artist = cJSON_IsString(artist_item) ? artist_item->valuestring : "";
    const char *device = cJSON_IsString(device_item) ? device_item->valuestring : "";
    /* -1 (not just "field missing") means "unknown" - n8n sends this
     * explicitly when Spotify's own response has no device (and so no
     * volume) attached at all, distinct from a real 0%. */
    int volume_percent = cJSON_IsNumber(volume_item) ? volume_item->valueint : -1;

    spotify_screen_set_state(is_playing, track, artist, device, volume_percent);

    cJSON_Delete(root);
}

/* Shares webhook_post.c's single worker task with lights_mqtt.c rather
 * than owning a separate one - see webhook_post.h for why (a second
 * dedicated task here was a real task-watchdog crash, caught live). */
void spotify_mqtt_control(const char *action)
{
    char body[64];
    snprintf(body, sizeof(body), "{\"action\":\"%s\"}", action);
    webhook_post_enqueue(CONTROL_WEBHOOK_URL, body);
}

void spotify_mqtt_set_volume(int volume_percent)
{
    if (volume_percent < 0) {
        volume_percent = 0;
    } else if (volume_percent > 100) {
        volume_percent = 100;
    }

    char body[64];
    snprintf(body, sizeof(body), "{\"action\":\"volume\",\"volume_percent\":%d}", volume_percent);
    webhook_post_enqueue(CONTROL_WEBHOOK_URL, body);
}
