#pragma once

#define CLAUDE_USAGE_STATE_TOPIC "homeops/claude_usage/state"

/* Parses one message from CLAUDE_USAGE_STATE_TOPIC and updates the
 * Claude Usage screen. Published by a small Python poller running on
 * `sky` (~/claude_usage_poll.py, systemd --user timer, every ~2 min) -
 * NOT this firmware, and NOT an n8n workflow, unlike every other topic
 * this device consumes. Real data source: a throwaway 1-token request to
 * the actual Anthropic Messages API (api.anthropic.com/v1/messages),
 * reading the real rate-limit response headers
 * (anthropic-ratelimit-unified-5h/7d-utilization) it returns when
 * authenticated with the same Claude Code OAuth token already used to
 * log in on the Mac - not a scrape of claude.ai's private account page
 * (the original 2026-09-14 Claude Usage screen's blocker, and the reason
 * it got removed same day it was added). Same technique as the
 * open-source Clawdmeter project (github.com/HermannBjorgvin/Clawdmeter).
 * The token is a manual, one-time copy placed on `sky` directly from the
 * Mac's Keychain - it is never refreshed automatically (Claude Code
 * itself owns that lifecycle) and will need re-copying whenever it
 * expires. Called by homeops_mqtt.c, which owns the shared MQTT client
 * this topic is dispatched from. */
void claude_usage_mqtt_handle_message(const char *data, int len);
