import json
import os
import uuid

import urllib.request

N8N_URL = "http://pilab.local:5678"
API_KEY = open(os.path.expanduser("~/home-ops-dashboard/n8n/.n8n_api_key")).read().strip()
FLIGHTAWARE_KEY = open(os.path.expanduser("~/home-ops-dashboard/n8n/.flightaware_api_key")).read().strip()
POSTGRES_CRED = {"id": "YOUR_N8N_CREDENTIAL_ID", "name": "Postgres account"}
ERROR_WORKFLOW_ID = "YOUR_WORKFLOW_ID"

WF_ID_FILE = os.path.expanduser("~/home-ops-dashboard/n8n/mia_fids_poll_workflow_id.txt")

# MIA's own public flight information display feed - no API key, no
# per-query cost. Confirmed live 2026-09-12: returns every scheduled
# arrival/departure (~200+ each) as XML, keyed by 2-letter IATA airline
# code (CXR) + bare flight number (TRN), including the OTHER airport's
# IATA code (CTY) and aircraft registration (REG). Added specifically to
# cut FlightAware AeroAPI usage - most of this project's office-window
# traffic is ordinary MIA-bound/outbound commercial flights that this free
# feed already covers, reserving the paid FlightAware call for what this
# feed can't answer (cargo, private/GA tail numbers, non-MIA traffic just
# passing through the corridor).
MIA_ARRIVALS_URL = "https://webvids.miami-airport.com/webfids/webfids?action=updateArrivals"
MIA_DEPARTURES_URL = "https://webvids.miami-airport.com/webfids/webfids?action=updateDepartures"


def node_id():
    return str(uuid.uuid4())


parse_fids_js = """
// Regex-based extraction, not a real XML parser - n8n's Code node sandbox
// has no DOMParser/xml2js available, and this feed's structure is simple
// and flat enough (one <flight>...</flight> block per entry, no nesting
// depth beyond the optional <codeShares> block this doesn't need) that
// regex is the pragmatic choice, same approach already used elsewhere in
// this project for the NWS ISO-timestamp extraction.
function extractTag(block, tag) {
  const m = block.match(new RegExp('<' + tag + '>([\\\\s\\\\S]*?)</' + tag + '>'));
  return m ? m[1] : null;
}

function parseFeed(xml, out) {
  if (typeof xml !== 'string') return;
  const blocks = xml.match(/<flight>[\\s\\S]*?<\\/flight>/g) || [];
  for (const block of blocks) {
    const cxr = extractTag(block, 'CXR');
    const trn = extractTag(block, 'TRN');
    const cty = extractTag(block, 'CTY');
    const airlineName = extractTag(block, 'airlineName');
    const dir = extractTag(block, 'DIR'); // 'A' (arrival) or 'D' (departure)
    if (!cxr || !trn || !cty || !dir) continue;
    // MIA is always one side of every entry in this feed (it's MIA's own
    // board) - arrivals land AT MIA (cty is the origin), departures leave
    // FROM MIA (cty is the destination).
    out[cxr + trn] = {
      airline: airlineName || null,
      origin_iata: dir === 'A' ? cty : 'MIA',
      destination_iata: dir === 'A' ? 'MIA' : cty,
    };
  }
}

// n8n's HTTP Request node lands a non-JSON (text/xml) response body under
// `.data` - confirmed the same way this project's NWS integration found
// (documented in PROJECT.md: "the response came back as an unparsed
// string under a `data` key").
const arrivalsResp = $('Fetch Arrivals').first().json;
const departuresResp = $('Fetch Departures').first().json;

// Real bug found 2026-09-13, live in execution history: a timed-out fetch
// (onError: continueRegularOutput on both HTTP nodes) lands here as
// {error, details}, not {data: xml} - typeof-guarding inside parseFeed()
// silently treated that as "zero flights on this side" instead of "this
// side's fetch failed". Confirmed via execution history: table count
// dropped to 371/377/381 (one side timed out) and to 0 (both sides timed
// out) four times across a single afternoon, each after a plain "timeout
// of 5000ms exceeded" against MIA's own site.
//
// Original fix here was a full-replace guard (skip the Postgres write
// entirely if either side errored). Better fix, explicit suggestion from
// the user 2026-09-13: MIA's feed is never a delta - every poll already
// returns the full board (confirmed live: arrivals alone spans ~09-12
// 09:15 through 09-14 00:15, a rolling ~38h window every single call), so
// there's no reason this needs to be a full-replace at all. This node now
// just builds whatever it can from whichever side(s) actually succeeded;
// the write itself (pg_query below) merges onto the existing pool via
// jsonb `||` rather than overwriting it, so a failed or partial fetch
// simply contributes nothing new instead of erasing what's already known
// - no separate "both sides failed" special case needed except to skip a
// pointless no-op write.
const arrivalsOk = !arrivalsResp.error && typeof arrivalsResp.data === 'string';
const departuresOk = !departuresResp.error && typeof departuresResp.data === 'string';
if (!arrivalsOk && !departuresOk) {
  return [];
}

const table = {};
if (arrivalsOk) parseFeed(arrivalsResp.data, table);
if (departuresOk) parseFeed(departuresResp.data, table);

return [{
  json: {
    table,
    count: Object.keys(table).length,
    updated_at: new Date().toISOString(),
  },
}];
""".strip()

