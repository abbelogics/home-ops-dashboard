"""Home Ops - F1 Sessions (added 2026-09-24).

Publishes a notification-bar alert 15, 10 and 5 minutes before every F1
session (practice, sprint qualifying, sprint, qualifying, race) - explicit
request. calendar.formula1.com itself is an ECAL sign-up widget with no
readable feed, so the schedule comes from Jolpica F1
(api.jolpi.ca/ergast/f1/current.json - the Ergast successor, free, no key,
per-session UTC times incl. sprint weekends).

Shape: every minute, "Needs Refresh?" decides whether the cached schedule
(workflow staticData) is older than 6h -> if so, fetch + store it -> then
"Check Sessions" publishes at most one alert. Non-retained on purpose (same
as the package-delivery source): a retained "in 5 min" would resurface on
every BOX-3 reboot long after the session started. A non-retained publish
also leaves the calendar's own retained message on this topic untouched.

Payload extras read by the firmware (notification_mqtt.c):
  icon:  "practice" (car) / "quali" (car + stopwatch) / "race" (car + flag)
  sound: true outside 23:00-07:00 local - quiet hours, explicit request
         (the bar still updates at night, it just doesn't chime).

Run once to create; re-running updates the same workflow in place.
"""
import json
import os
import uuid

import urllib.error
import urllib.request

N8N_URL = "http://pilab.local:5678"
API_KEY = open(os.path.expanduser("~/home-ops-dashboard/n8n/.n8n_api_key")).read().strip()
MQTT_CRED = {"id": "YOUR_N8N_CREDENTIAL_ID", "name": "Mosquitto (PiLab)"}
ERROR_WORKFLOW_ID = "YOUR_WORKFLOW_ID"
WF_ID_FILE = os.path.expanduser("~/home-ops-dashboard/n8n/f1_sessions_workflow_id.txt")
TIMEZONE = "America/New_York"


def node_id():
    return str(uuid.uuid4())


needs_refresh_js = """
const sd = $getWorkflowStaticData('global');
const REFRESH_MS = 6 * 60 * 60 * 1000;
// SCHEMA bumps force a refetch when the stored session shape changes.
const SCHEMA = 2;
const stale = !sd.schedule || !sd.fetchedAt || sd.schema !== SCHEMA || Date.now() - sd.fetchedAt > REFRESH_MS;
return [{ json: { refresh: stale ? 1 : 0 } }];
""".strip()

# Flattens Jolpica's per-race session fields into one list. Only stores a
# new schedule if the response actually parsed - a failed fetch (onError
# continue upstream) keeps the previous cache and retries next minute.
store_schedule_js = """
const sd = $getWorkflowStaticData('global');
const races = $input.first().json?.MRData?.RaceTable?.Races;
if (!Array.isArray(races) || races.length === 0) {
  return [{ json: { stored: 0 } }];
}
// [Jolpica field, bar label, icon, badge code, assumed length in min].
// Jolpica only has start times, so the "session live" badge uses fixed
// lengths (explicit: race 2h, even though a red flag can run longer).
const FIELDS = [
  ['FirstPractice', 'Practice 1', 'practice', 'P1', 60],
  ['SecondPractice', 'Practice 2', 'practice', 'P2', 60],
  ['ThirdPractice', 'Practice 3', 'practice', 'P3', 60],
  ['SprintQualifying', 'Sprint Qualifying', 'quali', 'SQ', 45],
  ['Sprint', 'Sprint', 'race', 'Sprint', 60],
  ['Qualifying', 'Qualifying', 'quali', 'Q', 60],
];
const sessions = [];
for (const r of races) {
  const gp = r.raceName.replace(' Grand Prix', ' GP');
  for (const [key, label, icon, code, mins] of FIELDS) {
    const s = r[key];
    if (s?.date && s?.time) sessions.push({ gp, label, icon, code, mins, start: Date.parse(`${s.date}T${s.time}`) });
  }
  if (r.date && r.time) {
    sessions.push({ gp, label: 'Race', icon: 'race', code: 'Race', mins: 120, start: Date.parse(`${r.date}T${r.time}`) });
  }
}
sd.schedule = sessions.filter((s) => !Number.isNaN(s.start));
sd.fetchedAt = Date.now();
sd.schema = 2;
return [{ json: { stored: sd.schedule.length } }];
""".strip()

