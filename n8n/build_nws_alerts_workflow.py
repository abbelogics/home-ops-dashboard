"""Home Ops - NWS Alerts (added 2026-09-24).

Official NWS watches / warnings / advisories for the office's exact point
(api.weather.gov/alerts/active?point=...) -> BOX-3 notification bar, plus a
Telegram for new Warnings and Watches (Telegram-only preference; advisories
and statements stay on the bar only - heat advisories alone would be noisy
in a Miami summer). Explicit request ("advisories", 2026-09-24).

Why point-based: storm-based warnings (severe thunderstorm, tornado, flash
flood) are polygons, so only a polygon actually covering the building
matches - not every warning somewhere in Miami-Dade. Watches/advisories
are zone-based (FLZ074 Metro Miami-Dade / county FLC086).

Bar payload (homeops/notification/state, source "nws"):
  severity "red"    - any Warning, or NWS severity Extreme
  severity "orange" - Watches, Advisories, Statements
  icon "hurricane" (tropical/surge events) or "warning" - read by firmware
      from the 2026-09-24 build on; older firmware just shows no icon.
Published NON-retained, re-sent every run while active: the calendar owns
the retained message on this topic (a retained NWS publish would replace
it), and the firmware only re-pops/beeps when the text actually changes,
so a repeat is silent. An empty message clears the slot once alerts end.

Message text is kept stable across re-sends (event names + "until" from the
alert's `ends`, never `expires`, which NWS bumps on every routine reissue).

Run once to create; re-running updates the same workflow in place.
"""
import json
import os
import uuid

import urllib.request

N8N_URL = "http://pilab.local:5678"
API_KEY = open(os.path.expanduser("~/home-ops-dashboard/n8n/.n8n_api_key")).read().strip()
MQTT_CRED = {"id": "YOUR_N8N_CREDENTIAL_ID", "name": "Mosquitto (PiLab)"}
POSTGRES_CRED = {"id": "YOUR_N8N_CREDENTIAL_ID", "name": "Postgres account"}
TELEGRAM_CRED = {"id": "YOUR_N8N_CREDENTIAL_ID", "name": "Telegram account"}
TELEGRAM_CHAT_ID = "YOUR_TELEGRAM_CHAT_ID"
ERROR_WORKFLOW_ID = "YOUR_WORKFLOW_ID"
WF_ID_FILE = os.path.expanduser("~/home-ops-dashboard/n8n/nws_alerts_workflow_id.txt")

# User-calibrated office location, rounded to the 4 decimals api.weather.gov
# accepts (more is rejected) - ~10 m from the exact pin.
POINT = "YOUR_LAT,YOUR_LON"


def node_id():
    return str(uuid.uuid4())


