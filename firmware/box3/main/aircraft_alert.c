#include "aircraft_alert.h"

#include <math.h>
#include <string.h>

#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "bsp/esp-box-3.h"
#include "wifi_time.h"

static const char *TAG = "aircraft_alert";

#define ALERT_SAMPLE_RATE 16000

/* A soft three-note ascending chime (C5-E5-G5, a plain major triad - the
 * same shape as a typical phone/OS notification sound), not an alarm
 * beep - explicit request ("a nice sound, something pleasant"). Same
 * chime for all three trigger kinds (747/A380/military); `kind` is only
 * used for logging. A softer, lower-register (C4-E4-G4) variant was tried
 * based on "sounds too low"/"softer chime" feedback, but after previewing
 * ~10 synthesized candidates locally (afplay, not on-device - much faster
 * to iterate on than a flash cycle per attempt), this original C5-E5-G5
 * version at its original tempo/fade was the one actually preferred. */
static const float NOTE_HZ[] = {523.25f, 659.25f, 783.99f};
#define NOTE_MS      150
#define NOTE_GAP_MS  20
#define FADE_MS      15

typedef struct {
    char kind[16];
} alert_request_t;

static QueueHandle_t s_queue;
static esp_codec_dev_handle_t s_spk_dev;

/* Lazy - only created on the first actual alert, well after boot, so this
 * never races lights_voice_init()'s own I2S/speaker-adjacent setup. */
static void ensure_speaker(void)
{
    if (s_spk_dev) {
        return;
    }

    s_spk_dev = bsp_audio_codec_speaker_init();
    if (!s_spk_dev) {
        ESP_LOGE(TAG, "Speaker init failed - aircraft alert sound disabled");
        return;
    }

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = ALERT_SAMPLE_RATE,
        .channel = 1,
        .bits_per_sample = 16,
    };
    if (esp_codec_dev_open(s_spk_dev, &fs) != 0) {
        ESP_LOGE(TAG, "Speaker open failed - aircraft alert sound disabled");
        s_spk_dev = NULL;
        return;
    }
    /* 65 (a mid-range guess) was reported as too quiet on real hardware -
     * pushed toward the top of the range. */
    esp_codec_dev_set_out_vol(s_spk_dev, 90);
}

/* Sine fundamental plus a quieter second harmonic (an octave up, 1/4 the
 * amplitude) - a pure single sine tends to sound like a thin, cheap
 * electronic beep on a small speaker; this is closer to a soft bell/chime
 * timbre. Linear fade in/out avoids the click a hard-edged tone burst
 * would have. */
static void play_note(float freq_hz)
{
    int n = ALERT_SAMPLE_RATE * NOTE_MS / 1000;
    int fade = ALERT_SAMPLE_RATE * FADE_MS / 1000;
    int16_t *buf = heap_caps_malloc(n * sizeof(int16_t), MALLOC_CAP_DEFAULT);
    if (!buf) {
        ESP_LOGE(TAG, "No memory for alert tone buffer");
        return;
    }

    for (int i = 0; i < n; i++) {
        float t = (float)i / ALERT_SAMPLE_RATE;
        float env = 1.0f;
        if (i < fade) {
            env = (float)i / fade;
        } else if (i > n - fade) {
            env = (float)(n - i) / fade;
        }
        float sample = 0.8f * sinf(2.0f * (float)M_PI * freq_hz * t)
                     + 0.2f * sinf(2.0f * (float)M_PI * freq_hz * 2.0f * t);
        /* 9000 (paired with the raised codec volume in ensure_speaker)
         * was confirmed good quality but requested a bit louder still.
         * 13000 previewed clean (no distortion) locally - well short of
         * the 22000 that overdrove the real speaker earlier - so nudged
         * up to that instead of jumping to another extreme. */
        buf[i] = (int16_t)(env * 13000.0f * sample);
    }

    esp_codec_dev_write(s_spk_dev, buf, n * sizeof(int16_t));
    heap_caps_free(buf);

    int gap_n = ALERT_SAMPLE_RATE * NOTE_GAP_MS / 1000;
    int16_t *silence = heap_caps_calloc(gap_n, sizeof(int16_t), MALLOC_CAP_DEFAULT);
    if (silence) {
        esp_codec_dev_write(s_spk_dev, silence, gap_n * sizeof(int16_t));
        heap_caps_free(silence);
    }
}

/* F1 session alert (2026-09-24) - explicitly wanted distinct from both the
 * red/orange beep (single 1200Hz) and the aircraft chime (C-E-G triad).
 * "Start lights": five short low blips (the five red lights coming on),
 * then one longer higher tone (lights out). Tone buffers go to PSRAM
 * explicitly - the lights-out tone is ~11KB, which a plain malloc would
 * try to take from internal RAM first (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL
 * = 16384), the pool this device can't spare. */