# One alert per (session, tier). Tiers are windows, not exact minutes, so
# a run that lands a few seconds late (or a deploy mid-window) still fires
# the right tier exactly once. The message shows the real minutes left
# (normally exactly 15/10/5 on a 1-minute cadence). ttl 5 = each alert
# lives until the next tier replaces it; the last one expires at the start.
check_sessions_js = """
const sd = $getWorkflowStaticData('global');
const sessions = sd.schedule || [];
const now = Date.now();
sd.sent = sd.sent || {};
for (const k of Object.keys(sd.sent)) {
  if (now - sd.sent[k] > 24 * 60 * 60 * 1000) delete sd.sent[k];
}

const TIERS = [15, 10, 5];
const hour = Number(new Date(now).toLocaleString('en-US', { hour: 'numeric', hourCycle: 'h23', timeZone: '""" + TIMEZONE + """' }));
const quiet = hour >= 23 || hour < 7;

const out = [];
for (const s of sessions) {
  const minsLeft = (s.start - now) / 60000;
  if (minsLeft <= 0 || minsLeft > TIERS[0]) continue;
  // Smallest tier the session is already inside: 12 min left -> tier 15.
  let tier = TIERS[0];
  for (const t of TIERS) if (minsLeft <= t) tier = t;
  const key = `${s.start}|${s.label}|${tier}`;
  if (sd.sent[key]) continue;
  sd.sent[key] = now;
  out.push({ json: {
    source: 'f1',
    message: `${s.label} - ${s.gp} in ${Math.ceil(minsLeft)} min`,
    severity: 'f1',
    ttl_min: 5,
    icon: s.icon,
    sound: !quiet,
  } });
}
// "Session live" badge: the session currently running, if any. Only
// reported when it changes (retained topic - the broker hands the latest
// to the device on every reconnect); lastLive starts undefined so the
// first run after a deploy always publishes.
const live = sessions.find((s) => now >= s.start && now < s.start + s.mins * 60000);
const liveKey = live ? `${live.start}|${live.code}` : '';
let session = null;
if (sd.lastLive !== liveKey) {
  sd.lastLive = liveKey;
  session = live
    ? { code: live.code, gp: live.gp, ends_at: Math.floor((live.start + live.mins * 60000) / 1000) }
    : { code: '', ends_at: 0 };
}

// Two sessions can't realistically overlap, but if they ever did, only
// the soonest countdown goes out - one slot per source on the device.
// Always one item, so both payload branches below run every minute.
return [{ json: { alert: out.length ? out[0].json : null, session } }];
""".strip()

