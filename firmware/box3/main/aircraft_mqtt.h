#pragma once

#define AIRCRAFT_STATE_TOPIC "homeops/aircraft/state"

/* Parses one message from AIRCRAFT_STATE_TOPIC (the aircraft poll
 * workflow's retained state) and updates the Home screen, FlightAware
 * usage, and aircraft alert as needed. Called by homeops_mqtt.c, which
 * owns the shared MQTT client this topic is dispatched from. */
void aircraft_mqtt_handle_message(const char *data, int len);
