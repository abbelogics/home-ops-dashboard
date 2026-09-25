import json
import os
import uuid

import urllib.request

N8N_URL = "http://pilab.local:5678"
API_KEY = open(os.path.expanduser("~/home-ops-dashboard/n8n/.n8n_api_key")).read().strip()
POSTGRES_CRED = {"id": "YOUR_N8N_CREDENTIAL_ID", "name": "Postgres account"}
MQTT_CRED = {"id": "YOUR_N8N_CREDENTIAL_ID", "name": "Mosquitto (PiLab)"}
ERROR_WORKFLOW_ID = "YOUR_WORKFLOW_ID"  # existing shared Error Handler

# Dedicated keypair (n8n/.ssh_n8n/n8n_health_check, not tracked in git),
# added 2026-09-14 for device temperature readouts on the Systems screen -
# explicit request. Its public half was appended to ~/.ssh/authorized_keys
# on each box below directly (not via these credentials - n8n only needs
# the private half). PiLab and Sky both took it with no fuss (plain
# port-22 SSH, already reachable). Abbe's SSH listens on 2222, not 22 (a
# deliberate security choice on that box, per the user) - same key, same
# user, just a different port/credential. NAS still doesn't have a
# credential here - it needs its TrueNAS reporting API instead of SSH,
# pending an API key as of this comment.
SSH_CRED_PILAB = {"id": "YOUR_N8N_CREDENTIAL_ID", "name": "SSH (PiLab)"}
SSH_CRED_SKY = {"id": "YOUR_N8N_CREDENTIAL_ID", "name": "SSH (Sky)"}
SSH_CRED_ABBE = {"id": "YOUR_N8N_CREDENTIAL_ID", "name": "SSH (Abbe)"}

# NAS has no SSH access set up - its CPU temp comes from TrueNAS SCALE's
# own reporting API instead (POST /api/v2.0/reporting/get_data, graph
# "cputemp"), authenticated with an API key from Settings > API Keys in
# its web UI, added 2026-09-14. Confirmed live via curl before wiring
# this up: the response's aggregations.mean.cpu is a sensible single
# board-level average (~60C at test time) across all 12 logical cores -
# same idea as vcgencmd's single reading on the Pis, just averaged
# instead of a single sensor.
TRUENAS_CRED = {"id": "YOUR_N8N_CREDENTIAL_ID", "name": "TrueNAS API"}

# Static IPs - these boxes don't move. Sky's health check itself still hits
# the "sky" hostname (works fine, resolved separately from PiLab), this is
# just what gets shown/published alongside the status.
IPS = {
    "sky": "sky.local",
    "abbe": "vpn.local",
    "nas": "nas.local",
    "pilab": "pilab.local",
}


def node_id():
    return str(uuid.uuid4())


def http_check_node(name, url, x):
    return {
        "id": node_id(),
        "name": name,
        "type": "n8n-nodes-base.httpRequest",
        "typeVersion": 4.5,
        "position": [x, 0],
        "onError": "continueRegularOutput",
        "parameters": {"url": url, "options": {"timeout": 3000}},
    }


shared_helpers_js = """
function failed(nodeName) {
  const item = $(nodeName).first();
  return !item || !!item.json.error;
}

// vcgencmd measure_temp prints e.g. "temp=52.1'C" on stdout - the SSH
// node's execute-command output. Returns null (not 0) on any failure -
// keeps the field out of the payload entirely rather than showing a
// fake reading, same "has_temp" gate the firmware itself uses.
function parseTemp(nodeName) {
  if (failed(nodeName)) return null;
  const out = $(nodeName).first().json.stdout || '';
  const m = out.match(/temp=([\\d.]+)/);
  return m ? parseFloat(m[1]) : null;
}

// TrueNAS's reporting API returns a JSON array (one entry per requested
// graph) rather than SSH's plain-text stdout - n8n's HTTP Request node
// splits a top-level array response into one item per element, so
// "first()" here is the "cputemp" graph's own object, not the array.
// aggregations.mean.cpu is the all-core average over the requested
// window (see "Check NAS Temp"'s body) - a single representative number,
// same idea as the Pis' single vcgencmd reading.
function parseNasTemp() {
  if (failed('Check NAS Temp')) return null;
  const mean = $('Check NAS Temp').first().json?.aggregations?.mean?.cpu;
  return typeof mean === 'number' ? mean : null;
}
""".strip()

