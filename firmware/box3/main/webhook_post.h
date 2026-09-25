#pragma once

/* Shared background worker for fire-and-forget JSON POSTs to n8n
 * webhooks (lights control, Spotify control, and any future one) - one
 * persistent task/queue for all of them, not one per caller. A second
 * caller-owned dedicated task (Spotify's own, mirroring lights_mqtt.c's
 * pre-existing one) cost another ~4KB of internal SRAM stack on a device
 * that was already down to ~6KB free - real task-watchdog crash caught
 * live 2026-09-16, right at boot, a few seconds after the LVGL task
 * started. Same lesson this project already learned once with 4 separate
 * MQTT clients (see PROJECT.md), just one caller too many to notice
 * before it broke this time. */
void webhook_post_init(void);

/* Enqueues a POST of `json_body` (a complete, already-serialized JSON
 * string) to `url`, Content-Type: application/json. Non-blocking - the
 * actual HTTP round-trip happens on the shared worker task. Both
 * strings are copied into the queue item, so the caller's own buffers
 * can be reused/freed immediately after this call returns. */
void webhook_post_enqueue(const char *url, const char *json_body);
