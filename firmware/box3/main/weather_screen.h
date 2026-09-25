#pragma once

#include "cJSON.h"

/* Weather page (see weather_screen.c). Called from the MQTT client's task
 * with the parsed homeops/weather/state payload; takes the display lock. */
void weather_screen_update(const cJSON *root);
