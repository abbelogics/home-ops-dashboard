#pragma once

#include <stdbool.h>

#define SPOTIFY_STATE_TOPIC "homeops/spotify/state"

/* No spotify_mqtt_init() - control calls go through webhook_post.c's
 * shared worker task (see main.c's webhook_post_init() call), not a
 * dedicated one of this module's own. An earlier version had its own
 * task here, mirroring lights_mqtt.c's now-superseded pattern - real
 * task-watchdog crash at boot, caught live 2026-09-16 (see
 * webhook_post.h for the internal-SRAM-exhaustion reasoning). */

/* Parses one message from SPOTIFY_STATE_TOPIC ("Home Ops - Spotify Now
 * Playing"'s retained state) and updates the Spotify screen. Called by
 * homeops_mqtt.c, which owns the shared MQTT client this topic is
 * dispatched from. */
void spotify_mqtt_handle_message(const char *data, int len);

/* Fire-and-forget HTTP POST to the "Home Ops - Spotify Control" n8n
 * workflow's webhook (see ../n8n/build_spotify_control_workflow.py),
 * requesting play/pause/next/previous. Plain http://pilab.local:5678
 * (not the HTTPS domain n8n's OAuth callback now needs) - same as
 * lights_mqtt.c's own webhook call, no TLS overhead needed for a
 * same-LAN request the ESP32 itself is making. Actual confirmation comes
 * back separately over MQTT once the next 15s poll picks up the change -
 * this call doesn't wait for or guarantee that. `action` must be one of
 * "play", "pause", "next", "previous" (matches the control workflow's
 * own Build Spotify Action switch). */
void spotify_mqtt_control(const char *action);

/* Same as spotify_mqtt_control(), but for volume - 0-100, clamped. */
void spotify_mqtt_set_volume(int volume_percent);
