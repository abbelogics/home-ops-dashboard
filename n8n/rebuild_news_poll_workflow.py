import json
import os
import uuid

import urllib.request

N8N_URL = "http://pilab.local:5678"
API_KEY = open(os.path.expanduser("~/home-ops-dashboard/n8n/.n8n_api_key")).read().strip()
MQTT_CRED = {"id": "YOUR_N8N_CREDENTIAL_ID", "name": "Mosquitto (PiLab)"}
ERROR_WORKFLOW_ID = "YOUR_WORKFLOW_ID"  # existing shared Error Handler

# Colombian news - El Tiempo's "colombia" section feed, not their
# "ultimas-noticias" ("latest news") feed originally used here. Switched
# 2026-09-15, explicit request after the user noticed stale headlines:
# confirmed live, ultimas-noticias.xml's own <item> pubDates were wildly
# stale (Aug 18/Jun 18/Jun 10 despite the feed's own channel-level
# pubDate being today) while colombia.xml's items were all within the
# last 2 hours - a real bug/staleness on El Tiempo's side for that
# specific feed, not a topic-focus preference. colombia.xml is also
# narrower/more substantive (actual national news vs. the general feed's
# lifestyle/fashion filler mixed in), a second, independent reason to
# prefer it.
COLOMBIA_RSS_URL = "https://www.eltiempo.com/rss/colombia.xml"

# F1 news - Formula1.com's own official feed, not Motorsport.com (a
# third-party site) originally used here. Switched 2026-09-15, explicit
# request - confirmed live via curl, real working feed with current
# items, and being the official source is a better fit than a third
# party's own coverage.
F1_RSS_URL = "https://www.formula1.com/en/latest/all.xml"

# Set when updating an already-deployed workflow in place (see
# news_poll_workflow_id.txt) rather than creating a new one - PUT instead of
# POST. Left None to create fresh.
EXISTING_WORKFLOW_ID = "YOUR_WORKFLOW_ID"


def node_id():
    return str(uuid.uuid4())


# Shared by both "Format <source>" nodes below - only the prefix/count
# differ, kept as one template rather than two near-duplicate blobs.
def format_js(prefix, max_items):
    return f"""
// Bundled Montserrat font has no accented-Latin glyphs (confirmed live on
// this exact ticker before - see home_screen.c's own comments on
// home_screen_set_aircraft/the news ticker) - strip accents to plain
// ASCII here rather than have them render as tofu boxes on the device.
function toAscii(s) {{
  return String(s || '')
    .normalize('NFD')
    .replace(/[\\u0300-\\u036f]/g, '')
    .replace(/[^\\x20-\\x7E]/g, '')
    .trim();
}}

const items = $input.all().slice(0, {max_items});
const titles = items.map((item) => {{
  let t = toAscii(item.json.title);
  // Safety cap only for a pathologically long headline, not a routine
  // truncation - cuts at the last word boundary under the cap, never
  // mid-word (explicit complaint 2026-09-15: "captura de Jua..." read as
  // broken/incomplete, not just long). The firmware's own scroll speed
  // now scales with actual text width (see home_screen.c), so there's no
  // need to keep headlines short just to keep the ticker feeling fast.
  if (t.length > 110) {{
    const cut = t.slice(0, 110);
    const lastSpace = cut.lastIndexOf(' ');
    t = (lastSpace > 60 ? cut.slice(0, lastSpace) : cut) + '...';
  }}
  return '{prefix}: ' + t;
}});

return [{{json: {{piece: titles.join('   |   ')}}}}];
""".strip()


combine_js = """
const pieces = $input.all().map((item) => item.json.piece).filter(Boolean);

// Trailing spaces give the scrolling ticker (LV_LABEL_LONG_SCROLL_CIRCULAR)
// a gap before it loops back to the start - same trick the firmware's own
// placeholder text already used (see home_screen.c).
return [{
  json: {
    headline: pieces.join('   //   ') + '        ',
    updated_at: new Date().toISOString(),
  },
}];
""".strip()