# Merge-on-write, not replace-on-write (2026-09-13, see "Parse FIDS" above
# for why): COALESCE the existing row's `table` (empty jsonb if this is the
# very first poll ever), `||`-merge this cycle's freshly-parsed entries on
# top (jsonb `||` is a shallow merge, right-hand side wins per key - so a
# route that changed shows the new value, a flight number MIA dropped from
# its own board just keeps its last-known value instead of disappearing),
# then rebuild the stored value with an accurate `count` against the
# MERGED table, not just this cycle's fetch size. Values are passed as
# query parameters ($1/$2), so an airline/city name can contain any character.
pg_query = (
    "WITH merged AS ("
    "  SELECT COALESCE("
    "    (SELECT value->'table' FROM homeops.state_cache WHERE key = 'mia_fids'),"
    "    '{}'::jsonb"
    "  ) || $1::jsonb AS tbl"
    ") "
    "INSERT INTO homeops.state_cache (key, value) "
    "SELECT 'mia_fids', jsonb_build_object("
    "  'table', tbl,"
    "  'count', (SELECT count(*) FROM jsonb_object_keys(tbl)),"
    "  'updated_at', $2::text"
    ") "
    "FROM merged "
    "ON CONFLICT (key) DO UPDATE SET value = EXCLUDED.value, updated_at = now();"
)

# FlightAware's own free, no-cost usage endpoint (confirmed live
# 2026-09-12: GET /account/usage, matches the AeroAPI web dashboard
# exactly) - folded into this same 10-min poll rather than standing up a
# yet another workflow. Feeds the on-device usage display (see
# aircraft_mqtt.c / systems_screen.c) with FlightAware's own authoritative
# numbers instead of trusting the aircraft-poll workflow's own internal
# call counter.
# Values go in as Postgres query parameters ($1, $2 via options.queryReplacement),
# never pasted into the SQL text - 2026-09-24: an aircraft owner named
# "L'EAGLE AIR II INC" broke the old '{{ JSON.stringify($json) }}' literal
# (apostrophe ended the string -> syntax error). Parameters accept any character.
fa_usage_pg_query = (
    "INSERT INTO homeops.state_cache (key, value) "
    "VALUES ('fa_usage', $1::jsonb) "
    "ON CONFLICT (key) DO UPDATE SET value = EXCLUDED.value, updated_at = now();"
)

# Bumped from 5000ms 2026-09-13 - MIA's own site regularly exceeded the
# original 5s timeout (confirmed via execution history: "timeout of 5000ms
# exceeded" AxiosErrors on both Fetch Arrivals/Fetch Departures, repeatedly,
# not a one-off), which is what fed the empty/truncated-table bug fixed in
# parse_fids_js above. Still bounded, just more realistic for this feed.
text_response_options = {
    "timeout": 15000,
    "response": {"response": {"responseFormat": "text"}},
}

