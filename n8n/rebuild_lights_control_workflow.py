import json
import os
import uuid

import urllib.request

N8N_URL = "http://pilab.local:5678"
API_KEY = open(os.path.expanduser("~/home-ops-dashboard/n8n/.n8n_api_key")).read().strip()
MQTT_CRED = {"id": "YOUR_N8N_CREDENTIAL_ID", "name": "Mosquitto (PiLab)"}
ERROR_WORKFLOW_ID = "YOUR_WORKFLOW_ID"

HUE_BRIDGE_IP = "192.168.x.x"
# Read from a local dotfile (not committed/hardcoded, same pattern as
# .n8n_api_key) so the raw key doesn't sit in this script's source - it
# does end up embedded in the node URLs stored in n8n's own workflow JSON
# though, since Hue's local API has no header-based auth to use instead
# (the "username" is purely a URL path segment). Accepted tradeoff, no
# cleaner option given Hue's API shape.
HUE_KEY = open(os.path.expanduser("~/home-ops-dashboard/n8n/.hue_api_key")).read().strip()
HUE_LIGHT_ID = "2"  # "Luz de la oficina" - confirmed via GET /lights, id stable unless the light is re-added

HUE_LIGHT_URL = f"https://{HUE_BRIDGE_IP}/api/{HUE_KEY}/lights/{HUE_LIGHT_ID}"


def node_id():
    return str(uuid.uuid4())


# Bridge uses a self-signed cert on the local API - both HTTP nodes below
# need this to avoid a TLS verification failure.
HTTPS_OPTIONS = {"timeout": 4000, "allowUnauthorizedCerts": True}

format_state_js = """
const light = $input.first().json;
const state = light.state || {};
return [{ json: { on: !!state.on, bri: typeof state.bri === 'number' ? state.bri : 254 } }];
""".strip()

# Hue's own brightness range is 1-254 (not 0-100/0-255) - passed through
# as-is end to end (firmware slider, MQTT state messages, this webhook)
# rather than converting anywhere, so there's only one range to reason
# about.
parse_command_js = """
const body = $input.first().json.body || {};
const out = {};
if (body.on !== undefined) {
    out.on = !!body.on;
}
if (body.bri !== undefined) {
    let bri = parseInt(body.bri, 10);
    if (isNaN(bri)) bri = 254;
    out.bri = Math.max(1, Math.min(254, bri));
}
return [{ json: out }];
""".strip()

