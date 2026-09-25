import json
import os
import uuid

import urllib.request

N8N_URL = "https://n8n.example.com"
API_KEY = open(os.path.expanduser("~/home-ops-dashboard/n8n/.n8n_api_key")).read().strip()
ERROR_WORKFLOW_ID = "YOUR_WORKFLOW_ID"  # existing shared Error Handler

SPOTIFY_CRED = {"id": "YOUR_N8N_CREDENTIAL_ID", "name": "Spotify account"}
MQTT_CRED = {"id": "YOUR_N8N_CREDENTIAL_ID", "name": "Mosquitto (PiLab)"}

# Raw HTTP Request nodes against api.spotify.com, not the built-in Spotify
# node - that node has no device_id parameter at all on any player
# operation (checked its source directly), so it can only ever act on
# "whatever Spotify already considers active", not specifically the Bose
# soundbar. Same predefinedCredentialType/spotifyOAuth2Api mechanism
# either way - just full control over query params this way.
build_notification_js = """
const resp = $input.first().json;
// A 204 No Content (nothing playing) comes back here as an empty/
// undefined body, not an httpRequest error - onError continueRegularOutput
// on the fetch node is a backstop for a genuine network/auth failure, not
// the normal "nothing playing" case.
const item = resp && resp.item;
const isPlaying = !!(resp && resp.is_playing);

if (!isPlaying || !item) {
  return [{ json: { source: 'spotify', message: '', severity: 'spotify', ttl_min: 0 } }];
}

const track = item.name || 'Unknown track';
const artists = (item.artists || []).map((a) => a.name).join(', ') || 'Unknown artist';
// "|" separator for a clear title/artist distinction - explicit request
// 2026-09-16. True bold-for-the-title-only isn't feasible here without a
// second custom font plus splitting the strip into multiple widgets,
// which would also break the single-label auto-scroll this strip uses
// (see LV_LABEL_LONG_MODE_SCROLL_CIRCULAR in home_screen.c) - a real
// tradeoff, not an oversight.

return [{ json: { source: 'spotify', message: `${track} | ${artists}`, severity: 'spotify', ttl_min: 0 } }];
""".strip()

build_state_js = """
const resp = $input.first().json;
const item = resp && resp.item;
const isPlaying = !!(resp && resp.is_playing);
// device (and its volume) can be present even while paused - a device
// stays "active" in Spotify Connect independent of playback state, so
// this is read the same way in both branches below rather than only
// when isPlaying. -1 (not null) when unknown, since the firmware side
// treats this as a plain int, not something JSON-null-aware.
const device = resp && resp.device;
const volumePercent = device && typeof device.volume_percent === 'number' ? device.volume_percent : -1;

// Real Spotify API quirk, confirmed live 2026-09-16 by inspecting a raw
// poll response: this account's Bose soundbar reports device.name
// IDENTICAL to device.id (a long hex string, e.g.
// "490928b9605cb8f0b8137fefb18ebedb42b8f568") on the currently-playing
// endpoint - not missing, not null, just genuinely not a human name.
// Confirmed this is endpoint-specific, not account-wide: the control
// workflow's own device list lookup (build_spotify_control_workflow.py,
// GET /v1/me/player/devices) matches this exact device by a working
// /bose/i name regex, so /v1/me/player/devices DOES get a proper name
// for it - just not this endpoint. Rather than add a second API call to
// every poll just to re-resolve a name this project already knows,
// fall back to a hardcoded friendly name whenever device.name looks like
// a raw ID (equals device.id, or is a long hex blob) - this setup only
// has the one Bluetooth/Connect-bridged Speaker-type device that hits
// this quirk; a real future device with normal metadata passes through
// untouched. Firmware side (spotify_screen.c) shows this verbatim as
// "Playing on <device>" - explains the live "a lot of numbers and
// letters" report before this fix.
function resolveDeviceName(d) {
  if (!d) return '';
  const looksLikeRawId = d.name === d.id || /^[0-9a-f]{20,}$/i.test(d.name || '');
  if (looksLikeRawId) {
    return d.type === 'Speaker' ? 'Bose Smart Soundbar' : (d.type || 'Speaker');
  }
  return d.name || '';
}
const deviceName = resolveDeviceName(device);

if (!isPlaying || !item) {
  return [{ json: {
    is_playing: false, track: '', artist: '', album: '',
    device: deviceName, volume_percent: volumePercent,
    progress_ms: 0, duration_ms: 0, updated_at: new Date().toISOString(),
  } }];
}

return [{ json: {
  is_playing: true,
  track: item.name || '',
  artist: (item.artists || []).map((a) => a.name).join(', '),
  album: item.album ? item.album.name : '',
  device: deviceName,
  volume_percent: volumePercent,
  progress_ms: resp.progress_ms || 0,
  duration_ms: item.duration_ms || 0,
  updated_at: new Date().toISOString(),
} }];
""".strip()


