#include "lights_voice.h"

#include <stdbool.h>

/* esp-sr's esp_afe_config.h uses CONFIG_IDF_TARGET_ESP32S3 (for
 * AFE_CONFIG_DEFAULT()) without including sdkconfig.h itself, unlike
 * ESP-IDF's own headers which always do - include it explicitly first
 * rather than depend on some other header in the chain happening to pull
 * it in. */
#include "sdkconfig.h"

#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "model_path.h"

#include "driver/i2s_std.h"

#include "bsp/esp-box-3.h"
#include "lights_mqtt.h"
#include "screens.h"

static const char *TAG = "lights_voice";

/* Adapted from Espressif's own factory_demo for this exact board
 * (~/esp/esp-box/examples/factory_demo/main/app/app_sr.c) - stripped down
 * to just this project's two commands, English-only, no Chinese/language
 * switching, no LED/audio-chirp/SD-card-recording chrome that demo also
 * has. The one real adaptation (not just deletion): that reference reads
 * the mic via the older `bsp_i2s_read()`, which doesn't exist in this
 * project's newer esp-box-3 BSP version - this uses the current
 * esp_codec_dev-based API (`bsp_audio_codec_microphone_init()` +
 * `esp_codec_dev_read()`) instead. Everything else about the AFE/wakenet/
 * multinet pipeline shape is the same. */

/* Command IDs registered with MultiNet below - intentionally just two, no
 * need for the reference's dynamic add/remove/modify command-list
 * machinery when the command set is fixed and this small. */
#define CMD_LIGHTS_ON  0
#define CMD_LIGHTS_OFF 1

/* BOX-3's mic array capture is 2 raw I2S channels, but this board's AFE
 * config expects a 3rd (reference) channel slot per frame, zeroed since
 * there's no AEC/echo-cancellation reference signal in use - same fixed
 * hardware fact the reference implementation hardcodes. */
#define MIC_RAW_CHANNELS 2
#define AFE_FEED_CHANNELS 3

typedef struct {
    bool wake;             /* true = wake word just fired, ignore state/command_id */
    esp_mn_state_t state;  /* meaningful only when !wake */
    int command_id;        /* meaningful only when state == ESP_MN_STATE_DETECTED */
    float prob;             /* meaningful only when state == ESP_MN_STATE_DETECTED */
} voice_result_t;

static esp_afe_sr_iface_t *s_afe_handle;
static esp_afe_sr_data_t *s_afe_data;
static const esp_mn_iface_t *s_multinet;
static model_iface_data_t *s_mn_data;
static esp_codec_dev_handle_t s_mic_dev;
static QueueHandle_t s_result_queue;