code_js = f"""
{shared_helpers_js}

const IPS = {json.dumps(IPS)};

const skyFailed = failed('Check Sky');
let aircraftCount = null;
if (!skyFailed) {{
  const sky = $('Check Sky').first().json;
  aircraftCount = Array.isArray(sky.aircraft) ? sky.aircraft.length : null;
}}

const pilabTemp = parseTemp('Temp PiLab');
const skyTemp = parseTemp('Temp Sky');
const abbeTemp = parseTemp('Temp Abbe');
const nasTemp = parseNasTemp();

return [
  {{ json: {{ key: 'sky_status', value: {{ online: !skyFailed, aircraft_count: aircraftCount, ip: IPS.sky, temp_c: skyTemp }} }} }},
  {{ json: {{ key: 'abbe_status', value: {{ online: !failed('Check Abbe'), ip: IPS.abbe, temp_c: abbeTemp }} }} }},
  {{ json: {{ key: 'nas_status', value: {{ online: !failed('Check NAS'), ip: IPS.nas, temp_c: nasTemp }} }} }},
  {{ json: {{ key: 'pilab_status', value: {{ online: true, ip: IPS.pilab, temp_c: pilabTemp }} }} }},
];
""".strip()

# Firmware's systems_mqtt.c expects one JSON object keyed by short name
# (nas/pilab/abbe/sky), not the "<name>_status" keys the Postgres rows use
# above and not one execution per row - a separate single-item branch off
# the same three HTTP checks, rather than reusing "Parse Into Rows"'s
# 4-item output (that would run the MQTT publish node 4 times per cycle,
# publishing the same retained message redundantly).
mqtt_payload_js = f"""
{shared_helpers_js}

const IPS = {json.dumps(IPS)};

// No aircraft_count here (removed 2026-09-11, not wanted on the Systems
// screen row) - still recorded in the Postgres row above for diagnostics,
// this is just the display payload. temp_c added 2026-09-14, explicit
// request - firmware's has_temp gate (systems_mqtt.c) treats a
// null/missing temp_c the same as before this field existed.
return [{{
  json: {{
    nas: {{ online: !failed('Check NAS'), ip: IPS.nas, temp_c: parseNasTemp() }},
    pilab: {{ online: true, ip: IPS.pilab, temp_c: parseTemp('Temp PiLab') }},
    abbe: {{ online: !failed('Check Abbe'), ip: IPS.abbe, temp_c: parseTemp('Temp Abbe') }},
    sky: {{ online: !failed('Check Sky'), ip: IPS.sky, temp_c: parseTemp('Temp Sky') }},
  }},
}}];
""".strip()

