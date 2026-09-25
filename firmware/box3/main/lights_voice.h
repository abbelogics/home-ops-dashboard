#pragma once

/* On-device offline voice control for the office lamp (espressif/esp-sr) -
 * wake word "Hi, ESP", then either "Lights On" or "Lights Off". English
 * only - ESP-SR's MultiNet command recognizer has no Spanish support as of
 * this writing (checked directly against Espressif's docs before building
 * this), only English and Chinese.
 *
 * Requires mic hardware (built into the BOX-3) and the "model" SPIFFS
 * partition (see ../partitions.csv) to be flashed with the wake-word/
 * command models - `idf.py flash` handles that automatically as part of
 * the normal SPIFFS image, same as the airline logos in `storage`.
 *
 * Call once from app_main(), after bsp_i2s init concerns are otherwise
 * handled - this owns the mic codec device itself internally. */
void lights_voice_init(void);
