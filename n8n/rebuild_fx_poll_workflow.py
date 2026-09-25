"""Home Ops - FX Poll (rebuilt 2026-09-24, explicit request).

1 USD against COP, EUR and GBP for the BOX-3's FX screen, from two
official daily reference sources (explicit choice, replacing the old
open.er-api.com aggregator - whose COP was ~2% off the TRM when compared
live):

  COP  TRM (Tasa Representativa del Mercado), Superintendencia Financiera,
       via Colombia's open-data API (datos.gov.co dataset 32sa-8pi3).
       One row per publication; a Friday row covers the weekend/holiday
       Monday (vigenciadesde..vigenciahasta). Tomorrow's TRM is published
       the afternoon before, so "current" = latest row whose vigenciadesde
       is not after today in Bogota.
  EUR, GBP  European Central Bank reference rates via Frankfurter
       (api.frankfurter.dev, free, no key), USD base. Business days only,
       ~16:00 CET.

"prev" = the previous published rate (not literally yesterday), for the
up/down arrow + difference on the screen. Polled hourly (both sources
change once a day - hourly just catches that promptly); published
RETAINED on homeops/fx/state only when something changed.

The original 2026-09-15 workflow (TfqEpweDeH3aQ4Ri) was archived when the
first FX screen was removed; this script updates it in place if n8n lets
it, otherwise creates a new one and rewrites fx_poll_workflow_id.txt.
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
WF_ID_FILE = os.path.expanduser("~/home-ops-dashboard/n8n/fx_poll_workflow_id.txt")

TRM_URL = "https://www.datos.gov.co/resource/32sa-8pi3.json?$order=vigenciadesde%20DESC&$limit=5"
# 10-day window so a long weekend/holiday still leaves two business days.
ECB_URL = ("={{ 'https://api.frankfurter.dev/v1/' + $now.minus({ days: 10 }).toFormat('yyyy-MM-dd')"
           " + '..?base=USD&symbols=EUR,GBP' }}")


def node_id():
    return str(uuid.uuid4())


build_js = """
const sd = $getWorkflowStaticData('global');

// TRM rows arrive as separate items (JSON array response).
const todayBogota = new Date().toLocaleDateString('en-CA', { timeZone: 'America/Bogota' });
const trmRows = $('Fetch TRM').all().map((i) => i.json)
  .filter((r) => r && r.valor && r.vigenciadesde)
  .sort((a, b) => b.vigenciadesde.localeCompare(a.vigenciadesde));
const curIdx = trmRows.findIndex((r) => r.vigenciadesde.slice(0, 10) <= todayBogota);
const trmCur = curIdx >= 0 ? trmRows[curIdx] : null;
const trmPrev = curIdx >= 0 ? trmRows[curIdx + 1] : null;

const ecbRates = $('Fetch ECB').first().json?.rates || {};
const ecbDates = Object.keys(ecbRates).sort();
const ecbCurDate = ecbDates[ecbDates.length - 1];
const ecbPrevDate = ecbDates[ecbDates.length - 2];

// A failed fetch keeps the last good values for that source rather than
// publishing a hole.
const last = sd.last ? JSON.parse(sd.last) : {};
const out = {
  cop: trmCur ? { rate: Number(trmCur.valor), prev: trmPrev ? Number(trmPrev.valor) : null,
                  date: trmCur.vigenciadesde.slice(0, 10) } : last.cop,
  eur: ecbCurDate ? { rate: ecbRates[ecbCurDate].EUR, prev: ecbPrevDate ? ecbRates[ecbPrevDate].EUR : null,
                      date: ecbCurDate } : last.eur,
  gbp: ecbCurDate ? { rate: ecbRates[ecbCurDate].GBP, prev: ecbPrevDate ? ecbRates[ecbPrevDate].GBP : null,
                      date: ecbCurDate } : last.gbp,
};
if (!out.cop || !out.eur || !out.gbp) return [];

const key = JSON.stringify(out);
if (key === sd.last) return [];
sd.last = key;
return [{ json: out }];
""".strip()

nodes = [
    {
        "id": node_id(), "name": "Every Hour", "type": "n8n-nodes-base.scheduleTrigger",
        "typeVersion": 1.3, "position": [0, 0],
        "parameters": {"rule": {"interval": [{"field": "hours"}]}},
    },
    {
        # alwaysOutputData + continue-on-error: a TRM outage must not stop
        # the ECB half (and vice versa) - the Build node falls back to the
        # last good values per source.
        "id": node_id(), "name": "Fetch TRM", "type": "n8n-nodes-base.httpRequest",
        "typeVersion": 4.5, "position": [220, 0], "onError": "continueRegularOutput", "alwaysOutputData": True,
        "parameters": {"url": TRM_URL, "options": {"timeout": 10000}},
    },
    {
        # executeOnce: TRM emits one item per row; fetch ECB just once.
        "id": node_id(), "name": "Fetch ECB", "type": "n8n-nodes-base.httpRequest",
        "typeVersion": 4.5, "position": [440, 0], "onError": "continueRegularOutput", "alwaysOutputData": True,
        "executeOnce": True,
        "parameters": {"url": ECB_URL, "options": {"timeout": 10000}},
    },
    {
        "id": node_id(), "name": "Build FX State", "type": "n8n-nodes-base.code",
        "typeVersion": 2, "position": [660, 0],
        "parameters": {"language": "javaScript", "jsCode": build_js},
    },
    {
        "id": node_id(), "name": "Publish FX", "type": "n8n-nodes-base.mqtt",
        "typeVersion": 1, "position": [880, 0],
        "parameters": {"topic": "homeops/fx/state", "sendInputData": True, "options": {"retain": True}},
        "credentials": {"mqtt": MQTT_CRED},
    },
]

connections = {
    "Every Hour": {"main": [[{"node": "Fetch TRM", "type": "main", "index": 0}]]},
    "Fetch TRM": {"main": [[{"node": "Fetch ECB", "type": "main", "index": 0}]]},
    "Fetch ECB": {"main": [[{"node": "Build FX State", "type": "main", "index": 0}]]},
    "Build FX State": {"main": [[{"node": "Publish FX", "type": "main", "index": 0}]]},
}

workflow = {
    "name": "Home Ops - FX Poll",
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


wf_id = open(WF_ID_FILE).read().strip() if os.path.exists(WF_ID_FILE) else None
try:
    if not wf_id:
        raise FileNotFoundError
    api(f"/workflows/{wf_id}", "PUT", workflow)
    print("Updated", wf_id)
except (urllib.error.HTTPError, FileNotFoundError) as e:
    detail = e.read().decode()[:200] if isinstance(e, urllib.error.HTTPError) else "no id"
    print("Update not possible (", detail, ") - creating a new workflow")
    wf_id = api("/workflows", "POST", workflow)["id"]
    with open(WF_ID_FILE, "w") as f:
        f.write(wf_id)
    print("Created", wf_id)

print("Active:", api(f"/workflows/{wf_id}/activate", "POST")["active"])
