#include "webhook_post.h"

#include <stdio.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "webhook_post";

typedef struct {
    char url[96];
    char body[160];
} webhook_post_request_t;

static QueueHandle_t s_queue;

/* Same persistent-task-with-reserved-stack pattern lights_mqtt.c's own
 * worker used to have (see its git history) - reserved once here at init
 * time (memory is plentiful then), not a fresh task spawned per call
 * (xTaskCreate() needing a contiguous ~4KB block at an arbitrary call
 * moment is a real, previously-hit failure mode on this device). */
static void post_task(void *arg)
{
    (void)arg;

    for (;;) {
        webhook_post_request_t req;
        if (xQueueReceive(s_queue, &req, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        esp_http_client_config_t config = {
            .url = req.url,
            .method = HTTP_METHOD_POST,
            .timeout_ms = 4000,
        };
        esp_http_client_handle_t client = esp_http_client_init(&config);
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, req.body, strlen(req.body));

        esp_err_t err = esp_http_client_perform(client);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "webhook POST to %s failed: %s", req.url, esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "webhook POST to %s ok, status %d", req.url, esp_http_client_get_status_code(client));
        }

        esp_http_client_cleanup(client);
    }
}

void webhook_post_init(void)
{
    s_queue = xQueueCreate(4, sizeof(webhook_post_request_t));
    xTaskCreate(post_task, "webhook_post", 4096, NULL, tskIDLE_PRIORITY + 1, NULL);
}

void webhook_post_enqueue(const char *url, const char *json_body)
{
    webhook_post_request_t req;
    snprintf(req.url, sizeof(req.url), "%s", url);
    snprintf(req.body, sizeof(req.body), "%s", json_body);
    if (xQueueSend(s_queue, &req, 0) != pdTRUE) {
        ESP_LOGW(TAG, "webhook_post queue full, dropping request to %s", url);
    }
}
