#include "notification_sound.h"

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>

#include "esp_codec_dev.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "bsp/esp-box-3.h"
#include "wifi_time.h"

static const char *TAG = "notification_sound";

#define BEEP_SAMPLE_RATE 16000
#define BEEP_FREQ_HZ     1200
#define BEEP_DURATION_MS 150
#define BEEP_SAMPLE_COUNT (BEEP_SAMPLE_RATE * BEEP_DURATION_MS / 1000)
#define BEEP_FADE_MS     5
#define BEEP_FADE_SAMPLES (BEEP_SAMPLE_RATE * BEEP_FADE_MS / 1000)

static esp_codec_dev_handle_t s_speaker_dev;
static QueueHandle_t s_beep_queue;

static void beep_task(void *arg)
{
    (void)arg;

    int16_t *buf = malloc(BEEP_SAMPLE_COUNT * sizeof(int16_t));
    if (!buf) {
        ESP_LOGE(TAG, "No memory for beep buffer - notification sound disabled");
        vTaskDelete(NULL);
        return;
    }

    /* Generated once, reused for every beep - a short 5ms fade in/out
     * avoids the audible click a hard-edged tone would otherwise have. */
    for (int i = 0; i < BEEP_SAMPLE_COUNT; i++) {
        float t = (float)i / BEEP_SAMPLE_RATE;
        float amp = 0.5f;
        if (i < BEEP_FADE_SAMPLES) {
            amp *= (float)i / BEEP_FADE_SAMPLES;
        } else if (i > BEEP_SAMPLE_COUNT - BEEP_FADE_SAMPLES) {
            amp *= (float)(BEEP_SAMPLE_COUNT - i) / BEEP_FADE_SAMPLES;
        }
        buf[i] = (int16_t)(amp * 32767.0f * sinf(2.0f * (float)M_PI * BEEP_FREQ_HZ * t));
    }

    bool dummy;
    while (1) {
        if (xQueueReceive(s_beep_queue, &dummy, portMAX_DELAY) == pdTRUE) {
            int ret = esp_codec_dev_write(s_speaker_dev, buf, BEEP_SAMPLE_COUNT * sizeof(int16_t));
            ESP_LOGI(TAG, "beep played, esp_codec_dev_write ret=%d", ret);
        }
    }
}

void notification_sound_init(void)
{
    /* bsp_audio_codec_speaker_init() calls bsp_audio_init(NULL) itself
     * only if the I2S peripheral isn't already up - it is by the time
     * this runs (lights_voice_init() brings it up first at the required
     * 16kHz, see that file's own comment on why the BSP's own 22050Hz
     * default is wrong for this project), so this reuses that same bus
     * rather than re-initializing it, same as bsp_audio_codec_
     * microphone_init() already does. */
    s_speaker_dev = bsp_audio_codec_speaker_init();
    if (!s_speaker_dev) {
        ESP_LOGW(TAG, "Speaker init failed - notification sound disabled");
        return;
    }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = 0,
        .sample_rate = BEEP_SAMPLE_RATE,
        .mclk_multiple = 0,
    };
    if (esp_codec_dev_open(s_speaker_dev, &fs) != 0) {
        ESP_LOGW(TAG, "Speaker open failed - notification sound disabled");
        s_speaker_dev = NULL;
        return;
    }
    int vol_ret = esp_codec_dev_set_out_vol(s_speaker_dev, 70);

    s_beep_queue = xQueueCreate(2, sizeof(bool));
    xTaskCreate(beep_task, "notif_beep", 3072, NULL, tskIDLE_PRIORITY + 1, NULL);
    ESP_LOGI(TAG, "Notification sound ready (set_out_vol ret=%d)", vol_ret);
}

void notification_sound_beep(void)
{
    if (!s_speaker_dev || !s_beep_queue) {
        ESP_LOGW(TAG, "beep requested but sound not initialized (dev=%p queue=%p)", (void *)s_speaker_dev,
                  (void *)s_beep_queue);
        return;
    }
    if (wifi_time_is_night()) {
        ESP_LOGI(TAG, "beep suppressed - night mode (23:00-07:00)");
        return;
    }
    ESP_LOGI(TAG, "beep requested, queuing");
    /* Non-blocking, 0 ticks to wait - a beep already queued is enough,
     * no need to pile up a backlog if several notifications change in
     * quick succession. */
    bool dummy = true;
    xQueueSend(s_beep_queue, &dummy, 0);
}
