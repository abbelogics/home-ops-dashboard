#pragma once

#define FX_STATE_TOPIC "homeops/fx/state"

/* FX screen feed (restored 2026-09-24) - retained, published by n8n's
 * "Home Ops - FX Poll" (../n8n/rebuild_fx_poll_workflow.py) only when a
 * rate changes. 1 USD against each currency, from two official daily
 * reference sources: COP = TRM (datos.gov.co), EUR/GBP = ECB (Frankfurter).
 *   {"cop": {"rate": 3264.39, "prev": 3208.66, "date": "2026-09-24"},
 *    "eur": {...}, "gbp": {...}}
 * "prev" is the previous published rate (may be null). The pre-2026-09-24
 * flat format ({"usd_cop": ...}) is ignored. Dispatched from the shared
 * homeops_mqtt.c client - no client of its own. */
void fx_mqtt_handle_message(const char *data, int len);
