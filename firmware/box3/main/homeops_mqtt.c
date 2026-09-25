#include "homeops_mqtt.h"

#include <stdbool.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h"

#include "aircraft_mqtt.h"
#include "claude_usage_mqtt.h"
#include "codex_usage_mqtt.h"
#include "lights_mqtt.h"
#include "fx_mqtt.h"
#include "notification_mqtt.h"
#include "screens.h"
#include "spotify_mqtt.h"
#include "systems_mqtt.h"
#include "weather_mqtt.h"

static const char *TAG = "homeops_mqtt";

#define BROKER_URI "mqtt://pilab.local:1883"
/* Mosquitto (PiLab) started requiring auth 2026-09-22 (was allow_anonymous
 * true - anyone on the LAN could read every topic here, including
 * lights control, and inject fake messages). Same credential used by
 * n8n's "Mosquitto (PiLab)" credential and sky's claude/codex usage
 * pollers - see PROJECT.md. Username isn't sensitive on its own, kept
 * as a plain constant; the actual password comes from CONFIG_MQTT_PASSWORD
 * (Kconfig.projbuild), real value in sdkconfig.local (gitignored) - same
 * established pattern this project already uses for CONFIG_WIFI_PASSWORD
 * and CONFIG_TOTP_SECRET_BASE32, not a new mechanism. */
#define MQTT_USERNAME "homeops"