# Feeds the box3 dashboard's shared notification strip (see
# notification_mqtt.c/home_screen.c) - level-triggered, not one-off:
# publishes a red "X is offline" message whenever any monitored system is
# down, and an empty message (which notification_mqtt.c treats as "clear
# this source's notification") once it recovers. Only publishes on an
# actual state CHANGE (plus a periodic re-publish while still true, see
# REPUBLISH_INTERVAL_MS below), not every 30s cycle - live-tested
# 2026-09-15 with a real ongoing NAS outage and caught this the hard way:
# without the static-data change-check below, this node would re-publish
# "Nas is offline" every single cycle, and back when the notification
# strip was a single "last call wins" slot (since replaced by a real
# priority system, see home_screen.c), an ongoing outage would
# permanently starve out every other source. $getWorkflowStaticData
# persists across executions of this same workflow, so it's a reliable
# one-line way to remember "what did we publish last time" without a
# Postgres round-trip.
notification_payload_js = f"""
{shared_helpers_js}

const staticData = $getWorkflowStaticData('global');
const results = [];

// NAS excluded from the red alert below - explicit "it's off most of
// the time, it's known, drop it from the alert" 2026-09-15 (not a real
// problem worth a red banner or the beep that rides on it - see
// notification_sound.c). Still shown/checked normally on the full
// Systems screen (Parse Into Rows/Build MQTT Payload above). Opposite
// polarity instead, per an immediate same-day follow-up ("sometimes I
// forget, can you make that one green if the NAS is on?"): silent when
// off, a green confirmation when it's actually on. Tracked separately
// from the abbe/sky/pilab offline set below - own static-data key, own
// change check, own notification source ("nas").
const nasOnline = !failed('Check NAS');
if (staticData.lastNasOnline !== nasOnline) {{
  staticData.lastNasOnline = nasOnline;
  results.push({{
    json: {{ source: 'nas', message: nasOnline ? 'NAS is on' : '', severity: 'green', ttl_min: 0 }},
  }});
}}

// Split into 3 independent sources (pilab/sky/abbe), each its own
// notification slot - explicit request ("Pilab Offline (Red), Sky
// offline (Red), Abbe offline (Red)" as separate catalog entries).
// Replaces the old combined "systems" source (one shared message like
// "Abbe, Sky are offline") - independent sources means Sky recovering
// doesn't require Abbe to also be back before ITS alert clears.
//
// PiLab is a real structural exception, not an oversight: this workflow
// runs ON PiLab's own n8n instance, so if PiLab itself goes down, this
// very check stops running too - it can never observe its own outage.
// Kept as its own source (for symmetry, and in case external monitoring
// ever feeds this key some other way) but the value itself stays
// hardcoded `true` - same limitation the old code already had.
const systems = {{
  pilab: true,
  sky: !failed('Check Sky'),
  abbe: !failed('Check Abbe'),
}};

// Firmware caps a red alert's own DISPLAY window (so it can't block the
// calendar/blue screen forever) and steps back to whatever's next in
// priority once that expires - the alert itself stays active/pending,
// it just isn't shown on the strip. That only works long-term if a
// still-ongoing outage keeps getting re-asserted, not just on the
// initial state change - explicitly chosen over the alternative (flash
// once, then silent for the rest of the outage) when asked which of the
// two to build. Every 30 min here is independent of the per-system
// change check below (which still fires immediately on any real
// transition) - this is purely the "still true, remind again" path for
// an unresolved condition.
const REPUBLISH_INTERVAL_MS = 30 * 60 * 1000;

for (const key of Object.keys(systems)) {{
  const online = systems[key];
  const name = key.charAt(0).toUpperCase() + key.slice(1);
  const lastKey = `last_${{key}}_online`;
  const publishedAtKey = `last_${{key}}_publishedAt`;

  if (staticData[lastKey] !== online) {{
    staticData[lastKey] = online;
    staticData[publishedAtKey] = Date.now();
    results.push({{
      json: {{ source: key, message: online ? '' : `${{name}} is offline`, severity: 'red', ttl_min: 0 }},
    }});
  }} else if (
    !online &&
    (!staticData[publishedAtKey] || Date.now() - staticData[publishedAtKey] >= REPUBLISH_INTERVAL_MS)
  ) {{
    staticData[publishedAtKey] = Date.now();
    results.push({{ json: {{ source: key, message: `${{name}} is offline`, severity: 'red', ttl_min: 0 }} }});
  }}
}}

return results;
""".strip()

# Values go in as Postgres query parameters ($1, $2 via options.queryReplacement),
# never pasted into the SQL text - 2026-09-24: an aircraft owner named
# "L'EAGLE AIR II INC" broke the old '{{ JSON.stringify($json) }}' literal
# (apostrophe ended the string -> syntax error). Parameters accept any character.
pg_query = (
    "INSERT INTO homeops.state_cache (key, value) "
    "VALUES ($1, $2::jsonb) "
    "ON CONFLICT (key) DO UPDATE SET value = EXCLUDED.value, updated_at = now();"
)

