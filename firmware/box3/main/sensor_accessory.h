#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* Probes and (if present) initializes the ESP32-S3-BOX-3 Sensor accessory
 * (AHT20 temp/humidity, on its own I2C bus). Safe to call even if the
 * accessory isn't physically attached - just leaves it marked absent. */
esp_err_t sensor_accessory_init(void);

bool sensor_accessory_present(void);

/* Returns false if the accessory isn't attached or the read failed. */
bool sensor_accessory_get_humiture(float *temp_c, float *humidity);

/* Raw current level of the AT581X radar's digital output pin (GPIO21):
 * true while the sensor is asserting a detection. The sensor re-triggers
 * (pulses high again) roughly every ~2s of continued presence rather than
 * holding the line high continuously - callers watching for "is anyone
 * still here" should treat a low->high transition as "still present", not
 * just the instantaneous level. Always false if the accessory isn't
 * attached. */
bool sensor_accessory_motion_level(void);
