#pragma once

#define WEATHER_STATE_TOPIC "homeops/weather/state"

/* Parses one message from WEATHER_STATE_TOPIC (the weather poll
 * workflow's retained state) and updates the Home screen. Called by
 * homeops_mqtt.c, which owns the shared MQTT client this topic is
 * dispatched from. */
void weather_mqtt_handle_message(const char *data, int len);