nodes = [
    {
        "id": node_id(),
        "name": "Every 15 min",
        "type": "n8n-nodes-base.scheduleTrigger",
        "typeVersion": 1.3,
        "position": [0, 0],
        "parameters": {"rule": {"interval": [{"field": "minutes", "minutesInterval": 15}]}},
    },
    {
        "id": node_id(),
        "name": "Fetch Colombia News",
        "type": "n8n-nodes-base.rssFeedRead",
        "typeVersion": 1.1,
        "position": [220, -100],
        "parameters": {"url": COLOMBIA_RSS_URL},
    },
    {
        "id": node_id(),
        "name": "Format Colombia",
        "type": "n8n-nodes-base.code",
        "typeVersion": 2,
        "position": [440, -100],
        "parameters": {"language": "javaScript", "jsCode": format_js("COL", 4)},
    },
    {
        "id": node_id(),
        "name": "Fetch F1 News",
        "type": "n8n-nodes-base.rssFeedRead",
        "typeVersion": 1.1,
        "position": [220, 100],
        "parameters": {"url": F1_RSS_URL},
    },
    {
        "id": node_id(),
        "name": "Format F1",
        "type": "n8n-nodes-base.code",
        "typeVersion": 2,
        "position": [440, 100],
        "parameters": {"language": "javaScript", "jsCode": format_js("F1", 4)},
    },
    {
        "id": node_id(),
        "name": "Merge",
        "type": "n8n-nodes-base.merge",
        "typeVersion": 3,
        "position": [660, 0],
        "parameters": {"mode": "append"},
    },
    {
        "id": node_id(),
        "name": "Combine Ticker",
        "type": "n8n-nodes-base.code",
        "typeVersion": 2,
        "position": [880, 0],
        "parameters": {"language": "javaScript", "jsCode": combine_js},
    },
    {
        "id": node_id(),
        "name": "Publish News State",
        "type": "n8n-nodes-base.mqtt",
        "typeVersion": 1,
        "position": [1100, 0],
        "parameters": {"topic": "homeops/news/state", "sendInputData": True, "options": {"retain": True}},
        "credentials": {"mqtt": MQTT_CRED},
    },
]

connections = {
    "Every 15 min": {
        "main": [
            [
                {"node": "Fetch Colombia News", "type": "main", "index": 0},
                {"node": "Fetch F1 News", "type": "main", "index": 0},
            ]
        ]
    },
    "Fetch Colombia News": {"main": [[{"node": "Format Colombia", "type": "main", "index": 0}]]},
    "Fetch F1 News": {"main": [[{"node": "Format F1", "type": "main", "index": 0}]]},
    # Two different nodes both targeting the SAME input index on one node
    # does NOT behave like an implicit append merge - confirmed the hard
    # way live (only the second branch's data ever made it through,
    # Colombia's silently vanished even though "Format Colombia" itself
    # produced a correct item). A real Merge node with distinct input
    # indices (0/1) per branch is the actual correct n8n pattern - matches
    # how Home Ops - Aircraft Poll's own "Merge" node is wired.
    "Format Colombia": {"main": [[{"node": "Merge", "type": "main", "index": 0}]]},
    "Format F1": {"main": [[{"node": "Merge", "type": "main", "index": 1}]]},
    "Merge": {"main": [[{"node": "Combine Ticker", "type": "main", "index": 0}]]},
    "Combine Ticker": {"main": [[{"node": "Publish News State", "type": "main", "index": 0}]]},
}

workflow = {
    "name": "Home Ops - News Poll",
    "nodes": nodes,
    "connections": connections,
    "settings": {
        "executionOrder": "v1",
        "errorWorkflow": ERROR_WORKFLOW_ID,
        "callerPolicy": "workflowsFromSameOwner",
    },
}

if EXISTING_WORKFLOW_ID:
    update_payload = {
        "name": workflow["name"],
        "nodes": workflow["nodes"],
        "connections": workflow["connections"],
        "settings": workflow["settings"],
    }
    req = urllib.request.Request(
        f"{N8N_URL}/api/v1/workflows/{EXISTING_WORKFLOW_ID}",
        data=json.dumps(update_payload).encode(),
        headers={"X-N8N-API-KEY": API_KEY, "Content-Type": "application/json"},
        method="PUT",
    )
    with urllib.request.urlopen(req) as resp:
        result = json.load(resp)
        print("Updated. ID:", result["id"], "active:", result.get("active"))
else:
    req = urllib.request.Request(
        f"{N8N_URL}/api/v1/workflows",
        data=json.dumps(workflow).encode(),
        headers={"X-N8N-API-KEY": API_KEY, "Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(req) as resp:
        result = json.load(resp)
        print("Created. ID:", result["id"])