build_js = r"""
const sd = $getWorkflowStaticData('global');
sd.sent = sd.sent || {};          // alert id -> first-seen ms (Telegram dedup)
sd.lastMessage = sd.lastMessage ?? '';
const r = $input.first().json;

// Fetch failed (network / NWS API blip): change nothing - keep whatever is
// showing, never clear an active alert because of an outage.
if (!r || !Array.isArray(r.features)) return [{ json: { notify: null, telegrams: [], store: null, changed: false } }];

const now = Date.now();
const alerts = r.features.map(f => f.properties || {})
  .filter(p => p.status === 'Actual' && p.messageType !== 'Cancel')
  .filter(p => !p.ends || Date.parse(p.ends) > now);

const isRed = p => /Warning/i.test(p.event || '') || p.severity === 'Extreme';
const isTelegram = p => /Warning|Watch/i.test(p.event || '');
const rank = p => (isRed(p) ? 0 : 1) * 10 + (['Extreme', 'Severe', 'Moderate', 'Minor'].indexOf(p.severity) + 1);
alerts.sort((a, b) => rank(a) - rank(b));

function until(p) {
  if (!p.ends) return '';
  const d = new Date(p.ends);
  const tz = 'America/New_York';
  const day = x => x.toLocaleDateString('en-US', { timeZone: tz });
  const time = d.toLocaleTimeString('en-US', { timeZone: tz, hour: 'numeric', minute: '2-digit' }).replace(':00', '');
  const sameDay = day(d) === day(new Date());
  return ' until ' + (sameDay ? '' : d.toLocaleDateString('en-US', { timeZone: tz, weekday: 'short' }) + ' ') + time;
}

// One slot per source on the device -> one combined line, most severe
// first, duplicates (same event from overlapping zones) collapsed.
const seen = new Set();
const parts = [];
for (const p of alerts) {
  if (seen.has(p.event)) continue;
  seen.add(p.event);
  parts.push(p.event + (parts.length === 0 ? until(p) : ''));
}
let message = parts.join(' + ');
if (message.length > 190) message = message.slice(0, 187) + '...';

let notify = null;
if (alerts.length) {
  const top = alerts[0];
  notify = {
    source: 'nws',
    message,
    severity: isRed(top) ? 'red' : 'orange',
    ttl_min: 0,
    icon: /Hurricane|Tropical|Storm Surge/i.test(alerts.map(a => a.event).join(' ')) ? 'hurricane' : 'warning',
  };
} else if (sd.lastMessage !== '') {
  notify = { source: 'nws', message: '', severity: 'orange', ttl_min: 0 };  // clear once
}
sd.lastMessage = message;

// Telegram: a new Warning/Watch only. An NWS update carries the earlier
// ids in `references` - if any of those was already sent, it's the same
// alert being reissued, not a new one.
const telegrams = [];
for (const p of alerts) {
  if (!p.id || sd.sent[p.id]) continue;
  const refs = (p.references || []).map(x => x.identifier || x['@id'] || '');
  const isUpdate = refs.some(id => sd.sent[id]);
  sd.sent[p.id] = now;
  if (isUpdate || !isTelegram(p)) continue;
  const icon = isRed(p) ? '\u{1F534}' : '\u{1F7E0}';
  const text = [
    `${icon} ${p.event}${until(p)}`,
    p.headline || '',
    p.instruction ? '\n' + p.instruction.replace(/\s+/g, ' ').slice(0, 600) : '',
  ].filter(Boolean).join('\n');
  telegrams.push({ text });
}
// Forget ids after 7 days so staticData doesn't grow forever.
for (const [id, t] of Object.entries(sd.sent)) if (now - t > 7 * 86400e3) delete sd.sent[id];

// Home-screen weather line (2026-09-25): the single most important alert,
// short enough for one 16px line, saved to homeops.state_cache 'nws_alert'
// for the Weather Poll to fold in. A change pings the Weather Poll webhook
// so the line updates within seconds, not at the next 5-min poll.
let store = { text: '', severity: '', icon: '' };
if (alerts.length) {
  const top = alerts[0];
  const more = seen.size > 1 ? ` +${seen.size - 1}` : '';
  store = { text: top.event + until(top) + more, severity: isRed(top) ? 'red' : 'orange',
            icon: /Hurricane|Tropical|Storm Surge/i.test(top.event) ? 'hurricane' : 'warning' };
}
const storeKey = JSON.stringify(store);
const changed = sd.lastStore !== undefined && sd.lastStore !== storeKey;
sd.lastStore = storeKey;

return [{ json: { notify, telegrams, store, changed } }];
""".strip()

store_query = (
    "INSERT INTO homeops.state_cache (key, value) VALUES ('nws_alert', $1::jsonb) "
    "ON CONFLICT (key) DO UPDATE SET value = EXCLUDED.value, updated_at = now();"
)