nodes = [
    # --- Poll: light state -> MQTT (every 30s, mirrors the health-check
    # workflow's polling pattern) ---
    {
        "id": node_id(), "name": "Every 30s", "type": "n8n-nodes-base.scheduleTrigger",
        "typeVersion": 1.3, "position": [0, 0],
        "parameters": {"rule": {"interval": [{"field": "seconds", "secondsInterval": 30}]}},
    },
    {
        "id": node_id(), "name": "Get Light State", "type": "n8n-nodes-base.httpRequest",
        "typeVersion": 4.5, "position": [220, 0], "onError": "continueRegularOutput",
        "parameters": {"url": HUE_LIGHT_URL, "options": HTTPS_OPTIONS},
    },
    {
        "id": node_id(), "name": "Format State", "type": "n8n-nodes-base.code",
        "typeVersion": 2, "position": [440, 0],
        "parameters": {"language": "javaScript", "jsCode": format_state_js},
    },
    {
        "id": node_id(), "name": "Publish Lights State", "type": "n8n-nodes-base.mqtt",
        "typeVersion": 1, "position": [660, 0],
        "parameters": {"topic": "homeops/lights/state", "sendInputData": True, "options": {"retain": True}},
        "credentials": {"mqtt": MQTT_CRED},
    },
    # --- Control: BOX-3 button -> HTTP POST -> Hue, then re-poll
    # immediately for fast feedback instead of waiting up to 30s for the
    # next cycle. A webhook, not an MQTT Trigger - n8n's MQTT Trigger node
    # failed to activate on this instance (rolled back with only
    # "Connection closed" logged, no further detail found after checking
    # both n8n's and mosquitto's logs) - webhooks are n8n's most
    # standard/battle-tested trigger type, so this sidesteps whatever that
    # issue was rather than chasing it further. Means the firmware speaks
    # plain HTTP for the control path instead of reusing its existing MQTT
    # connection, but that's well-supported (esp_http_client) and n8n runs
    # on plain HTTP already (no TLS complexity like the Hue bridge call).
    {
        "id": node_id(), "name": "On Set Command", "type": "n8n-nodes-base.webhook",
        "typeVersion": 2, "position": [0, 220],
        "parameters": {"path": "lights-set", "httpMethod": "POST", "options": {}},
    },
    {
        "id": node_id(), "name": "Parse Command", "type": "n8n-nodes-base.code",
        "typeVersion": 2, "position": [220, 220],
        "parameters": {
            "language": "javaScript",
            "jsCode": parse_command_js,
        },
    },
    {
        "id": node_id(), "name": "Set Light State", "type": "n8n-nodes-base.httpRequest",
        "typeVersion": 4.5, "position": [440, 220], "onError": "continueRegularOutput",
        "parameters": {
            "url": f"{HUE_LIGHT_URL}/state",
            "method": "PUT",
            "sendBody": True,
            "specifyBody": "json",
            # Forwards whatever fields Parse Command decided to include
            # ("on" and/or "bri") - a brightness-only slider drag doesn't
            # carry "on", and (unlike the old { on: $json.on } shape) no
            # longer forces the light off as a side effect of that.
            "jsonBody": "={{ JSON.stringify($json) }}",
            "options": HTTPS_OPTIONS,
        },
    },
    {
        "id": node_id(), "name": "Get Light State (post-set)", "type": "n8n-nodes-base.httpRequest",
        "typeVersion": 4.5, "position": [660, 220], "onError": "continueRegularOutput",
        "parameters": {"url": HUE_LIGHT_URL, "options": HTTPS_OPTIONS},
    },
    {
        "id": node_id(), "name": "Format State (post-set)", "type": "n8n-nodes-base.code",
        "typeVersion": 2, "position": [880, 220],
        "parameters": {"language": "javaScript", "jsCode": format_state_js},
    },
    {
        "id": node_id(), "name": "Publish Lights State (post-set)", "type": "n8n-nodes-base.mqtt",
        "typeVersion": 1, "position": [1100, 220],
        "parameters": {"topic": "homeops/lights/state", "sendInputData": True, "options": {"retain": True}},
        "credentials": {"mqtt": MQTT_CRED},
    },
]

connections = {
    "Every 30s": {"main": [[{"node": "Get Light State", "type": "main", "index": 0}]]},
    "Get Light State": {"main": [[{"node": "Format State", "type": "main", "index": 0}]]},
    "Format State": {"main": [[{"node": "Publish Lights State", "type": "main", "index": 0}]]},
    "On Set Command": {"main": [[{"node": "Parse Command", "type": "main", "index": 0}]]},
    "Parse Command": {"main": [[{"node": "Set Light State", "type": "main", "index": 0}]]},
    "Set Light State": {"main": [[{"node": "Get Light State (post-set)", "type": "main", "index": 0}]]},
    "Get Light State (post-set)": {"main": [[{"node": "Format State (post-set)", "type": "main", "index": 0}]]},
    "Format State (post-set)": {"main": [[{"node": "Publish Lights State (post-set)", "type": "main", "index": 0}]]},
}

workflow = {
    "name": "Home Ops - Lights Control",
    "nodes": nodes,
    "connections": connections,
    "settings": {
        "executionOrder": "v1",
        "errorWorkflow": ERROR_WORKFLOW_ID,
        "callerPolicy": "workflowsFromSameOwner",
    },
}

ID_FILE = os.path.expanduser("~/home-ops-dashboard/n8n/lights_control_workflow_id.txt")

if os.path.exists(ID_FILE):
    WF = open(ID_FILE).read().strip()
    req = urllib.request.Request(
        f"{N8N_URL}/api/v1/workflows/{WF}",
        data=json.dumps(workflow).encode(),
        headers={"X-N8N-API-KEY": API_KEY, "Content-Type": "application/json"},
        method="PUT",
    )
    with urllib.request.urlopen(req) as resp:
        result = json.load(resp)
        print("Updated. Active:", result["active"])
else:
    req = urllib.request.Request(
        f"{N8N_URL}/api/v1/workflows",
        data=json.dumps(workflow).encode(),
        headers={"X-N8N-API-KEY": API_KEY, "Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(req) as resp:
        result = json.load(resp)
        print("Created workflow id:", result["id"])
        with open(ID_FILE, "w") as f:
            f.write(result["id"])
