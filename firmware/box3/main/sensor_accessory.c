/*
 * Direct driver bring-up for the ESP32-S3-BOX-3 Sensor accessory.
 *
 * esp-box's own BSP only wires up aht20/at581x when the project directory
 * name literally contains the string "factory_demo" (see
 * esp-box/components/bsp/CMakeLists.txt) - everywhere else it silently
 * compiles a stub that always reports the accessory as absent. Rather than
 * rename this project to match that string match, this file re-implements
 * the same bring-up directly against the aht20/at581x components, using
 * the same I2C bus, pins, and addresses the official factory_demo uses
 * (esp-box/components/bsp/src/boards/esp32_bsp_sensor.c).
 */
#include "sensor_accessory.h"

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#include "aht20.h"
#include "at581x.h"

static const char *TAG = "sensor_accessory";

/* The Sensor accessory sits on the I2C port BSP doesn't use for the main
 * display/touch/audio bus, wired to GPIO40/41 specifically for the bottom
 * expansion connector. */
#define SENSOR_I2C_NUM       0
#define SENSOR_I2C_SDA       GPIO_NUM_41
#define SENSOR_I2C_SCL       GPIO_NUM_40
#define SENSOR_I2C_CLK_HZ    400000

/* AT581X radar digital output, per esp-box's factory_demo BSP_RADAR_OUT_IO
 * (esp32_bsp_sensor.c) - same board, same pin. */
#define SENSOR_RADAR_OUT_IO  GPIO_NUM_21

static bool s_present = false;
static aht20_dev_handle_t s_aht20 = NULL;
static at581x_dev_handle_t s_at581x = NULL;

static bool i2c_probe(uint8_t addr)
{
    bool ok = false;
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    /* addr is already the pre-shifted 8-bit form (see AT581X_ADDRRES_0 / AHT20_ADDRRES_0). */
    i2c_master_write_byte(cmd, addr | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd);
    if (i2c_master_cmd_begin(SENSOR_I2C_NUM, cmd, pdMS_TO_TICKS(2000)) == ESP_OK) {
        ok = true;
    }
    i2c_cmd_link_delete(cmd);
    return ok;
}

