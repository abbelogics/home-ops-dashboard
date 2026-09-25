#pragma once

/* Single shared MQTT client for every state topic this device consumes
 * (aircraft/weather/systems/lights) - consolidated 2026-09-15 from 4
 * separate esp_mqtt_client instances. Each separate client cost ~9.6KB of
 * this device's scarce internal RAM (mostly its own 6144-byte task stack,
 * CONFIG_MQTT_TASK_STACK_SIZE), almost entirely duplicate connection/task
 * overhead. Root-caused via per-init-step heap checkpoints in main.c:
 * voice (~82KB) and WiFi (~49KB) alone consume most of the ~183KB
 * internal RAM available at boot, so 4 separate ~9.6KB MQTT clients on
 * top of that left too little for the last two to even start
 * (esp_mqtt_client_start() failing outright on systems_mqtt/lights_mqtt).
 * One client, multiple topic subscriptions, dispatched by topic name in
 * homeops_mqtt.c - the same pattern this codebase already used for
 * weather+FX before FX was removed. Non-blocking - safe to call before
 * WiFi has connected; the client retries on its own. */
void homeops_mqtt_init(void);
