import json
import os
import uuid

import urllib.request

N8N_URL = "http://pilab.local:5678"
API_KEY = open(os.path.expanduser("~/home-ops-dashboard/n8n/.n8n_api_key")).read().strip()
ERROR_WORKFLOW_ID = "YOUR_WORKFLOW_ID"  # existing shared Error Handler

# Real OAuth2 (not an App Password shortcut like the IMAP credential) - see
# PROJECT.md for the full setup saga: bare-IP redirect rejected by Google,
# nip.io wildcard DNS as a first workaround, then a real owned domain
# (n8n.example.com, a new A record pointing at PiLab's LAN IP) once the
# nip.io route kept hitting n8n's own login-session/cookie binding on a
# mismatched host. n8n's own WEBHOOK_URL/N8N_EDITOR_BASE_URL had to be set
# to match (see lab-stack/docker-compose.yml on PiLab), otherwise n8n
# generated its own callback URL from N8N_HOST=0.0.0.0 (its bind address,
# not a reachable one) regardless of what was registered in Google Cloud.
GOOGLE_CALENDAR_CRED = {"id": "YOUR_N8N_CREDENTIAL_ID", "name": "Google Calendar account"}
MQTT_CRED = {"id": "YOUR_N8N_CREDENTIAL_ID", "name": "Mosquitto (PiLab)"}

TIMEZONE = "America/New_York"  # matches MIA/this project's other local-time assumptions


def node_id():
    return str(uuid.uuid4())


# Blue, level-triggered like the NAS-on source, not TTL-based - explicit
# "if I have something for the day it would say so during the whole day,
# if there is nothing then it does not show anything" request. Runs every
# 15 min (same cadence as the weather poll) so a newly-added event shows
# up reasonably promptly, and so the empty-day case gets cleared/re-
# asserted regularly too, not just once at midnight.
build_message_js = """
// "Get Today's Events" upstream has alwaysOutputData set so a genuinely
// empty day (0 real Google Calendar events) still reaches this node
// instead of the whole chain silently halting - see that node's own
// comment. On that path it emits one placeholder item (`{}`) instead of
// zero, which a real event never looks like (every real Google Calendar
// event has an `id`) - filtered out here before anything else runs.
const rawEvents = $input.all().map((item) => item.json).filter((ev) => ev && ev.id);

// Only show a timed event during its own start-to-end window, not any
// time beforehand once it's on the calendar and not after it's over -
// explicit "it should only appear for the time of the appointment...
// after that it disappears" request. All-day events have no meaningful
// start/end moment within the day (no *.dateTime, only *.date), so
// they're left alone as before - shown for the whole day, already
// bounded by the day-boundary query above.
function timedStartMs(ev) {
  return ev.start?.dateTime ? new Date(ev.start.dateTime).getTime() : null;
}
function endTime(ev) {
  return ev.end?.dateTime ? new Date(ev.end.dateTime).getTime() : null;
}
const now = Date.now();
const events = rawEvents.filter((ev) => {
  const start = timedStartMs(ev);
  if (start !== null && start > now) {
    return false;
  }
  const end = endTime(ev);
  return end === null || end > now;
});

if (events.length === 0) {
  return [{ json: { source: 'calendar', message: '', severity: 'blue', ttl_min: 0 } }];
}

// Sorted by start time so "first" is actually chronologically first, not
// just Google's own return order.
function startTime(ev) {
  const raw = ev.start?.dateTime || ev.start?.date;
  return raw ? new Date(raw).getTime() : 0;
}
events.sort((a, b) => startTime(a) - startTime(b));

function formatTime(ev) {
  return new Date(ev.start.dateTime).toLocaleTimeString('en-US', {
    hour: 'numeric',
    minute: '2-digit',
    timeZone: '""" + TIMEZONE + """',
  });
}

// All-day events just show the title, no "All day" prefix - explicit
// request 2026-09-15, simpler than either "All day Progressive" or
// "All day: Progressive".
function lineFor(ev) {
  const title = ev.summary || '(No title)';
  return ev.start?.dateTime ? `${formatTime(ev)} ${title}` : title;
}

// More than one event today: a real ticker instead of the old static
// "first event (+N more)" summary, which hid every event past the first -
// explicit "I think this should run as a ticker" request. All events
// joined onto one line with a bullet separator; the firmware's strip
// (LV_LABEL_LONG_MODE_SCROLL_CIRCULAR, see home_screen.c) scrolls this
// automatically and continuously whenever it doesn't fit, and just sits
// there statically otherwise - a single event is short enough it never
// scrolls. Capped at MAX_TICKER_EVENTS so the joined string can't
// outgrow the firmware's fixed message buffer (NOTIFICATION_MESSAGE_MAX_
// LEN in home_screen.c) - a day busier than that folds the rest into one
// final "+N more" entry, same idea as before just pushed further out.
const MAX_TICKER_EVENTS = 4;
// Plain ASCII, not a bullet - this project's bundled Montserrat fonts
// (see sdkconfig) only cover the ASCII range, so a "•" would draw
// as a blank/missing glyph on-device.
const SEPARATOR = '   |   ';
let message;
if (events.length === 1) {
  message = lineFor(events[0]);
} else if (events.length <= MAX_TICKER_EVENTS) {
  message = events.map(lineFor).join(SEPARATOR);
} else {
  const shown = events.slice(0, MAX_TICKER_EVENTS - 1).map(lineFor);
  shown.push(`+${events.length - (MAX_TICKER_EVENTS - 1)} more`);
  message = shown.join(SEPARATOR);
}

return [{ json: { source: 'calendar', message, severity: 'blue', ttl_min: 0 } }];
""".strip()

