import json
import os
import uuid

import urllib.request

N8N_URL = "https://n8n.example.com"
API_KEY = open(os.path.expanduser("~/home-ops-dashboard/n8n/.n8n_api_key")).read().strip()
ERROR_WORKFLOW_ID = "YOUR_WORKFLOW_ID"  # existing shared Error Handler

SPOTIFY_CRED = {"id": "YOUR_N8N_CREDENTIAL_ID", "name": "Spotify account"}
MQTT_CRED = {"id": "YOUR_N8N_CREDENTIAL_ID", "name": "Mosquitto (PiLab)"}

# Spotify device_ids aren't stable across reconnects (confirmed by reading
# Spotify's own device docs, same standard as every other undocumented-
# behavior claim in this project - not guessed) - resolved by device NAME
# on every single control call instead of cached, so this keeps working
# after the soundbar drops off Spotify Connect and reappears (e.g. after a
# power cycle). No body needed on any of these calls - device_id/
# volume_percent are query params on Spotify's own API, not JSON bodies.
build_action_js = """
const body = $('On Control Command').first().json.body || {};
const action = body.action;
const devices = $input.first().json.devices || [];

const bose = devices.find((d) => /bose/i.test(d.name || ''));
const deviceId = bose ? bose.id : null;
// Falls back to no device_id (targets whatever Spotify already considers
// active) rather than failing outright if the soundbar isn't currently
// visible in the device list - e.g. Bluetooth not connected yet.
const deviceQuery = deviceId ? `device_id=${encodeURIComponent(deviceId)}` : '';

function buildUrl(base, extraQuery) {
  const parts = [deviceQuery, extraQuery].filter(Boolean);
  return parts.length ? `${base}?${parts.join('&')}` : base;
}

let url = null;
let method = null;
let requestBody = null;

switch (action) {
  case 'play':
    if (deviceId) {
      // Transfer Playback, not the plain resume-in-place /play endpoint -
      // explicit "if I press play it goes straight to the bose" request
      // 2026-09-16. Without this, /play only resumes whatever context is
      // ALREADY active (often the phone, since that's what actually
      // started something), which is exactly the "I need my phone for
      // everything" complaint that prompted this. Transfer Playback both
      // moves playback onto the target device AND (with play:true)
      // resumes its last context there - no separate resume call needed.
      url = 'https://api.spotify.com/v1/me/player';
      method = 'PUT';
      requestBody = JSON.stringify({ device_ids: [deviceId], play: true });
    } else {
      // Bose isn't visible in the device list right now (e.g. asleep/
      // not Bluetooth-connected) - fall back to a plain resume on
      // whatever's already active, same as before.
      url = buildUrl('https://api.spotify.com/v1/me/player/play');
      method = 'PUT';
    }
    break;
  case 'pause':
    url = buildUrl('https://api.spotify.com/v1/me/player/pause');
    method = 'PUT';
    break;
  case 'next':
    url = buildUrl('https://api.spotify.com/v1/me/player/next');
    method = 'POST';
    break;
  case 'previous':
    url = buildUrl('https://api.spotify.com/v1/me/player/previous');
    method = 'POST';
    break;
  case 'volume': {
    let vol = parseInt(body.volume_percent, 10);
    if (isNaN(vol)) vol = 50;
    vol = Math.max(0, Math.min(100, vol));
    url = buildUrl('https://api.spotify.com/v1/me/player/volume', `volume_percent=${vol}`);
    method = 'PUT';
    break;
  }
  default:
    // Unknown action - url stays null, the HTTP node's own onError
    // (continueRegularOutput) makes that a harmless no-op below rather
    // than a workflow failure.
    break;
}

return [{ json: { url, method, body: requestBody, action, device_found: !!bose } }];
""".strip()

