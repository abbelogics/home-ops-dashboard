#include "lights_mqtt.h"

#include <stdio.h>

#include "cJSON.h"
#include "esp_log.h"

#include "screens.h"
#include "webhook_post.h"

static const char *TAG = "lights_mqtt";

/* Plain HTTP, not TLS - n8n runs unencrypted on the LAN (see
 * BROKER_URI above for the same PiLab host). The Hue bridge's own HTTPS
 * call happens n8n-side, not here. */
#define SET_WEBHOOK_URL "http://pilab.local:5678/webhook/lights-set"

void lights_mqtt_handle_message(const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (root == NULL) {
        ESP_LOGW(TAG, "Failed to parse lights state JSON");
        return;
    }

    cJSON *on_item = cJSON_GetObjectItem(root, "on");
    if (cJSON_IsBool(on_item)) {
        lights_screen_set_state(cJSON_IsTrue(on_item));
    }

    cJSON *bri_item = cJSON_GetObjectItem(root, "bri");
    if (cJSON_IsNumber(bri_item)) {
        lights_screen_set_brightness(bri_item->valueint);
    }

    cJSON_Delete(root);
}

/* Shares webhook_post.c's single worker task with spotify_mqtt.c rather
 * than owning its own - see webhook_post.h for why (a second caller-
 * owned dedicated task cost another ~4KB of internal SRAM stack and
 * caused a real task-watchdog crash, caught live 2026-09-16). */
void lights_mqtt_set(bool on)
{
    char body[32];
    snprintf(body, sizeof(body), "{\"on\":%s}", on ? "true" : "false");
    webhook_post_enqueue(SET_WEBHOOK_URL, body);
}

void lights_mqtt_set_brightness(int bri)
{
    if (bri < 1) {
        bri = 1;
    } else if (bri > 254) {
        bri = 254;
    }

    char body[32];
    snprintf(body, sizeof(body), "{\"bri\":%d}", bri);
    webhook_post_enqueue(SET_WEBHOOK_URL, body);
}