nodes = [
    {
        # 15 min -> 30s, explicit live request 2026-09-16 ("if there is
        # anything on the calendar it should appear within 30 seconds") -
        # a genuinely new event added to today's calendar could otherwise
        # take up to 15 minutes to ever show up on the strip. Google
        # Calendar's own read quota is generous enough for this (this
        # project already polls Spotify every 15s with no issue) - the
        # 15-minute interval was never a rate-limit necessity, just the
        # original default.
        "id": node_id(),
        "name": "Every 30s",
        "type": "n8n-nodes-base.scheduleTrigger",
        "typeVersion": 1.3,
        "position": [0, 0],
        "parameters": {"rule": {"interval": [{"field": "seconds", "secondsInterval": 30}]}},
    },
    {
        # Explicit start/end-of-day expressions (Luxon, via n8n's $today)
        # rather than relying on the node's own "today" shortcut, if it
        # has one - keeps the exact timezone explicit and matches this
        # project's own pattern of computing values in a Code node ahead
        # of the node that consumes them (see e.g. "Build NAS Temp
        # Query" in rebuild_health_check_workflow.py).
        "id": node_id(),
        "name": "Compute Today Range",
        "type": "n8n-nodes-base.code",
        "typeVersion": 2,
        "position": [220, 0],
        "parameters": {
            "language": "javaScript",
            "jsCode": (
                f"const tz = '{TIMEZONE}';\n"
                "const now = DateTime.now().setZone(tz);\n"
                "return [{ json: { timeMin: now.startOf('day').toISO(), timeMax: now.endOf('day').toISO() } }];"
            ),
        },
    },
    {
        # alwaysOutputData - real bug found live 2026-09-18: when this
        # node's own Google Calendar query genuinely finds zero events,
        # it naturally emits zero items, and n8n then skips every
        # downstream node entirely for the rest of this run (a node fed
        # zero items by its sole producer just never executes) - so
        # "Build Notification"'s already-correct "events.length === 0 ->
        # publish the empty clear message" branch never actually ran, and
        # Publish Notification never ran either. Confirmed via the live
        # execution log (lastNodeExecuted stuck at this node,
        # "Build Notification"/"Publish Notification" absent from runData
        # on every recent run) and by the exact same fix already proven
        # here for "Fetch MIA FIDS"/"Fetch FA Usage" in
        # rebuild_aircraft_poll_workflow.py - alwaysOutputData belongs on
        # the node whose OWN result can legitimately be empty, not on the
        # node consuming it (tried that first, still starved downstream -
        # this is what actually fixed it). When it fires, this node emits
        # one placeholder item (`{}`, no real event fields) instead of
        # zero - "Build Notification" below filters that out by requiring
        # a real `id` field before treating anything as an actual event.
        "id": node_id(),
        "name": "Get Today's Events",
        "type": "n8n-nodes-base.googleCalendar",
        "typeVersion": 1.3,
        "position": [440, 0],
        "parameters": {
            "resource": "event",
            "operation": "getAll",
            "calendar": {"__rl": True, "mode": "list", "value": "primary"},
            "timeMin": "={{ $json.timeMin }}",
            "timeMax": "={{ $json.timeMax }}",
            "options": {},
            "returnAll": True,
        },
        "credentials": {"googleCalendarOAuth2Api": GOOGLE_CALENDAR_CRED},
        "onError": "continueRegularOutput",
        "alwaysOutputData": True,
    },
    {
        "id": node_id(),
        "name": "Build Notification",
        "type": "n8n-nodes-base.code",
        "typeVersion": 2,
        "position": [660, 0],
        "parameters": {"language": "javaScript", "jsCode": build_message_js},
    },
    {
        "id": node_id(),
        "name": "Publish Notification",
        "type": "n8n-nodes-base.mqtt",
        "typeVersion": 1,
        "position": [880, 0],
        "parameters": {"topic": "homeops/notification/state", "sendInputData": True, "options": {"retain": True}},
        "credentials": {"mqtt": MQTT_CRED},
    },
]

connections = {
    "Every 30s": {"main": [[{"node": "Compute Today Range", "type": "main", "index": 0}]]},
    "Compute Today Range": {"main": [[{"node": "Get Today's Events", "type": "main", "index": 0}]]},
    "Get Today's Events": {"main": [[{"node": "Build Notification", "type": "main", "index": 0}]]},
    "Build Notification": {"main": [[{"node": "Publish Notification", "type": "main", "index": 0}]]},
}

workflow = {
    "name": "Home Ops - Calendar Check",
    "nodes": nodes,
    "connections": connections,
    "settings": {
        "executionOrder": "v1",
        "errorWorkflow": ERROR_WORKFLOW_ID,
        "callerPolicy": "workflowsFromSameOwner",
    },
}

WF = open(os.path.expanduser("~/home-ops-dashboard/n8n/calendar_check_workflow_id.txt")).read().strip()

req = urllib.request.Request(
    f"{N8N_URL}/api/v1/workflows/{WF}",
    data=json.dumps(workflow).encode(),
    headers={"X-N8N-API-KEY": API_KEY, "Content-Type": "application/json"},
    method="PUT",
)

with urllib.request.urlopen(req) as resp:
    result = json.load(resp)
    print("Updated. Active:", result["active"])