# Immediate post-action re-poll + publish, not just fire-and-forget - real
# live report 2026-09-16 ("play and pause take a long time to respond,
# check and see if you can make it the same speed as the lights"). Same
# pattern lights_control already used (see rebuild_lights_control_
# workflow.py's own "Control... then re-poll immediately for fast
# feedback instead of waiting up to 30s"): this workflow used to fire the
# action and stop, leaving spotify_screen.c's play/pause button showing
# "..." until the entirely separate "Home Ops - Spotify Now Playing"
# workflow's own independent 15s timer happened to land - up to ~15s
# (averaging ~7.5s) of visible lag per tap. A short wait before the
# confirmation GET (Spotify's own API needs a beat to reflect a just-made
# change - same "eventual consistency" reasoning the lights workflow's
# post-set re-poll already accounts for, just a different backend)
# avoids reading back stale pre-action state.
build_confirm_state_js = """
const resp = $input.first().json;
const item = resp && resp.item;
const isPlaying = !!(resp && resp.is_playing);
const device = resp && resp.device;
const volumePercent = device && typeof device.volume_percent === 'number' ? device.volume_percent : -1;

// Same real Spotify API quirk as the now-playing poll's own "Build
// Spotify State" node - see rebuild_spotify_now_playing_workflow.py's
// own comment for the full story (this account's Bose soundbar reports
// device.name identical to device.id on this exact endpoint).
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
    # Webhook, not an MQTT Trigger - same reasoning as Home Ops - Lights
    # Control (n8n's MQTT Trigger node failed to activate on this
    # instance, see that workflow's own comment for the full story).
    {
        "id": node_id(),
        "name": "On Control Command",
        "type": "n8n-nodes-base.webhook",
        "typeVersion": 2,
        "position": [0, 0],
        "parameters": {"path": "spotify-control", "httpMethod": "POST", "options": {}},
    },
    {
        "id": node_id(),
        "name": "Get Devices",
        "type": "n8n-nodes-base.httpRequest",
        "typeVersion": 4.5,
        "position": [220, 0],
        "onError": "continueRegularOutput",
        "parameters": {
            "url": "https://api.spotify.com/v1/me/player/devices",
            "authentication": "predefinedCredentialType",
            "nodeCredentialType": "spotifyOAuth2Api",
            "options": {"timeout": 5000},
        },
        "credentials": {"spotifyOAuth2Api": SPOTIFY_CRED},
    },
    {
        "id": node_id(),
        "name": "Build Spotify Action",
        "type": "n8n-nodes-base.code",
        "typeVersion": 2,
        "position": [440, 0],
        "parameters": {"language": "javaScript", "jsCode": build_action_js},
    },
    {
        "id": node_id(),
        "name": "Execute Spotify Action",
        "type": "n8n-nodes-base.httpRequest",
        "typeVersion": 4.5,
        "position": [660, 0],
        "onError": "continueRegularOutput",
        "parameters": {
            "url": "={{ $json.url }}",
            "method": "={{ $json.method }}",
            "authentication": "predefinedCredentialType",
            "nodeCredentialType": "spotifyOAuth2Api",
            # Always sends a body - "{}" for actions that don't need one
            # (pause/next/previous/volume all pass device_id/
            # volume_percent as query params, not JSON), which Spotify's
            # API just ignores on those endpoints. Simpler than making
            # sendBody itself conditional on whether Build Spotify Action
            # actually set a body.
            "sendBody": True,
            "specifyBody": "json",
            "jsonBody": "={{ $json.body || '{}' }}",
            "options": {"timeout": 5000},
        },
        "credentials": {"spotifyOAuth2Api": SPOTIFY_CRED},
    },
    {
        "id": node_id(),
        "name": "Wait Before Confirm",
        "type": "n8n-nodes-base.wait",
        "typeVersion": 1.1,
        "position": [880, 0],
        "parameters": {"unit": "seconds", "amount": 0.6},
    },
    {
        "id": node_id(),
        "name": "Get Current State",
        "type": "n8n-nodes-base.httpRequest",
        "typeVersion": 4.5,
        "position": [1100, 0],
        "onError": "continueRegularOutput",
        "parameters": {
            "url": "https://api.spotify.com/v1/me/player",
            "authentication": "predefinedCredentialType",
            "nodeCredentialType": "spotifyOAuth2Api",
            "options": {"timeout": 5000},
        },
        "credentials": {"spotifyOAuth2Api": SPOTIFY_CRED},
    },
    {
        "id": node_id(),
        "name": "Build Confirm State",
        "type": "n8n-nodes-base.code",
        "typeVersion": 2,
        "position": [1320, 0],
        "parameters": {"language": "javaScript", "jsCode": build_confirm_state_js},
    },
    {
        "id": node_id(),
        "name": "Publish Spotify State (post-action)",
        "type": "n8n-nodes-base.mqtt",
        "typeVersion": 1,
        "position": [1540, 0],
        "parameters": {"topic": "homeops/spotify/state", "sendInputData": True, "options": {"retain": True}},
        "credentials": {"mqtt": MQTT_CRED},
    },
]

connections = {
    "On Control Command": {"main": [[{"node": "Get Devices", "type": "main", "index": 0}]]},
    "Get Devices": {"main": [[{"node": "Build Spotify Action", "type": "main", "index": 0}]]},
    "Build Spotify Action": {"main": [[{"node": "Execute Spotify Action", "type": "main", "index": 0}]]},
    "Execute Spotify Action": {"main": [[{"node": "Wait Before Confirm", "type": "main", "index": 0}]]},
    "Wait Before Confirm": {"main": [[{"node": "Get Current State", "type": "main", "index": 0}]]},
    "Get Current State": {"main": [[{"node": "Build Confirm State", "type": "main", "index": 0}]]},
    "Build Confirm State": {"main": [[{"node": "Publish Spotify State (post-action)", "type": "main", "index": 0}]]},
}

workflow = {
    "name": "Home Ops - Spotify Control",
    "nodes": nodes,
    "connections": connections,
    "settings": {
        "executionOrder": "v1",
        "errorWorkflow": ERROR_WORKFLOW_ID,
        "callerPolicy": "workflowsFromSameOwner",
    },
}

WF = open(os.path.expanduser("~/home-ops-dashboard/n8n/spotify_control_workflow_id.txt")).read().strip()

req = urllib.request.Request(
    f"{N8N_URL}/api/v1/workflows/{WF}",
    data=json.dumps(workflow).encode(),
    headers={"X-N8N-API-KEY": API_KEY, "Content-Type": "application/json"},
    method="PUT",
)

with urllib.request.urlopen(req) as resp:
    result = json.load(resp)
    print("Updated. Active:", result["active"])