nodes = [
    {
        "id": node_id(), "name": "Every Minute", "type": "n8n-nodes-base.scheduleTrigger",
        "typeVersion": 1.3, "position": [0, 0],
        "parameters": {"rule": {"interval": [{"field": "minutes", "minutesInterval": 1}]}},
    },
    {
        "id": node_id(), "name": "Needs Refresh?", "type": "n8n-nodes-base.code",
        "typeVersion": 2, "position": [220, 0],
        "parameters": {"language": "javaScript", "jsCode": needs_refresh_js},
    },
    {
        # Number compare, not boolean - see the n8n boolean-If bug noted
        # in PROJECT.md / the Error Handler rebuild.
        "id": node_id(), "name": "Refresh?", "type": "n8n-nodes-base.if",
        "typeVersion": 2, "position": [440, 0],
        "parameters": {
            "conditions": {
                "options": {"caseSensitive": True, "leftValue": "", "typeValidation": "strict"},
                "conditions": [{
                    "id": node_id(),
                    "leftValue": "={{ $json.refresh }}",
                    "rightValue": 0,
                    "operator": {"type": "number", "operation": "gt"},
                }],
                "combinator": "and",
            },
            "options": {},
        },
    },
    {
        "id": node_id(), "name": "Fetch F1 Schedule", "type": "n8n-nodes-base.httpRequest",
        "typeVersion": 4.5, "position": [660, -120], "onError": "continueRegularOutput",
        "parameters": {"url": "https://api.jolpi.ca/ergast/f1/current.json", "options": {"timeout": 10000}},
    },
    {
        "id": node_id(), "name": "Store Schedule", "type": "n8n-nodes-base.code",
        "typeVersion": 2, "position": [880, -120],
        "parameters": {"language": "javaScript", "jsCode": store_schedule_js},
    },
    {
        "id": node_id(), "name": "Check Sessions", "type": "n8n-nodes-base.code",
        "typeVersion": 2, "position": [1100, 0],
        "parameters": {"language": "javaScript", "jsCode": check_sessions_js},
    },
    {
        "id": node_id(), "name": "Publish Notification", "type": "n8n-nodes-base.mqtt",
        "typeVersion": 1, "position": [1540, -100],
        "parameters": {"topic": "homeops/notification/state", "sendInputData": True, "options": {"retain": False}},
        "credentials": {"mqtt": MQTT_CRED},
    },
    {
        "id": node_id(), "name": "Alert Payload", "type": "n8n-nodes-base.code",
        "typeVersion": 2, "position": [1320, -100],
        "parameters": {"language": "javaScript",
                       "jsCode": "const a = $input.first().json.alert;\nreturn a ? [{ json: a }] : [];"},
    },
    {
        "id": node_id(), "name": "Session Payload", "type": "n8n-nodes-base.code",
        "typeVersion": 2, "position": [1320, 100],
        "parameters": {"language": "javaScript",
                       "jsCode": "const s = $input.first().json.session;\nreturn s ? [{ json: s }] : [];"},
    },
    {
        # Retained, unlike the countdown: a reboot mid-session should get
        # the badge back. The device also expires it by its own clock.
        "id": node_id(), "name": "Publish Session", "type": "n8n-nodes-base.mqtt",
        "typeVersion": 1, "position": [1540, 100],
        "parameters": {"topic": "homeops/f1/session", "sendInputData": True, "options": {"retain": True}},
        "credentials": {"mqtt": MQTT_CRED},
    },
]

connections = {
    "Every Minute": {"main": [[{"node": "Needs Refresh?", "type": "main", "index": 0}]]},
    "Needs Refresh?": {"main": [[{"node": "Refresh?", "type": "main", "index": 0}]]},
    "Refresh?": {"main": [
        [{"node": "Fetch F1 Schedule", "type": "main", "index": 0}],
        [{"node": "Check Sessions", "type": "main", "index": 0}],
    ]},
    "Fetch F1 Schedule": {"main": [[{"node": "Store Schedule", "type": "main", "index": 0}]]},
    "Store Schedule": {"main": [[{"node": "Check Sessions", "type": "main", "index": 0}]]},
    "Check Sessions": {"main": [[
        {"node": "Alert Payload", "type": "main", "index": 0},
        {"node": "Session Payload", "type": "main", "index": 0},
    ]]},
    "Alert Payload": {"main": [[{"node": "Publish Notification", "type": "main", "index": 0}]]},
    "Session Payload": {"main": [[{"node": "Publish Session", "type": "main", "index": 0}]]},
}

workflow = {
    "name": "Home Ops - F1 Sessions",
    "nodes": nodes,
    "connections": connections,
    "settings": {
        "executionOrder": "v1",
        "errorWorkflow": ERROR_WORKFLOW_ID,
        "callerPolicy": "workflowsFromSameOwner",
        # Explicit, not omitted - see rebuild_aircraft_poll_workflow.py:
        # "none" left runs stuck as "running" forever on n8n 2.41.
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