nodes = [
    {
        "id": node_id(), "name": "Every 2 min", "type": "n8n-nodes-base.scheduleTrigger",
        "typeVersion": 1.3, "position": [0, 0],
        "parameters": {"rule": {"interval": [{"field": "minutes", "minutesInterval": 2}]}},
    },
    {
        # api.weather.gov serves application/geo+json, which n8n does not
        # auto-detect as JSON - responseFormat must be explicit (PROJECT.md,
        # 2026-09-11 NWS notes). A User-Agent is required by NWS.
        "id": node_id(), "name": "Fetch Alerts", "type": "n8n-nodes-base.httpRequest",
        "typeVersion": 4.5, "position": [220, 0], "onError": "continueRegularOutput", "retryOnFail": True, "maxTries": 3, "waitBetweenTries": 5000,
        "parameters": {
            "url": f"https://api.weather.gov/alerts/active?point={POINT}",
            "sendHeaders": True,
            "headerParameters": {"parameters": [
                {"name": "User-Agent", "value": "home-ops-dashboard (PiLab)"},
                {"name": "Accept", "value": "application/geo+json"},
            ]},
            "options": {"timeout": 15000, "response": {"response": {"responseFormat": "json"}}},
        },
    },
    {
        "id": node_id(), "name": "Build Alerts", "type": "n8n-nodes-base.code",
        "typeVersion": 2, "position": [440, 0],
        "parameters": {"language": "javaScript", "jsCode": build_js},
    },
    {
        "id": node_id(), "name": "Notify Payload", "type": "n8n-nodes-base.code",
        "typeVersion": 2, "position": [660, -100],
        "parameters": {"language": "javaScript",
                       "jsCode": "const n = $input.first().json.notify;\nreturn n ? [{ json: n }] : [];"},
    },
    {
        "id": node_id(), "name": "Publish Notification", "type": "n8n-nodes-base.mqtt",
        "typeVersion": 1, "position": [880, -100],
        "parameters": {"topic": "homeops/notification/state", "sendInputData": True, "options": {"retain": False}},
        "credentials": {"mqtt": MQTT_CRED},
    },
    {
        "id": node_id(), "name": "Store Payload", "type": "n8n-nodes-base.code",
        "typeVersion": 2, "position": [660, 300],
        "parameters": {"language": "javaScript",
                       "jsCode": "const s = $input.first().json.store;\nreturn s ? [{ json: s }] : [];"},
    },
    {
        "id": node_id(), "name": "Store Alert", "type": "n8n-nodes-base.postgres",
        "typeVersion": 2.6, "position": [880, 300],
        "parameters": {"operation": "executeQuery", "query": store_query,
                       "options": {"queryReplacement": "={{ [JSON.stringify($json)] }}"}},
        "credentials": {"postgres": POSTGRES_CRED},
    },
    {
        "id": node_id(), "name": "Changed?", "type": "n8n-nodes-base.code",
        "typeVersion": 2, "position": [1100, 300],
        "parameters": {"language": "javaScript",
                       "jsCode": "return $('Build Alerts').first().json.changed ? [{ json: {} }] : [];"},
    },
    {
        "id": node_id(), "name": "Refresh Weather", "type": "n8n-nodes-base.httpRequest",
        "typeVersion": 4.5, "position": [1320, 300], "onError": "continueRegularOutput",
        "parameters": {"url": "http://localhost:5678/webhook/homeops-weather-refresh", "options": {"timeout": 10000}},
    },
    {
        "id": node_id(), "name": "Telegram Items", "type": "n8n-nodes-base.code",
        "typeVersion": 2, "position": [660, 100],
        "parameters": {"language": "javaScript",
                       "jsCode": "return ($input.first().json.telegrams || []).map(t => ({ json: t }));"},
    },
    {
        "id": node_id(), "name": "Send Telegram", "type": "n8n-nodes-base.telegram",
        "typeVersion": 1.2, "position": [880, 100],
        "webhookId": node_id(),
        "parameters": {"chatId": TELEGRAM_CHAT_ID, "text": "={{ $json.text }}",
                       "additionalFields": {"appendAttribution": False}},
        "credentials": {"telegramApi": TELEGRAM_CRED},
    },
]

connections = {
    "Every 2 min": {"main": [[{"node": "Fetch Alerts", "type": "main", "index": 0}]]},
    "Fetch Alerts": {"main": [[{"node": "Build Alerts", "type": "main", "index": 0}]]},
    "Build Alerts": {"main": [[
        {"node": "Notify Payload", "type": "main", "index": 0},
        {"node": "Telegram Items", "type": "main", "index": 0},
        {"node": "Store Payload", "type": "main", "index": 0},
    ]]},
    "Store Payload": {"main": [[{"node": "Store Alert", "type": "main", "index": 0}]]},
    "Store Alert": {"main": [[{"node": "Changed?", "type": "main", "index": 0}]]},
    "Changed?": {"main": [[{"node": "Refresh Weather", "type": "main", "index": 0}]]},
    "Notify Payload": {"main": [[{"node": "Publish Notification", "type": "main", "index": 0}]]},
    "Telegram Items": {"main": [[{"node": "Send Telegram", "type": "main", "index": 0}]]},
}

workflow = {
    "name": "Home Ops - NWS Alerts",
    "nodes": nodes,
    "connections": connections,
    "settings": {
        "executionOrder": "v1",
        "errorWorkflow": ERROR_WORKFLOW_ID,
        "callerPolicy": "workflowsFromSameOwner",
        "saveDataSuccessExecution": "all",
    },
}


def api(path, method, body=None):
    req = urllib.request.Request(
        f"{N8N_URL}/api/v1{path}",
        data=json.dumps(body).encode() if body is not None else None,
        headers={"X-N8N-API-KEY": API_KEY, "Content-Type": "application/json"},
        method=method,
    )
    with urllib.request.urlopen(req) as resp:
        return json.load(resp)


if __name__ == "__main__":
    if os.path.exists(WF_ID_FILE):
        wf_id = open(WF_ID_FILE).read().strip()
        api(f"/workflows/{wf_id}", "PUT", workflow)
        print("Updated", wf_id)
    else:
        wf_id = api("/workflows", "POST", workflow)["id"]
        with open(WF_ID_FILE, "w") as f:
            f.write(wf_id)
        print("Created", wf_id)
    result = api(f"/workflows/{wf_id}/activate", "POST")
    print("Active:", result["active"])