static void play_tone_psram(float freq_hz, int ms, int gap_ms)
{
    int n = ALERT_SAMPLE_RATE * ms / 1000;
    int gap_n = ALERT_SAMPLE_RATE * gap_ms / 1000;
    int fade = ALERT_SAMPLE_RATE * 8 / 1000;
    int16_t *buf = heap_caps_calloc(n + gap_n, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "No PSRAM for F1 tone buffer");
        return;
    }
    for (int i = 0; i < n; i++) {
        float t = (float)i / ALERT_SAMPLE_RATE;
        float env = 1.0f;
        if (i < fade) {
            env = (float)i / fade;
        } else if (i > n - fade) {
            env = (float)(n - i) / fade;
        }
        float sample = 0.8f * sinf(2.0f * (float)M_PI * freq_hz * t)
                     + 0.2f * sinf(2.0f * (float)M_PI * freq_hz * 2.0f * t);
        buf[i] = (int16_t)(env * 13000.0f * sample);
    }
    esp_codec_dev_write(s_spk_dev, buf, (n + gap_n) * sizeof(int16_t));
    heap_caps_free(buf);
}

static void play_f1_start_lights(void)
{
    for (int i = 0; i < 5; i++) {
        play_tone_psram(440.0f, 70, 130);
    }
    play_tone_psram(880.0f, 350, 0);
}

static void alert_task(void *arg)
{
    (void)arg;

    for (;;) {
        alert_request_t req;
        if (xQueueReceive(s_queue, &req, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        ESP_LOGI(TAG, "Playing alert chime (%s)", req.kind);
        ensure_speaker();
        if (!s_spk_dev) {
            continue;
        }
        if (strcmp(req.kind, "f1") == 0) {
            play_f1_start_lights();
            continue;
        }
        for (size_t i = 0; i < sizeof(NOTE_HZ) / sizeof(NOTE_HZ[0]); i++) {
            play_note(NOTE_HZ[i]);
        }
    }
}

/* Lazy, like ensure_speaker() - creates the queue and worker task on the
 * FIRST actual alert, not at aircraft_alert_init() time. That earlier,
 * eager version hit the exact same silent-failure shape lights_mqtt.c did
 * earlier this session: xTaskCreate() called right after
 * lights_voice_init()'s own AFE/MultiNet allocations couldn't find a free
 * contiguous internal-RAM block (confirmed via logging: 6063 bytes free,
 * not enough for a 4096-byte stack) and returned pdFAIL - nothing crashed,
 * the feature just silently never worked, same as lights_mqtt's original
 * bug. A real aircraft sighting happens well after boot has settled
 * (WiFi/MQTT connected, that initial allocation spike long past), so
 * deferring creation to first use sidesteps this specific tight window
 * entirely rather than trying to out-guess it with a smaller stack size. */
static bool ensure_alert_task(void)
{
    if (s_queue) {
        return true;
    }

    s_queue = xQueueCreate(2, sizeof(alert_request_t));
    if (!s_queue) {
        ESP_LOGE(TAG, "Failed to create alert queue");
        return false;
    }

    /* Internal RAM turned out to be a genuine dead end for this task's
     * stack: 4096 (a generic guess) failed to allocate at all even 25s
     * post-boot (internal RAM stays chronically tight - ~6-7.5KB free -
     * for as long as voice control is running, not just a transient
     * construction-time spike); 2048 DID allocate but overflowed with a
     * live crash once the task actually ran (ensure_speaker()'s
     * i2c/codec driver calls need more than that); 3584 failed to
     * allocate again despite ~7.6KB reported free, pointing at
     * fragmentation - no single contiguous block that big, not just "not
     * enough total". Sidestepped entirely by putting the stack in PSRAM
     * (16MB, essentially unused elsewhere on this device) via
     * xTaskCreateWithCaps() instead of internal DRAM - stops this task
     * from competing with AFE for the same scarce pool at all, so a
     * generous size can be used safely instead of cutting it close. */
    BaseType_t ok = xTaskCreateWithCaps(alert_task, "aircraft_alert", 4096, NULL, tskIDLE_PRIORITY + 1, NULL,
                                         MALLOC_CAP_SPIRAM);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create alert task - free PSRAM: %u bytes, free internal: %u bytes",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        vQueueDelete(s_queue);
        s_queue = NULL;
        return false;
    }
    return true;
}

void aircraft_alert_notify(const char *kind)
{
    /* Night mode: no sounds at all 23:00-07:00 - covers the aircraft chime
     * and the F1 start-lights sound (explicit request 2026-09-24). */
    if (wifi_time_is_night()) {
        ESP_LOGI(TAG, "alert sound (%s) suppressed - night mode", kind ? kind : "");
        return;
    }
    if (!ensure_alert_task()) {
        return;
    }

    alert_request_t req = {0};
    if (kind) {
        strncpy(req.kind, kind, sizeof(req.kind) - 1);
    }
    if (xQueueSend(s_queue, &req, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Alert queue full, dropping request");
    }
}
