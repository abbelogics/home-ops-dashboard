#pragma once

#define NOTIFICATION_STATE_TOPIC "homeops/notification/state"

/* Generic notification bus for the Home screen's bottom strip
 * (home_screen_set_notification()/home_screen_clear_notification(), see
 * screens.h) - any n8n workflow can publish here rather than each source
 * needing its own dedicated topic/handler. Expected JSON:
 *   {"source": "systems", "message": "sky is offline", "severity": "red",
 *    "ttl_min": 0}
 * severity is "green"/"orange"/"red"; an empty or missing "message"
 * clears whatever that source last set instead of showing anything (see
 * home_screen_clear_notification()'s source-matching comment for why a
 * clear from one source can't stomp on another's active notification).
 * Called by homeops_mqtt.c, which owns the shared MQTT client this topic
 * is dispatched from. */
void notification_mqtt_handle_message(const char *data, int len);

/* F1 "session live" badge (2026-09-24) - retained, published by n8n's
 * Home Ops - F1 Sessions workflow on session start/end:
 *   {"code": "Q", "gp": "Azerbaijan GP", "ends_at": 1790340000}
 * (empty code clears)
 * See home_screen_set_f1_session(). */
#define F1_SESSION_TOPIC "homeops/f1/session"
void f1_session_mqtt_handle_message(const char *data, int len);