esp_err_t sensor_accessory_init(void)
{
    const i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = SENSOR_I2C_SDA,
        .sda_pullup_en = GPIO_PULLUP_DISABLE,
        .scl_io_num = SENSOR_I2C_SCL,
        .scl_pullup_en = GPIO_PULLUP_DISABLE,
        .master.clk_speed = SENSOR_I2C_CLK_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_param_config(SENSOR_I2C_NUM, &conf), TAG, "i2c config failed");
    ESP_RETURN_ON_ERROR(i2c_driver_install(SENSOR_I2C_NUM, conf.mode, 0, 0, 0), TAG, "i2c install failed");

    if (!i2c_probe(AT581X_ADDRRES_0)) {
        ESP_LOGW(TAG, "Sensor accessory not detected");
        i2c_driver_delete(SENSOR_I2C_NUM);
        s_present = false;
        return ESP_OK;
    }

    aht20_i2c_config_t aht20_conf = {
        .i2c_port = SENSOR_I2C_NUM,
        .i2c_addr = AHT20_ADDRRES_0,
    };
    esp_err_t ret = aht20_new_sensor(&aht20_conf, &s_aht20);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "AHT20 init failed: %s", esp_err_to_name(ret));
        s_present = false;
        return ret;
    }

    at581x_default_cfg_t at581x_def_cfg = ATH581X_INITIALIZATION_CONFIG();
    /* Default delta_cfg (200) reported "screen never sleeps" - live
     * testing (raw radar level logged every 3s) showed the output stuck
     * continuously high for 20+ minutes with the user ~12m away in
     * another room, i.e. false-triggering on noise/reflections, not real
     * presence at that range. delta_cfg is 0-1023, "the larger the value,
     * the shorter the [triggering] distance" per at581x.h. A first attempt
     * at 600 had no measurable effect (confirmed via the same live
     * logging, not assumed) - pushed to near max, and also reduced
     * gain_cfg's actual RF gain (a separate stage - "the larger the
     * enum value, the smaller the gain" per at581x_reg.h, GAIN_3 default
     * -> GAIN_9, top of the datasheet's own recommended GAIN_3~GAIN_9
     * range) at the same time, since delta_cfg alone clearly wasn't the
     * whole story. */
    at581x_def_cfg.delta_cfg = 1023; /* true max (0-1023 range) */
    /* GAIN_9 (top of the datasheet's recommended GAIN_3~GAIN_9 range) was
     * a real, measurable improvement over the GAIN_3 default - level now
     * genuinely toggles 0/1 instead of being permanently stuck high - but
     * still triggered "motion" roughly half the time with nobody in the
     * room, nowhere near the ~40 consecutive 3s-interval zero-readings
     * IDLE_SLEEP_TIMEOUT_SEC needs to actually reach sleep. Pushed past
     * the recommended range to GAIN_C (max) to see if going further still
     * helps - if this doesn't get it the rest of the way, the remaining
     * false triggers are more likely a real, independent object moving
     * near the sensor (fan, AC vent, curtain, pet) than something more
     * config tuning alone can fix. */
    /* 2026-09-12: GAIN_C (max) turned out to overshoot - reported "sitting
     * right next to it, typing, and it still sleeps" (arm's-length
     * keyboard motion not registering). GAIN_9 was already confirmed too
     * sensitive (still false-triggered at 12m, see above) but GAIN_A/
     * GAIN_B, one and two steps more sensitive than C, were never actually
     * tried - went with GAIN_A as the first real data point in that
     * untested gap.
     * 2026-09-13: GAIN_A confirmed too sensitive too, via an actual
     * "nobody present" baseline (not just active-use data this time) -
     * live serial log showed `idle_sec` reset by a false `confirmed=1`
     * every 20-40s, never once reaching the 120s sleep threshold. That
     * puts GAIN_B - the one setting directly between confirmed-too-
     * sensitive (A) and confirmed-too-insensitive (C), never tried until
     * now - as the obvious next data point rather than another guess.
     * Live-verify with the raw-level logging below before considering
     * this settled. */
    at581x_def_cfg.gain_cfg = AT581X_STAGE_GAIN_B;
    at581x_i2c_config_t at581x_conf = {
        .i2c_port = SENSOR_I2C_NUM,
        .i2c_addr = AT581X_ADDRRES_0,
        .def_conf = &at581x_def_cfg,
    };
    ret = at581x_new_sensor(&at581x_conf, &s_at581x);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "AT581X radar init failed: %s", esp_err_to_name(ret));
        s_present = false;
        return ret;
    }

    /* A pull-DOWN was tried here as a diagnostic (still read high most of
     * the time even against an active pull-down, which rules out a
     * floating/disconnected pin - the sensor really is driving this line).
     * Back to the original pull-up, matching esp-box's own factory_demo
     * reference for this exact pin. */
    const gpio_config_t radar_io_conf = {
        .pin_bit_mask = (1ULL << SENSOR_RADAR_OUT_IO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&radar_io_conf), TAG, "radar gpio config failed");

    s_present = true;
    ESP_LOGI(TAG, "Sensor accessory detected and initialized");
    return ESP_OK;
}

bool sensor_accessory_present(void)
{
    return s_present;
}

bool sensor_accessory_motion_level(void)
{
    if (!s_present) {
        return false;
    }
    return gpio_get_level(SENSOR_RADAR_OUT_IO) != 0;
}

bool sensor_accessory_get_humiture(float *temp_c, float *humidity)
{
    if (!s_present || s_aht20 == NULL) {
        return false;
    }

    uint32_t temp_raw = 0, rh_raw = 0;
    if (aht20_read_temperature_humidity(s_aht20, &temp_raw, temp_c, &rh_raw, humidity) != ESP_OK) {
        return false;
    }

    /* Guard against corrupted I2C reads: an all-zero buffer still passes
     * this driver's CRC check (CRC of all-zero data is 0), which would
     * otherwise show up as an exact 0% humidity / -50C reading - never a
     * real indoor value. */
    if (*humidity <= 0.0f || *humidity > 100.0f || *temp_c < -10.0f || *temp_c > 60.0f) {
        return false;
    }

    return true;
}
