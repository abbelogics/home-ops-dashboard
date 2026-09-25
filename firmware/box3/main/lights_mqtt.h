#pragma once

#include <stdbool.h>

#define LIGHTS_STATE_TOPIC "homeops/lights/state"

/* No lights_mqtt_init() - control calls go through webhook_post.c's
 * shared worker task (see main.c's webhook_post_init() call), not a
 * dedicated one of this module's own. This module's own separate task
 * (what this function used to be) was consolidated away once a second
 * caller - Spotify - adding its own copy of the same pattern caused a
 * real task-watchdog crash at boot, caught live 2026-09-16 (see
 * webhook_post.h). Doesn't own an MQTT client either (2026-09-15:
 * consolidated into the shared client in homeops_mqtt.c, see
 * lights_mqtt_handle_message() below).
 *
 * Parses one message from LIGHTS_STATE_TOPIC (the lights control
 * workflow's retained state) and updates the Lights screen's button/
 * brightness slider. Called by homeops_mqtt.c, which owns the shared
 * MQTT client this topic is dispatched from. */
void lights_mqtt_handle_message(const char *data, int len);

/* Fire-and-forget HTTP POST to the "Home Ops - Lights Control" n8n
 * workflow's webhook (see ../n8n/rebuild_lights_control_workflow.py),
 * requesting the office lamp be set to `on`. Actual confirmation comes
 * back separately over MQTT once the workflow re-polls the bridge - this
 * call doesn't wait for or guarantee that. Non-blocking - just enqueues
 * onto webhook_post.c's shared worker task, so callers (the LVGL
 * button's event callback, or the voice command handler) never pay for
 * a fresh task-stack allocation at the moment of the call. */
void lights_mqtt_set(bool on);

/* Same as lights_mqtt_set(), but for brightness - `bri` is Hue's own
 * native range (1-254, not a percentage), passed straight through to the
 * bridge unconverted. Doesn't touch on/off state; if the light is
 * currently off, Hue just remembers this level for the next time it's
 * turned on, same as adjusting brightness in the Hue app itself. */
void lights_mqtt_set_brightness(int bri);