def node_id():
    return str(uuid.uuid4())


nodes = [
    {
        "id": node_id(),
        "name": "Every 5s",
        "type": "n8n-nodes-base.scheduleTrigger",
        "typeVersion": 1.3,
        "position": [0, 0],
        "parameters": {"rule": {"interval": [{"field": "seconds", "secondsInterval": 5}]}},
    },
    {
        "id": node_id(),
        "name": "Get Currently Playing",
        "type": "n8n-nodes-base.httpRequest",
        "typeVersion": 4.5,
        "position": [220, 0],
        "onError": "continueRegularOutput",
        "parameters": {
            # /v1/me/player ("Get Playback State"), not the narrower
            # /v1/me/player/currently-playing - confirmed live 2026-09-16
            # the currently-playing endpoint has no "device" field at all
            # in its response (checked the raw execution data directly,
            # not assumed); /v1/me/player is a strict superset - same
            # track data plus device (needed to show/confirm which
            # device, e.g. the Bose soundbar, is actually playing).
            "url": "https://api.spotify.com/v1/me/player",
            "authentication": "predefinedCredentialType",
            "nodeCredentialType": "spotifyOAuth2Api",
            "options": {"timeout": 5000},
        },
        "credentials": {"spotifyOAuth2Api": SPOTIFY_CRED},
    },
    {
        "id": node_id(),
        "name": "Build Notification",
        "type": "n8n-nodes-base.code",
        "typeVersion": 2,
        "position": [440, -100],
        "parameters": {"language": "javaScript", "jsCode": build_notification_js},
    },
    {
        "id": node_id(),
        "name": "Publish Notification",
        "type": "n8n-nodes-base.mqtt",
        "typeVersion": 1,
        "position": [660, -100],
        "parameters": {"topic": "homeops/notification/state", "sendInputData": True, "options": {"retain": True}},
        "credentials": {"mqtt": MQTT_CRED},
    },
    {
        "id": node_id(),
        "name": "Build Spotify State",
        "type": "n8n-nodes-base.code",
        "typeVersion": 2,
        "position": [440, 100],
        "parameters": {"language": "javaScript", "jsCode": build_state_js},
    },
    {
        "id": node_id(),
        "name": "Publish Spotify State",
        "type": "n8n-nodes-base.mqtt",
        "typeVersion": 1,
        "position": [660, 100],
        "parameters": {"topic": "homeops/spotify/state", "sendInputData": True, "options": {"retain": True}},
        "credentials": {"mqtt": MQTT_CRED},
    },
]

connections = {
    "Every 5s": {"main": [[{"node": "Get Currently Playing", "type": "main", "index": 0}]]},
    "Get Currently Playing": {
        "main": [
            [
                {"node": "Build Notification", "type": "main", "index": 0},
                {"node": "Build Spotify State", "type": "main", "index": 0},
            ]
        ]
    },
    "Build Notification": {"main": [[{"node": "Publish Notification", "type": "main", "index": 0}]]},
    "Build Spotify State": {"main": [[{"node": "Publish Spotify State", "type": "main", "index": 0}]]},
}

workflow = {
    "name": "Home Ops - Spotify Now Playing",
    "nodes": nodes,
    "connections": connections,
    "settings": {
        "executionOrder": "v1",
        "errorWorkflow": ERROR_WORKFLOW_ID,
        "callerPolicy": "workflowsFromSameOwner",
        # Explicitly "all", not left to default: a PUT merges settings, so
        # omitting the key keeps whatever was there. "none" was tried
        # 2026-09-22 to cut execution-history churn and broke on n8n
        # 2.41 - every successful run stayed "running" in execution_entity
        # forever (never deleted, never pruned). Don't retry without
        # re-testing on a newer n8n first.
        "saveDataSuccessExecution": "all",
    },
}

WF = open(os.path.expanduser("~/home-ops-dashboard/n8n/spotify_now_playing_workflow_id.txt")).read().strip()

req = urllib.request.Request(
    f"{N8N_URL}/api/v1/workflows/{WF}",
    data=json.dumps(workflow).encode(),
    headers={"X-N8N-API-KEY": API_KEY, "Content-Type": "application/json"},
    method="PUT",
)

with urllib.request.urlopen(req) as resp:
    result = json.load(resp)
    print("Updated. Active:", result["active"])