nodes = [
    {
        "id": node_id(), "name": "Every 10 min", "type": "n8n-nodes-base.scheduleTrigger",
        "typeVersion": 1.3, "position": [0, 0],
        "parameters": {"rule": {"interval": [{"field": "minutes", "minutesInterval": 10}]}},
    },
    {
        "id": node_id(), "name": "Fetch Arrivals", "type": "n8n-nodes-base.httpRequest",
        "typeVersion": 4.5, "position": [220, 0], "onError": "continueRegularOutput",
        "parameters": {"url": MIA_ARRIVALS_URL, "options": text_response_options},
    },
    {
        "id": node_id(), "name": "Fetch Departures", "type": "n8n-nodes-base.httpRequest",
        "typeVersion": 4.5, "position": [440, 0], "onError": "continueRegularOutput",
        "parameters": {"url": MIA_DEPARTURES_URL, "options": text_response_options},
    },
    {
        # Chained after "Fetch Departures" (not parallel) for the same
        # execution-order reason documented on the connections below -
        # this node's own output doesn't depend on Departures' data, it's
        # purely to keep every fetch in this workflow sequential.
        "id": node_id(), "name": "Fetch FA Usage", "type": "n8n-nodes-base.httpRequest",
        "typeVersion": 4.5, "position": [660, 0], "onError": "continueRegularOutput",
        "parameters": {
            "url": "https://aeroapi.flightaware.com/aeroapi/account/usage",
            "sendHeaders": True,
            "headerParameters": {"parameters": [{"name": "x-apikey", "value": FLIGHTAWARE_KEY}]},
            "options": {"timeout": 5000},
        },
    },
    {
        "id": node_id(), "name": "Cache FA Usage", "type": "n8n-nodes-base.postgres",
        "typeVersion": 2.6, "position": [880, 120],
        "parameters": {"operation": "executeQuery", "query": fa_usage_pg_query, "options": {"queryReplacement": "={{ [JSON.stringify($json)] }}"}},
        "credentials": {"postgres": POSTGRES_CRED},
    },
    {
        "id": node_id(), "name": "Parse FIDS", "type": "n8n-nodes-base.code",
        "typeVersion": 2, "position": [880, -60],
        "parameters": {"language": "javaScript", "jsCode": parse_fids_js},
    },
    {
        "id": node_id(), "name": "Cache MIA FIDS", "type": "n8n-nodes-base.postgres",
        "typeVersion": 2.6, "position": [1100, -60],
        "parameters": {"operation": "executeQuery", "query": pg_query, "options": {"queryReplacement": "={{ [JSON.stringify($json.table), $json.updated_at ?? null] }}"}},
        "credentials": {"postgres": POSTGRES_CRED},
    },
]

connections = {
    # Real bug found via a live execution error: firing both HTTP fetches
    # in parallel into the same input index on "Parse FIDS" made it run
    # TWICE (once per branch, whichever completed first) instead of once
    # with both available - the earlier run's $('Fetch Departures')
    # cross-reference failed with "hasn't been executed" since that
    # branch genuinely hadn't finished yet. Chaining them sequentially
    # (Departures' fetch doesn't depend on Arrivals' data, this is purely
    # for execution ordering) guarantees both have real data by the time
    # "Parse FIDS" reads either via $().
    "Every 10 min": {"main": [[{"node": "Fetch Arrivals", "type": "main", "index": 0}]]},
    "Fetch Arrivals": {"main": [[{"node": "Fetch Departures", "type": "main", "index": 0}]]},
    "Fetch Departures": {"main": [[{"node": "Fetch FA Usage", "type": "main", "index": 0}]]},
    "Fetch FA Usage": {
        "main": [[
            {"node": "Cache FA Usage", "type": "main", "index": 0},
            {"node": "Parse FIDS", "type": "main", "index": 0},
        ]]
    },
    "Parse FIDS": {"main": [[{"node": "Cache MIA FIDS", "type": "main", "index": 0}]]},
}

workflow = {
    "name": "Home Ops - MIA FIDS Poll",
    "nodes": nodes,
    "connections": connections,
    "settings": {
        "executionOrder": "v1",
        "errorWorkflow": ERROR_WORKFLOW_ID,
        "callerPolicy": "workflowsFromSameOwner",
    },
}

if os.path.exists(WF_ID_FILE):
    wf_id = open(WF_ID_FILE).read().strip()
    req = urllib.request.Request(
        f"{N8N_URL}/api/v1/workflows/{wf_id}",
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
    wf_id = result["id"]
    open(WF_ID_FILE, "w").write(wf_id)
    print("Created workflow", wf_id)

    activate_req = urllib.request.Request(
        f"{N8N_URL}/api/v1/workflows/{wf_id}/activate",
        headers={"X-N8N-API-KEY": API_KEY, "Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(activate_req) as resp:
        result = json.load(resp)
        print("Activated. Active:", result["active"])
