#pragma once

#define CODEX_USAGE_STATE_TOPIC "homeops/codex_usage/state"

/* Parses one message from CODEX_USAGE_STATE_TOPIC and updates the Codex
 * Usage screen. Same architecture as claude_usage_mqtt.h (see that file
 * for the full pattern this one deliberately mirrors) - published by a
 * small Python poller on `sky` (~/codex_usage_poll.py, systemd --user
 * timer, every ~5 min), not this firmware, not an n8n workflow.
 *
 * Real data source: OpenAI's own ChatGPT-Codex backend, GET
 * https://chatgpt.com/backend-api/wham/usage, authenticated with the
 * same ChatGPT OAuth token Codex CLI itself uses to log in on the Mac
 * (~/.codex/auth.json) - confirmed live 2026-09-22 to return the exact
 * numbers the ChatGPT app's own "Usage and limits" screen shows
 * (rate_limit.primary_window/secondary_window.used_percent - "5-hour
 * limit"/"Weekly limit" respectively). Undocumented but real, same
 * trust tier as the Claude screen's rate-limit-header technique.
 *
 * Real difference from the Claude screen worth knowing: OpenAI's own
 * docs (developers.openai.com/codex/auth/ci-cd-auth) explicitly say the
 * supported way to keep a Codex session fresh is "run Codex and persist
 * the updated auth.json," not calling its OAuth refresh endpoint
 * yourself - unlike Claude Code's token, this one is NOT self-refreshed
 * by this poller. It's a one-time manual copy from the Mac (same
 * starting point Claude's own token had before it grew a refresh path)
 * and will need re-copying once it goes stale - deliberately not
 * automated further; sky is memory-constrained (Pi Zero 2 W, ~170MB
 * free) and installing the full Node-based Codex CLI just to keep a
 * token warm was judged not worth the footprint for this one screen. */
void codex_usage_mqtt_handle_message(const char *data, int len);