nodes = [
    {
        "id": node_id(),
        "name": "Every 30s",
        "type": "n8n-nodes-base.scheduleTrigger",
        "typeVersion": 1.3,
        "position": [0, 0],
        "parameters": {"rule": {"interval": [{"field": "seconds", "secondsInterval": 30}]}},
    },
    # Static IP, not the "sky" hostname - see the matching note in
    # rebuild_aircraft_poll_workflow.py. Consistent now with how Abbe/NAS
    # are already checked below.
    http_check_node("Check Sky", "http://sky.local/tar1090/data/aircraft.json", 220),
    http_check_node("Check Abbe", "http://vpn.local/admin/", 440),
    http_check_node("Check NAS", "http://nas.local/", 660),
    # CPU temp readouts for the Systems screen - explicit request
    # 2026-09-14. onError continueRegularOutput same as the HTTP checks -
    # a dead SSH connection shouldn't break the rest of the chain, just
    # leaves that box's temp_c null (see shared_helpers_js's parseTemp()).
    {
        "id": node_id(),
        "name": "Temp PiLab",
        "type": "n8n-nodes-base.ssh",
        "typeVersion": 1,
        "position": [770, 200],
        "onError": "continueRegularOutput",
        "parameters": {
            "authentication": "privateKey",
            "resource": "command",
            "operation": "execute",
            "command": "vcgencmd measure_temp",
        },
        "credentials": {"sshPrivateKey": SSH_CRED_PILAB},
    },
    {
        "id": node_id(),
        "name": "Temp Sky",
        "type": "n8n-nodes-base.ssh",
        "typeVersion": 1,
        "position": [880, 200],
        "onError": "continueRegularOutput",
        "parameters": {
            "authentication": "privateKey",
            "resource": "command",
            "operation": "execute",
            "command": "vcgencmd measure_temp",
        },
        "credentials": {"sshPrivateKey": SSH_CRED_SKY},
    },
    {
        "id": node_id(),
        "name": "Temp Abbe",
        "type": "n8n-nodes-base.ssh",
        "typeVersion": 1,
        "position": [990, 200],
        "onError": "continueRegularOutput",
        "parameters": {
            "authentication": "privateKey",
            "resource": "command",
            "operation": "execute",
            "command": "vcgencmd measure_temp",
        },
        "credentials": {"sshPrivateKey": SSH_CRED_ABBE},
    },
    {
        # A literal object inside the HTTP node's own jsonBody expression
        # (nested {}'s right inside the {{ }} mustache) hit n8n's
        # expression parser as "invalid syntax" - confirmed live 2026-09-14,
        # the "Check NAS Temp" node errored on every run. Every other JSON
        # POST body in this project (see rebuild_lights_control_workflow.py)
        # builds the object in a preceding Code node and just does
        # "{{ JSON.stringify($json) }}" in the HTTP node - following that
        # same working pattern here instead.
        "id": node_id(),
        "name": "Build NAS Temp Query",
        "type": "n8n-nodes-base.code",
        "typeVersion": 2,
        "position": [990, 320],
        "parameters": {
            "language": "javaScript",
            "jsCode": (
                "const now = Math.floor(Date.now() / 1000);\n"
                "return [{ json: { graphs: [{ name: 'cputemp' }], "
                "query: { start: now - 60, end: now, aggregate: true } } }];"
            ),
        },
    },
    {
        "id": node_id(),
        "name": "Check NAS Temp",
        "type": "n8n-nodes-base.httpRequest",
        "typeVersion": 4.5,
        "position": [1100, 200],
        "onError": "continueRegularOutput",
        "parameters": {
            "url": "http://nas.local/api/v2.0/reporting/get_data",
            "method": "POST",
            "authentication": "genericCredentialType",
            "genericAuthType": "httpBearerAuth",
            "sendBody": True,
            "specifyBody": "json",
            "jsonBody": "={{ JSON.stringify($json) }}",
            "options": {"timeout": 5000},
        },
        "credentials": {"httpBearerAuth": TRUENAS_CRED},
    },
    {
        "id": node_id(),
        "name": "Parse Into Rows",
        "type": "n8n-nodes-base.code",
        "typeVersion": 2,
        "position": [880, -80],
        "parameters": {"language": "javaScript", "jsCode": code_js},
    },
    {
        "id": node_id(),
        "name": "Upsert Cache Row",
        "type": "n8n-nodes-base.postgres",
        "typeVersion": 2.6,
        "position": [1100, -80],
        "parameters": {"operation": "executeQuery", "query": pg_query, "options": {"queryReplacement": "={{ [$json.key, JSON.stringify($json.value)] }}"}},
        "credentials": {"postgres": POSTGRES_CRED},
    },
    {
        "id": node_id(),
        "name": "Build MQTT Payload",
        "type": "n8n-nodes-base.code",
        "typeVersion": 2,
        "position": [880, 80],
        "parameters": {"language": "javaScript", "jsCode": mqtt_payload_js},
    },
    {
        "id": node_id(),
        "name": "Publish Systems State",
        "type": "n8n-nodes-base.mqtt",
        "typeVersion": 1,
        "position": [1100, 80],
        "parameters": {"topic": "homeops/systems/state", "sendInputData": True, "options": {"retain": True}},
        "credentials": {"mqtt": MQTT_CRED},
    },
    {
        "id": node_id(),
        "name": "Build Notification Payload",
        "type": "n8n-nodes-base.code",
        "typeVersion": 2,
        "position": [880, 440],
        "parameters": {"language": "javaScript", "jsCode": notification_payload_js},
    },
    {
        "id": node_id(),
        "name": "Publish Notification",
        "type": "n8n-nodes-base.mqtt",
        "typeVersion": 1,
        "position": [1100, 440],
        "parameters": {"topic": "homeops/notification/state", "sendInputData": True, "options": {"retain": True}},
        "credentials": {"mqtt": MQTT_CRED},
    },
]