static bool topic_is(const esp_mqtt_event_handle_t event, const char *topic)
{
    return event->topic_len == strlen(topic) && strncmp(event->topic, topic, event->topic_len) == 0;
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;

    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED: {
        ESP_LOGI(TAG, "Connected to broker, subscribing");
        esp_mqtt_client_subscribe(event->client, AIRCRAFT_STATE_TOPIC, 0);
        esp_mqtt_client_subscribe(event->client, WEATHER_STATE_TOPIC, 0);
        esp_mqtt_client_subscribe(event->client, SYSTEMS_STATE_TOPIC, 0);
        esp_mqtt_client_subscribe(event->client, LIGHTS_STATE_TOPIC, 0);
        esp_mqtt_client_subscribe(event->client, CLAUDE_USAGE_STATE_TOPIC, 0);
        esp_mqtt_client_subscribe(event->client, NOTIFICATION_STATE_TOPIC, 0);
        esp_mqtt_client_subscribe(event->client, SPOTIFY_STATE_TOPIC, 0);
        esp_mqtt_client_subscribe(event->client, CODEX_USAGE_STATE_TOPIC, 0);
        esp_mqtt_client_subscribe(event->client, F1_SESSION_TOPIC, 0);
        esp_mqtt_client_subscribe(event->client, FX_STATE_TOPIC, 0);
        /* Mirrors the MQTT_EVENT_DISCONNECTED handling below - clears the
         * local "pilab_link" alert and flips the Systems row back green
         * immediately on (re)connect, rather than waiting for the next
         * real homeops/systems/state message (still comes shortly after
         * from n8n and will keep it in sync going forward; this just
         * removes the gap where a just-recovered PiLab would otherwise
         * still show red for up to a few seconds). Harmless no-op on the
         * very first boot connect, since nothing set the alert yet. */
        home_screen_clear_notification("pilab_link");
        system_status_t pilab_status = {.online = true, .ip = "pilab.local", .has_temp = false, .temp_c = 0.0};
        systems_screen_set_status("pilab", &pilab_status);
        break;
    }
    case MQTT_EVENT_DATA:
        /* A message still bigger than buffer.size arrives in pieces; a
         * piece is never valid JSON on its own, so drop it loudly rather
         * than letting a handler fail to parse it silently. */
        if (event->total_data_len > event->data_len) {
            if (event->current_data_offset == 0) {
                ESP_LOGW(TAG, "Dropping %d-byte message on %.*s - exceeds MQTT rx buffer",
                         event->total_data_len, event->topic_len, event->topic);
            }
            break;
        }
        if (topic_is(event, AIRCRAFT_STATE_TOPIC)) {
            aircraft_mqtt_handle_message(event->data, event->data_len);
        } else if (topic_is(event, WEATHER_STATE_TOPIC)) {
            weather_mqtt_handle_message(event->data, event->data_len);
        } else if (topic_is(event, SYSTEMS_STATE_TOPIC)) {
            systems_mqtt_handle_message(event->data, event->data_len);
        } else if (topic_is(event, LIGHTS_STATE_TOPIC)) {
            lights_mqtt_handle_message(event->data, event->data_len);
        } else if (topic_is(event, CLAUDE_USAGE_STATE_TOPIC)) {
            claude_usage_mqtt_handle_message(event->data, event->data_len);
        } else if (topic_is(event, NOTIFICATION_STATE_TOPIC)) {
            notification_mqtt_handle_message(event->data, event->data_len);
        } else if (topic_is(event, SPOTIFY_STATE_TOPIC)) {
            spotify_mqtt_handle_message(event->data, event->data_len);
        } else if (topic_is(event, CODEX_USAGE_STATE_TOPIC)) {
            codex_usage_mqtt_handle_message(event->data, event->data_len);
        } else if (topic_is(event, F1_SESSION_TOPIC)) {
            f1_session_mqtt_handle_message(event->data, event->data_len);
        } else if (topic_is(event, FX_STATE_TOPIC)) {
            fx_mqtt_handle_message(event->data, event->data_len);
        }
        break;
    case MQTT_EVENT_DISCONNECTED: {
        ESP_LOGW(TAG, "Disconnected from broker, retrying");
        /* PiLab hosts this broker, so losing the connection to it IS
         * PiLab being unreachable from this device's point of view -
         * added 2026-09-22, explicit request ("get notified on the ESP
         * in case PiLab is off"). Deliberately reusing this already-
         * firing event rather than adding a second MQTT client/broker
         * just for a watchdog signal: this device already runs at
         * ~8.5KB free internal heap (see this file's own task.stack_size
         * history) and a second persistent client's task stack + buffers
         * would eat real, avoidable margin on hardware that's
         * repeatedly hit heap-exhaustion bugs from additions far
         * smaller than that. Own source tag ("pilab_link"), distinct
         * from anything n8n itself might ever publish under "pilab" -
         * two independent producers of a same-meaning alert shouldn't
         * share a source key. systems_screen_set_status() call is what
         * actually matters here beyond the banner: without it the
         * Systems screen's PiLab row just freezes on its last retained
         * value (which n8n's own health-check workflow hardcodes
         * `online: true` for, since that check runs ON PiLab and can
         * never observe its own outage - see PROJECT.md) instead of
         * ever reflecting a real outage. */
        home_screen_set_notification("pilab_link", "PiLab is offline", "red", 0);
        system_status_t pilab_status = {.online = false, .ip = "pilab.local", .has_temp = false, .temp_c = 0.0};
        systems_screen_set_status("pilab", &pilab_status);
        break;
    }
    default:
        break;
    }
}

/* Starts the MQTT client only once WiFi actually has an IP, not right
 * after wifi_time_init() returns (which just kicks off the connection
 * attempt asynchronously) - live bug report 2026-09-16, caught in the
 * serial log: esp_mqtt_client_start()'s internal xTaskCreate failing
 * ("Error create mqtt task" / ESP_FAIL) five times in a row, ~500ms
 * apart, all within the first ~4.7s of boot - well before WiFi's own
 * association even finishes (WiFi doesn't get an IP until ~10s in on
 * this device). That's sustained memory pressure from WiFi's own
 * driver/buffer init competing for this device's scarce internal SRAM
 * during that exact window, not a brief race a quick retry loop can ride
 * out - a short-interval retry (tried first, didn't help - see git
 * history) just retried into the same squeeze five times. Waiting for
 * IP_EVENT_STA_GOT_IP instead means the attempt happens after that
 * initial spike has settled. wifi_time.c registers its own independent
 * handler for the same event - ESP-IDF's event loop supports multiple
 * handlers per event, so this doesn't touch or depend on that file. */
