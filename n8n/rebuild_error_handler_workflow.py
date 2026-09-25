import json
import os
import uuid

import urllib.request

# Shared "Error Handler" workflow - every PiLab workflow (Home Ops AND the
# streetwear business ones) points settings.errorWorkflow at this id.
#
# 2026-09-22: added per-workflow throttling. Before this it was a bare
# Error Trigger -> Telegram, so one broker outage (the Mosquitto auth
# lockdown crash loop, ~13:4x) fanned out into 93 Telegram messages in a
# few minutes - Aircraft Poll alone runs every 3s and failed 49 times.
# Now: one Telegram per incident per workflow - a ROLLING quiet window
# (see throttle_query); repeats are counted and reported on the next
# incident's alert.
#
# Throttle state is a Postgres row per workflow (homeops.alert_throttle),
# not n8n static data - dozens of error executions land concurrently, and
# the INSERT ... ON CONFLICT DO UPDATE row lock serializes them atomically.
#
# No If node on purpose (n8n boolean-If bug, see memory/PROJECT notes):
# the query returns ZERO rows when suppressing, and n8n simply doesn't run
# downstream nodes on an empty input - so Telegram only fires on a row.
# (Must NOT set alwaysOutputData on "Throttle" or that breaks.)

N8N_URL = "http://pilab.local:5678"
API_KEY = open(os.path.expanduser("~/home-ops-dashboard/n8n/.n8n_api_key")).read().strip()
POSTGRES_CRED = {"id": "YOUR_N8N_CREDENTIAL_ID", "name": "Postgres account"}
TELEGRAM_CRED = {"id": "YOUR_N8N_CREDENTIAL_ID", "name": "Telegram account"}
TELEGRAM_CHAT_ID = "YOUR_TELEGRAM_CHAT_ID"
WF = "YOUR_WORKFLOW_ID"
THROTTLE_WINDOW = "30 minutes"


def node_id():
    return str(uuid.uuid4())


# Rolling window = one message per incident (second pass, same day -
# explicit ask: "I only want to receive one". A fixed 30-min window still
# meant ~6 messages over a 3h sky outage, since Aircraft Poll fails every
# 3s for as long as sky is down). Every error refreshes last_error; a
# message goes out only if this workflow had been error-free for the whole
# THROTTLE_WINDOW before this error. A continuous outage of any length =
# exactly one Telegram; a new incident after 30 quiet minutes alerts again.
#
# All CASE branches read the OLD row values (Postgres UPDATE semantics), so
# they agree on whether this error opens a new incident. now() is fixed per
# transaction, so "last_sent = now()" is true exactly when this call sent.
throttle_query = f"""
WITH t AS (
  INSERT INTO homeops.alert_throttle (workflow_id, last_sent, last_error, suppressed, prev_suppressed)
  VALUES ('{{{{ String($('Error Trigger').first().json.workflow.id || 'unknown').replace(/[^A-Za-z0-9_-]/g, '') }}}}', now(), now(), 0, 0)
  ON CONFLICT (workflow_id) DO UPDATE SET
    prev_suppressed = CASE WHEN alert_throttle.last_error < now() - interval '{THROTTLE_WINDOW}'
                           THEN alert_throttle.suppressed ELSE alert_throttle.prev_suppressed END,
    suppressed      = CASE WHEN alert_throttle.last_error < now() - interval '{THROTTLE_WINDOW}'
                           THEN 0 ELSE alert_throttle.suppressed + 1 END,
    last_sent       = CASE WHEN alert_throttle.last_error < now() - interval '{THROTTLE_WINDOW}'
                           THEN now() ELSE alert_throttle.last_sent END,
    last_error      = now()
  RETURNING last_sent = now() AS send_now, prev_suppressed
)
SELECT prev_suppressed FROM t WHERE send_now;
""".strip()