connections = {
    "Every 30s": {"main": [[{"node": "Check Sky", "type": "main", "index": 0}]]},
    "Check Sky": {"main": [[{"node": "Check Abbe", "type": "main", "index": 0}]]},
    "Check Abbe": {"main": [[{"node": "Check NAS", "type": "main", "index": 0}]]},
    "Check NAS": {"main": [[{"node": "Temp PiLab", "type": "main", "index": 0}]]},
    "Temp PiLab": {"main": [[{"node": "Temp Sky", "type": "main", "index": 0}]]},
    "Temp Sky": {"main": [[{"node": "Temp Abbe", "type": "main", "index": 0}]]},
    "Temp Abbe": {"main": [[{"node": "Build NAS Temp Query", "type": "main", "index": 0}]]},
    "Build NAS Temp Query": {"main": [[{"node": "Check NAS Temp", "type": "main", "index": 0}]]},
    "Check NAS Temp": {
        "main": [[
            {"node": "Parse Into Rows", "type": "main", "index": 0},
            {"node": "Build MQTT Payload", "type": "main", "index": 0},
            {"node": "Build Notification Payload", "type": "main", "index": 0},
        ]]
    },
    "Parse Into Rows": {"main": [[{"node": "Upsert Cache Row", "type": "main", "index": 0}]]},
    "Build MQTT Payload": {"main": [[{"node": "Publish Systems State", "type": "main", "index": 0}]]},
    "Build Notification Payload": {"main": [[{"node": "Publish Notification", "type": "main", "index": 0}]]},
}

workflow = {
    "name": "Home Ops - System Health Check",
    "nodes": nodes,
    "connections": connections,
    "settings": {
        "executionOrder": "v1",
        "errorWorkflow": ERROR_WORKFLOW_ID,
        "callerPolicy": "workflowsFromSameOwner",
    },
}

WF = open(os.path.expanduser("~/home-ops-dashboard/n8n/health_check_workflow_id.txt")).read().strip()

req = urllib.request.Request(
    f"{N8N_URL}/api/v1/workflows/{WF}",
    data=json.dumps(workflow).encode(),
    headers={"X-N8N-API-KEY": API_KEY, "Content-Type": "application/json"},
    method="PUT",
)

with urllib.request.urlopen(req) as resp:
    result = json.load(resp)
    print("Updated. Active:", result["active"])