static void ip_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)base;
    (void)event_id;
    (void)event_data;
    esp_mqtt_client_handle_t client = (esp_mqtt_client_handle_t)handler_args;

    /* Still fails occasionally even after got-IP - the network stack does
     * its own post-IP setup work in the same window, so a retry with a
     * real delay (this runs on the event loop's own task, not app_main's,
     * so blocking here doesn't stall boot) covers that too. */
    esp_err_t start_err = ESP_FAIL;
    for (int attempt = 0; attempt < 5; attempt++) {
        start_err = esp_mqtt_client_start(client);
        if (start_err == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG, "esp_mqtt_client_start() attempt %d failed: %s, retrying", attempt + 1,
                 esp_err_to_name(start_err));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (start_err != ESP_OK) {
        ESP_LOGE(TAG, "esp_mqtt_client_start() failed after retries: %s - all MQTT-backed features disabled",
                 esp_err_to_name(start_err));
    }
}

void homeops_mqtt_init(void)
{
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = BROKER_URI,
        .credentials.client_id = "box3-dashboard",
        .credentials.username = MQTT_USERNAME,
        .credentials.authentication.password = CONFIG_MQTT_PASSWORD,
        /* Default MQTT task stack (6KB) is nearly this device's entire
         * ~8.5KB free internal heap in one contiguous request - the real
         * structural cause behind the repeated "Error create mqtt task"
         * failures 2026-09-16, not just bad timing (see ip_event_handler's
         * own comment). Payloads here are small JSON (a couple hundred
         * bytes, no TLS - plain mqtt://), so 4096 is comfortable headroom
         * while asking for a lot less all at once.
         * Briefly bumped to 5120/6144 the same evening chasing a logo-
         * invisible bug, on the theory that mqtt_event_handler's aircraft
         * path also decodes the airline PNG on this same task - reverted
         * back to 4096 once the actual fix turned out to be the image
         * itself (aal.png swapped for an oversized 180-wide wordmark that
         * evening; reverted to the original compact 32x32 - see
         * home_screen.c/spiffs history), which decodes fine at the
         * original size. The extra headroom was never the right fix and
         * this device is already tight on internal heap everywhere else
         * (see the lights/TOTP heap-exhaustion investigation in
         * PROJECT.md) - no reason to keep it reserved unused. */
        .task.stack_size = 4096,
        /* esp-mqtt's default 1024-byte rx buffer splits anything larger
         * across multiple MQTT_EVENT_DATA events, and every *_handle_message
         * here parses a single event - a 1219-byte watchlist/747 aircraft
         * payload (DLH463, 2026-09-22) got its truncated first half fed to
         * cJSON and the whole update dropped, chime included. 2048 gives
         * ~2.5x the current worst-case aircraft payload (~780 bytes after
         * n8n slimmed fa_usage). This device never publishes (only
         * CONNECT/SUBSCRIBE/PINGREQ go out), so the tx buffer - which
         * otherwise defaults to buffer.size - drops to 512: net +512 bytes
         * of internal heap (both are malloc'd under the 16KB
         * SPIRAM_MALLOC_ALWAYSINTERNAL threshold). */
        .buffer.size = 2048,
        .buffer.out_size = 512,
    };

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "esp_mqtt_client_init() returned NULL - all MQTT-backed features disabled");
        return;
    }
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);

    /* One-shot: only the first IP after boot matters here, a later
     * reconnect (new IP after a WiFi drop) doesn't need this - the MQTT
     * client's own library already reconnects itself once started (see
     * MQTT_EVENT_DISCONNECTED above, "retrying"). */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler, client,
                                                          NULL));
}
