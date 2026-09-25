#pragma once

#define SYSTEMS_STATE_TOPIC "homeops/systems/state"

/* Parses one message from SYSTEMS_STATE_TOPIC (the health check
 * workflow's retained state) and updates the Systems screen. Called by
 * homeops_mqtt.c, which owns the shared MQTT client this topic is
 * dispatched from. */
void systems_mqtt_handle_message(const char *data, int len);
