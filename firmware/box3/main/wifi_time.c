#include "wifi_time.h"

#include <string.h>
#include <time.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

static const char *TAG = "wifi_time";

static volatile bool s_time_synced = false;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected, retrying");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

static void sntp_sync_cb(struct timeval *tv)
{
    (void)tv;
    s_time_synced = true;
    ESP_LOGI(TAG, "Time synced");
}

void wifi_time_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_WIFI_SSID,
            .password = CONFIG_WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Miami / US Eastern, with DST rules. */
    setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);
    tzset();

    esp_sntp_config_t sntp_config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    sntp_config.sync_cb = sntp_sync_cb;
    ESP_ERROR_CHECK(esp_netif_sntp_init(&sntp_config));

    ESP_LOGI(TAG, "wifi_time_init done, connecting to SSID: %s", CONFIG_WIFI_SSID);
}

bool wifi_time_get_hhmm(char *buf, size_t buf_len)
{
    if (!s_time_synced) {
        return false;
    }

    time_t now = time(NULL);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    strftime(buf, buf_len, "%H:%M", &timeinfo);
    return true;
}

bool wifi_time_is_synced(void)
{
    return s_time_synced;
}

#define NIGHT_START_HOUR 23
#define NIGHT_END_HOUR   7

bool wifi_time_is_night(void)
{
    if (!wifi_time_is_synced()) {
        return false;
    }
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    if (NIGHT_START_HOUR > NIGHT_END_HOUR) { /* wraps midnight, e.g. 23 -> 7 */
        return t.tm_hour >= NIGHT_START_HOUR || t.tm_hour < NIGHT_END_HOUR;
    }
    return t.tm_hour >= NIGHT_START_HOUR && t.tm_hour < NIGHT_END_HOUR;
}