# Restart-blip gate (2026-09-24): every lab-n8n restart can kill whichever
# workflow is mid-run - n8n stops its task runner before in-flight Code
# nodes finish - producing one "Task request timed out" error and a false
# Telegram (hit twice that day: Spotify Now Playing 18:13, System Health
# Check 21:4x). A genuinely broken runner fails on every run, so a lone
# timeout is dropped and only a second one from the same workflow within
# 15 min goes on to the Throttle. Every other error passes straight
# through. Zero-row output = stop (same no-If-node pattern as Throttle).
# Prefix match: n8n appends " [line N]" when a Code node itself throws.
# Values are query parameters, never pasted into the SQL text.
gate_query = """
WITH prev AS (
  SELECT last_seen FROM homeops.task_timeout_seen WHERE workflow_id = $1
), up AS (
  INSERT INTO homeops.task_timeout_seen (workflow_id, last_seen)
  SELECT $1, now() WHERE $2 LIKE 'Task request timed out%'
  ON CONFLICT (workflow_id) DO UPDATE SET last_seen = now()
)
SELECT 1 AS pass
WHERE $2 NOT LIKE 'Task request timed out%'
   OR EXISTS (SELECT 1 FROM prev WHERE last_seen > now() - interval '15 minutes');
""".strip()
gate_params = ("={{ [String($json.workflow.id || 'unknown'), "
               "String(($json.execution && $json.execution.error && $json.execution.error.message) || '')] }}")

telegram_text = (
    "=\U0001F6A8 n8n Error\n\n"
    "Workflow: {{ $('Error Trigger').first().json.workflow.name }}\n"
    "Node: {{ $('Error Trigger').first().json.execution.lastNodeExecuted }}\n\n"
    "Error:\n{{ $('Error Trigger').first().json.execution.error.message }}\n\n"
    "Execution:\n{{ $('Error Trigger').first().json.execution.url }}"
    "{{ $json.prev_suppressed > 0 ? '\\n\\n(Previous incident: ' + $json.prev_suppressed + ' repeat errors were muted)' : '' }}\n\n"
    "No more alerts from this workflow until it has been error-free for " + THROTTLE_WINDOW + "."
)

nodes = [
    {
        "id": node_id(), "name": "Error Trigger", "type": "n8n-nodes-base.errorTrigger",
        "typeVersion": 1, "position": [0, 0], "parameters": {},
    },
    {
        "id": node_id(), "name": "Restart Blip Gate", "type": "n8n-nodes-base.postgres",
        "typeVersion": 2.6, "position": [220, 0],
        "parameters": {"operation": "executeQuery", "query": gate_query,
                       "options": {"queryReplacement": gate_params}},
        "credentials": {"postgres": POSTGRES_CRED},
    },
    {
        "id": node_id(), "name": "Throttle", "type": "n8n-nodes-base.postgres",
        "typeVersion": 2.6, "position": [440, 0],
        "parameters": {"operation": "executeQuery", "query": throttle_query, "options": {}},
        "credentials": {"postgres": POSTGRES_CRED},
    },
    {
        "id": node_id(), "name": "Send a text message", "type": "n8n-nodes-base.telegram",
        "typeVersion": 1.2, "position": [660, 0],
        "webhookId": "6e373aeb-7b71-4423-9b81-252acf025548",
        "parameters": {"chatId": TELEGRAM_CHAT_ID, "text": telegram_text, "additionalFields": {}},
        "credentials": {"telegramApi": TELEGRAM_CRED},
    },
]

connections = {
    "Error Trigger": {"main": [[{"node": "Restart Blip Gate", "type": "main", "index": 0}]]},
    "Restart Blip Gate": {"main": [[{"node": "Throttle", "type": "main", "index": 0}]]},
    "Throttle": {"main": [[{"node": "Send a text message", "type": "main", "index": 0}]]},
}

workflow = {
    "name": "Error Handler",
    "nodes": nodes,
    "connections": connections,
    "settings": {"executionOrder": "v1", "binaryMode": "separate", "availableInMCP": False},
}

req = urllib.request.Request(
    f"{N8N_URL}/api/v1/workflows/{WF}",
    data=json.dumps(workflow).encode(),
    headers={"X-N8N-API-KEY": API_KEY, "Content-Type": "application/json"},
    method="PUT",
)

with urllib.request.urlopen(req) as resp:
    result = json.load(resp)
    print("Updated. Active:", result["active"])