static void feed_task(void *arg)
{
    (void)arg;

    int chunksize = s_afe_handle->get_feed_chunksize(s_afe_data);
    int16_t *buf = heap_caps_malloc(chunksize * sizeof(int16_t) * AFE_FEED_CHANNELS,
                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGE(TAG, "No memory for mic feed buffer");
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        int ret = esp_codec_dev_read(s_mic_dev, buf, chunksize * MIC_RAW_CHANNELS * sizeof(int16_t));
        if (ret != 0) {
            ESP_LOGW(TAG, "mic read failed: %d", ret);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        /* Expand 2-channel raw capture into the 3-channel layout AFE
         * expects, working backward so writes (always at a higher index
         * for the same source sample) never clobber data not yet read. */
        for (int i = chunksize - 1; i >= 0; i--) {
            buf[i * 3 + 2] = 0;
            buf[i * 3 + 1] = buf[i * 2 + 1];
            buf[i * 3 + 0] = buf[i * 2 + 0];
        }

        s_afe_handle->feed(s_afe_data, buf);
    }
}

static void detect_task(void *arg)
{
    (void)arg;

    bool detecting = false;

    for (;;) {
        afe_fetch_result_t *res = s_afe_handle->fetch(s_afe_data);
        if (!res || res->ret_value == ESP_FAIL) {
            continue;
        }

        if (res->wakeup_state == WAKENET_DETECTED) {
            voice_result_t r = {.wake = true};
            xQueueSend(s_result_queue, &r, 0);
        } else if (res->wakeup_state == WAKENET_CHANNEL_VERIFIED) {
            detecting = true;
            s_afe_handle->disable_wakenet(s_afe_data);
        }

        if (!detecting) {
            continue;
        }

        esp_mn_state_t state = s_multinet->detect(s_mn_data, res->data);
        if (state == ESP_MN_STATE_DETECTING) {
            continue;
        }

        if (state == ESP_MN_STATE_TIMEOUT) {
            voice_result_t r = {.wake = false, .state = state, .command_id = -1};
            xQueueSend(s_result_queue, &r, 0);
            s_afe_handle->enable_wakenet(s_afe_data);
            detecting = false;
            continue;
        }

        if (state == ESP_MN_STATE_DETECTED) {
            esp_mn_results_t *mn_result = s_multinet->get_results(s_mn_data);
            voice_result_t r = {.wake = false, .state = state,
                                 .command_id = mn_result->command_id[0], .prob = mn_result->prob[0]};
            xQueueSend(s_result_queue, &r, 0);
            s_afe_handle->enable_wakenet(s_afe_data);
            detecting = false;
            continue;
        }
    }
}

static void handler_task(void *arg)
{
    (void)arg;

    for (;;) {
        voice_result_t r;
        if (xQueueReceive(s_result_queue, &r, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (r.wake) {
            ESP_LOGI(TAG, "wake word detected");
            lights_screen_set_voice_status("Listening...");
            continue;
        }

        if (r.state == ESP_MN_STATE_TIMEOUT) {
            ESP_LOGI(TAG, "command timeout");
            lights_screen_set_voice_status(NULL);
            continue;
        }

        if (r.state == ESP_MN_STATE_DETECTED) {
            ESP_LOGI(TAG, "command detected: %d (confidence %.3f)", r.command_id, r.prob);
            if (r.command_id == CMD_LIGHTS_ON) {
                lights_screen_set_voice_status("Heard: Lights On");
                lights_mqtt_set(true);
            } else if (r.command_id == CMD_LIGHTS_OFF) {
                lights_screen_set_voice_status("Heard: Lights Off");
                lights_mqtt_set(false);
            }
            vTaskDelay(pdMS_TO_TICKS(2000));
            lights_screen_set_voice_status(NULL);
        }
    }
}

void lights_voice_init(void)
{
    /* bsp_audio_codec_microphone_init() calls bsp_audio_init(NULL)
     * internally if the I2S peripheral isn't already up, which then
     * defaults to 22050 Hz (BSP_I2S_DUPLEX_MONO_CFG(22050) inside
     * esp-box-3_idf5.c) - a real, confirmed-live mismatch against the
     * 16000 Hz this whole pipeline assumes throughout (AFE/wakenet/
     * multinet are all fixed at 16kHz). That ~38% rate mismatch was very
     * likely the actual cause of a persistent "AFE_SR: ERROR!
     * afe_feed_aec_init_false, rb_out slow!!!" warning seen on real
     * hardware that neither a CPU clock increase (160->240MHz) nor moving
     * AFE's processing to the other core changed at all - consistent
     * with a fixed-rate data mismatch rather than a scheduling/compute
     * bottleneck, since compute-bound fixes would be expected to help at
     * least partially. Call bsp_audio_init() explicitly at 16000 Hz
     * ourselves first, so the mic init below can never fall through to
     * that mismatched default. */
    const i2s_std_config_t i2s_16khz_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT,
            .din = BSP_I2S_DSIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    if (bsp_audio_init(&i2s_16khz_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "I2S init at 16kHz failed - voice control disabled");
        return;
    }

    s_mic_dev = bsp_audio_codec_microphone_init();
    if (!s_mic_dev) {
        ESP_LOGE(TAG, "Microphone init failed - voice control disabled");
        return;
    }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = MIC_RAW_CHANNELS,
        .channel_mask = 0,
        .sample_rate = 16000,
        .mclk_multiple = 0,
    };
    if (esp_codec_dev_open(s_mic_dev, &fs) != 0) {
        ESP_LOGE(TAG, "Microphone open failed - voice control disabled");
        return;
    }
    /* ES7210 driver applies a default 30dB gain automatically on open, but
     * measured raw peak amplitude during active loud talking was still
     * only ~74-118 out of 32767 (~-49dBFS) - pushing to the driver's near
     * max (37.5dB, see get_db() in the es7210 driver source) to see if
     * that's a meaningful lever, before assuming AFE's own AGC stage
     * will compensate for a signal this quiet on its own. */
    esp_codec_dev_set_in_gain(s_mic_dev, 37.5f);

    srmodel_list_t *models = esp_srmodel_init("model");
    if (!models) {
        ESP_LOGE(TAG, "No voice models found in the \"model\" partition - voice control disabled");
        return;
    }

    s_afe_handle = (esp_afe_sr_iface_t *)&ESP_AFE_SR_HANDLE;
    afe_config_t afe_config = AFE_CONFIG_DEFAULT();
    afe_config.wakenet_model_name = esp_srmodel_filter(models, ESP_WN_PREFIX, NULL);
    /* AFE_CONFIG_DEFAULT() for esp32s3 already sets aec_init=true with
     * pcm_config 2 mic + 1 ref channel - matching this file's own 3-channel
     * feed layout (2 real mics + a zeroed 3rd/reference slot) exactly.
     * Explicitly disabling AEC here (matching Espressif's factory_demo,
     * which does the same) turned out to be the actual cause of the
     * continuous "AFE_SR: ERROR! afe_feed_aec_init_false, rb_out slow!!!"
     * warning and total wake-word silence on real hardware - that error
     * string names the exact disabled-AEC code path. Confirmed via an
     * isolated diagnostic build (Espressif's own unmodified
     * wake_word_detection/afe example, esp-sr 2.1.3, AEC enabled) that
     * detected "Hi, ESP" repeatedly and reliably on this same board -
     * leaving AEC at its default (true) here fixed it, even with zeros
     * fed into the unused reference channel (no echo to cancel since this
     * project never plays audio out). */
    /* AFE spins up its own internal worker task for the actual NS/VAD/
     * beamforming processing, defaulting to core 0 at priority 5
     * (AFE_CONFIG_DEFAULT()'s afe_perferred_core/afe_perferred_priority).
     * Left at that default, it showed up as a continuous "AFE_SR: ERROR!
     * afe_feed_aec_init_false, rb_out slow!!!" warning (the fetch side
     * falling behind the feed side) that persisted even after fixing the
     * CPU clock - the real contention is that core 0 also hosts ESP-IDF's
     * WiFi driver task at priority 23 (much higher, confirmed in the
     * serial log), kept busy by four simultaneous MQTT clients' traffic -
     * something the factory_demo reference this is adapted from almost
     * certainly doesn't have anywhere near as much of. Moving AFE's own
     * processing to core 1 (already home to this file's own detect_task,
     * which spends nearly all its time blocked waiting on fetch() rather
     * than competing for compute) gets it away from that contention. */
    afe_config.afe_perferred_core = 1;
    if (!afe_config.wakenet_model_name) {
        ESP_LOGE(TAG, "No wake word model found - voice control disabled");
        return;
    }
    s_afe_data = s_afe_handle->create_from_config(&afe_config);

    char *mn_name = esp_srmodel_filter(models, ESP_MN_PREFIX, ESP_MN_ENGLISH);
    if (!mn_name) {
        ESP_LOGE(TAG, "No English command model found - voice control disabled");
        return;
    }
    s_multinet = esp_mn_handle_from_name(mn_name);
    s_mn_data = s_multinet->create(mn_name, 5760);
    /* "Lights On" vs "Lights Off" are acoustically close (same opening
     * word, one-syllable endings) and got confused for each other in
     * real-world testing. First attempt at a fix: raising the detection
     * threshold to 0.7 (from whatever MultiNet's own default is) to
     * reject low-confidence matches - that was too aggressive, causing
     * every command (right or wrong) to time out instead, confirmed via
     * a live test where 4/4 attempts hit ESP_MN_STATE_TIMEOUT. Left at
     * the library default for now; command detected: %d (confidence
     * %.3f) is logged in handler_task below so real confidence numbers
     * for both correct and incorrect recognitions can inform a properly
     * calibrated threshold (or a phrase-wording change) next. */

    esp_mn_commands_clear();
    esp_mn_commands_add(CMD_LIGHTS_ON, "Lights On");
    esp_mn_commands_add(CMD_LIGHTS_OFF, "Lights Off");
    esp_mn_error_t *err = esp_mn_commands_update(s_multinet, s_mn_data);
    if (err) {
        for (int i = 0; i < err->num; i++) {
            ESP_LOGE(TAG, "command registration error: id=%d string=\"%s\"",
                     err->phrases[i]->command_id, err->phrases[i]->string);
        }
    }
    esp_mn_commands_print();

    s_result_queue = xQueueCreate(3, sizeof(voice_result_t));

    /* Explicit pdPASS checks - 2026-09-15's wake-word regression turned
     * out to be detect_task's own creation silently failing under
     * internal-RAM pressure (fixed by moving lights_voice_init() first in
     * main.c's app_main(), before anything else claims heap - see that
     * call site's comment for the full story). A failed creation here
     * would otherwise leave the matching task simply not existing, with
     * nothing to say so - keeping these checks permanently now that this
     * failure mode is a known, real possibility on this device. */
    BaseType_t feed_ok = xTaskCreatePinnedToCore(feed_task, "voice_feed", 4096, NULL, 5, NULL, 0);
    BaseType_t detect_ok = xTaskCreatePinnedToCore(detect_task, "voice_detect", 8192, NULL, 5, NULL, 1);
    if (feed_ok != pdPASS) {
        ESP_LOGE(TAG, "feed_task creation failed (ret=%d) - voice control disabled", (int)feed_ok);
    }
    if (detect_ok != pdPASS) {
        ESP_LOGE(TAG, "detect_task creation failed (ret=%d) - voice control disabled", (int)detect_ok);
    }
    xTaskCreate(handler_task, "voice_handler", 4096, NULL, tskIDLE_PRIORITY + 2, NULL);

    ESP_LOGI(TAG, "Voice control ready - say \"Hi, ESP\" then \"Lights On\"/\"Lights Off\"");
}
