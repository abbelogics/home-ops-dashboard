import json
import os
import uuid

import urllib.request

N8N_URL = "http://pilab.local:5678"
API_KEY = open(os.path.expanduser("~/home-ops-dashboard/n8n/.n8n_api_key")).read().strip()
MQTT_CRED = {"id": "YOUR_N8N_CREDENTIAL_ID", "name": "Mosquitto (PiLab)"}
POSTGRES_CRED = {"id": "YOUR_N8N_CREDENTIAL_ID", "name": "Postgres account"}
# skyops - 2026-09-14, weather_log's own dedicated database, see
# log_weather_query.
POSTGRES_SKYOPS_CRED = {"id": "YOUR_N8N_CREDENTIAL_ID", "name": "Postgres account (skyops)"}
ERROR_WORKFLOW_ID = "YOUR_WORKFLOW_ID"

# User-calibrated office location (2026-09-24) - same point everywhere.
OFFICE_LAT = 0.0  # YOUR_LAT
OFFICE_LON = 0.0  # YOUR_LON


def node_id():
    return str(uuid.uuid4())


# History: Open-Meteo alone -> NWS/KMIA -> NWS + RainViewer radar -> back to
# Open-Meteo alone (all 2026-09-11; see PROJECT.md "Key decisions"). Each
# single source failed in its own way: the model misplaces Miami's small
# pop-up cells, KMIA is ~3 mi away, RainViewer was 10-15 min behind.
#
# 2026-09-24 ("make this more accurate", explicit request): fuse sources,
# each for what it is actually good at, instead of picking one:
#   - Rain now: NOAA MRMS radar at the office's own ~1 km cell, every 2 min,
#     ~3 min behind real time (PiLab ~/wx_nowcast.py -> state_cache
#     'wx_nowcast'). Also gives intensity (mm/h), precip type and lightning
#     probability. Stale (> ~10 min) -> falls back to the model's code.
#   - Sky (clear/partly/mostly/overcast): KMIA METAR cloud layers - a real
#     observation; clouds are far less hyper-local than rain, so 3 mi is
#     fine. Stale (> 90 min) -> model cloud_cover.
#   - Temp/feels/humidity/wind/sun times + 3-day forecast: Open-Meteo.
# Poll every 5 min (was 15) so the radar signal isn't wasted on a slow poll.
weather_url = (
    "https://api.open-meteo.com/v1/forecast"
    f"?latitude={OFFICE_LAT}&longitude={OFFICE_LON}"
    "&current=temperature_2m,apparent_temperature,weather_code,wind_speed_10m,wind_direction_10m,is_day,"
    "relative_humidity_2m,cloud_cover,precipitation,wind_gusts_10m"
    "&daily=sunrise,sunset,weather_code,temperature_2m_max,temperature_2m_min,"
    "precipitation_probability_max,cloud_cover_mean,uv_index_max"
    # Weather page (2026-09-24): next 6 hours. forecast_hours counts from the
    # current hour, so index 0 = now, 1..6 = the next six.
    "&hourly=temperature_2m,precipitation_probability,weather_code,cloud_cover,is_day"
    "&forecast_hours=7"
    # Today + the 3-day forecast row (tomorrow..+3). daily.sunrise[1] is
    # still tomorrow's sunrise for the "next sun event" display.
    "&forecast_days=4"
    "&temperature_unit=fahrenheit&wind_speed_unit=mph&timezone=America%2FNew_York"
)

metar_url = "https://aviationweather.gov/api/data/metar?ids=KMIA&format=json"

# One row: the radar nowcast plus the NWS workflow's current top alert
# (home-screen weather line, 2026-09-25).
nowcast_query = (
    "SELECT n.value, extract(epoch FROM now() - n.updated_at)::int AS row_age_sec, "
    "a.value AS nws, extract(epoch FROM now() - a.updated_at)::int AS nws_age_sec "
    "FROM homeops.state_cache n "
    "LEFT JOIN homeops.state_cache a ON a.key = 'nws_alert' "
    "WHERE n.key = 'wx_nowcast';"
)

format_js = r"""
const om = $('Fetch Forecast').first().json;
const current = om.current || {};
const daily = om.daily || {};
const isDay = current.is_day !== 0;

// ---- Sky cover: KMIA METAR first, model cloud_cover as fallback ----------
// Layer rank: 0 clear, 1 FEW, 2 SCT, 3 BKN, 4 OVC/VV. Layers at/above
// 20,000 ft (thin cirrus) only count when BKN/OVC, and then only as
// "partly" - a SCT250 cirrus deck doesn't make a sunny sky look cloudy.
const COVER_RANK = { SKC: 0, CLR: 0, NCD: 0, NSC: 0, CAVOK: 0, FEW: 1, SCT: 2, BKN: 3, OVC: 4, OVX: 4, VV: 4 };
let metar = null;
try {
  const m = $('Fetch METAR').first().json;
  if (m && m.obsTime && (Date.now() / 1000 - m.obsTime) <= 90 * 60) metar = m;
} catch (e) { metar = null; }

let skyRank = null, skySource = 'model';
if (metar && Array.isArray(metar.clouds)) {
  let low = 0, high = 0;
  for (const c of metar.clouds) {
    const r = COVER_RANK[c.cover] ?? 0;
    if (c.base === null || c.base === undefined || c.base < 20000) low = Math.max(low, r);
    else high = Math.max(high, r);
  }
  skyRank = Math.max(low, high >= 3 ? 2 : 0);
  skySource = 'metar';
}
if (skyRank === null) {
  const cc = current.cloud_cover ?? 0;
  skyRank = cc < 25 ? 0 : cc < 55 ? 2 : cc < 88 ? 3 : 4;
}
// 0/1 -> clear, 2 -> partly, 3 -> mostly cloudy, 4 -> overcast
const SKY = [
  ['Clear', 'sun', 'moon'], ['Mostly Clear', 'sun', 'moon'],
  ['Partly Cloudy', 'partly', 'partly_night'],
  ['Mostly Cloudy', 'mostly_cloudy', 'mostly_cloudy_night'],
  ['Overcast', 'cloudy', 'cloudy'],
];
const [skyDesc, skyDayIcon, skyNightIcon] = SKY[skyRank];

// ---- Rain now: MRMS radar at the office cell, model as fallback ---------
let nc = null;
try {
  const row = $('Fetch Nowcast').first().json;
  const v = row && row.value;
  if (v && row.row_age_sec <= 8 * 60 && v.frame_age_sec <= 10 * 60 && v.rate_mmhr !== null && v.rate_mmhr !== undefined) nc = v;
} catch (e) { nc = null; }

const code = current.weather_code;
const modelThunder = code >= 95;
const metarWx = (metar && metar.wxString) || '';
const metarThunder = /TS/.test(metarWx);

let rainSource, rate = null, intensity = null; // 'light' | 'moderate' | 'heavy'
if (nc) {
  rainSource = 'mrms';
  rate = nc.rate_mmhr;
  // AMS rain-rate classes: light < 2.5, moderate 2.5-7.6, heavy > 7.6 mm/h.
  // 0.2 mm/h floor filters radar noise / virga.
  if (rate >= 0.2) intensity = rate < 2.5 ? 'light' : rate <= 7.6 ? 'moderate' : 'heavy';
} else {
  rainSource = 'model';
  const MODEL_RAIN = { 51: 'light', 53: 'light', 55: 'moderate', 56: 'light', 57: 'moderate',
    61: 'light', 63: 'moderate', 65: 'heavy', 66: 'light', 67: 'heavy',
    80: 'light', 81: 'moderate', 82: 'heavy', 95: 'moderate', 96: 'heavy', 99: 'heavy' };
  intensity = MODEL_RAIN[code] || null;
}
const snowing = (nc && nc.precip_type === 'snow') || (!nc && [71, 73, 75, 77, 85, 86].includes(code));
const fog = !intensity && (/(^|\s|-|\+)(FG|BR)/.test(metarWx) ? metar.visib !== '10+' : [45, 48].includes(code) && !metar);

let icon, desc;
if (intensity) {
  const lightningLikely = nc ? ((nc.lightning_prob_30min ?? 0) >= 50 || metarThunder || modelThunder) : modelThunder;
  if (snowing) { icon = 'snow'; desc = 'Snow'; }
  else if (lightningLikely) { icon = 'thunder'; desc = 'Thunderstorm'; }
  else if (intensity === 'heavy') { icon = 'rain_heavy'; desc = 'Heavy Rain'; }
  // Rain falling while the sky is mostly open = a sun shower (common here,
  // and exactly the "raining but sunny" case) - moon version after dark.
  else if (skyRank <= 2) { icon = isDay ? 'sun_rain' : 'moon_rain'; desc = intensity === 'light' ? 'Light Shower' : 'Shower'; }
  else if (intensity === 'light') { icon = 'rain_light'; desc = 'Light Rain'; }
  else { icon = 'rain'; desc = 'Rain'; }
} else if (fog) { icon = 'fog'; desc = 'Fog'; }
else { icon = isDay ? skyDayIcon : skyNightIcon; desc = skyDesc; }

// Firmware before the 2026-09-24 icon set only knows these 9 keys (unknown
// keys fall back to the sun) - 'icon' stays in that set, 'icon_v2' carries
// the detailed one. New firmware prefers icon_v2.
const LEGACY = { mostly_cloudy: 'cloudy', mostly_cloudy_night: 'cloudy', sun_rain: 'rain',
  moon_rain: 'rain', rain_light: 'rain', rain_heavy: 'rain' };
const legacyIcon = LEGACY[icon] || icon;

// ---- 3-day forecast (tomorrow..+3) ---------------------------------------
// Open-Meteo's daily weather_code is the day's most significant code, so a
// 15%-chance drizzle day would otherwise show a rain icon. Rain icons need
// a real chance: < 30% -> sky icon from mean cloud cover; 30-59% -> "chance
// of showers" (sun_rain); >= 60% -> rain by intensity; thunder needs >= 40%.
const DAYS = ['SUN', 'MON', 'TUE', 'WED', 'THU', 'FRI', 'SAT'];
function dailyIcon(c, pop, cloud) {
  const skyIcon = cloud === null || cloud === undefined ? 'partly'
    : cloud < 25 ? 'sun' : cloud < 55 ? 'partly' : cloud < 88 ? 'mostly_cloudy' : 'cloudy';
  if ([45, 48].includes(c)) return 'fog';
  if ([71, 73, 75, 77, 85, 86].includes(c)) return 'snow';
  const wet = c >= 51;
  if (!wet || (pop ?? 0) < 30) return skyIcon;
  if (c >= 95) return pop >= 40 ? 'thunder' : 'sun_rain';
  if (pop < 60) return 'sun_rain';
  if ([65, 67, 82].includes(c)) return 'rain_heavy';
  if ([51, 53, 56, 61, 80].includes(c)) return 'rain_light';
  return 'rain';
}
const forecast = [];
for (let i = 1; i <= 3; i++) {
  const date = (daily.time || [])[i];
  if (!date) break;
  forecast.push({
    day: DAYS[new Date(date + 'T12:00:00Z').getUTCDay()],
    icon: dailyIcon((daily.weather_code || [])[i], (daily.precipitation_probability_max || [])[i],
                    (daily.cloud_cover_mean || [])[i]),
    lo: Math.round((daily.temperature_2m_min || [])[i]),
    hi: Math.round((daily.temperature_2m_max || [])[i]),
    pop: (daily.precipitation_probability_max || [])[i] ?? null,
  });
}

// ---- Weather page extras (2026-09-24) -----------------------------------
// Next 6 hours: same rain-needs-a-real-chance rule as the daily forecast,
// with night variants from the hour's own is_day.
const hourly = om.hourly || {};
function hourlyIcon(c, pop, cloud, day) {
  const sky = cloud === null || cloud === undefined ? 2
    : cloud < 25 ? 0 : cloud < 55 ? 2 : cloud < 88 ? 3 : 4;
  const skyIcon = [day ? 'sun' : 'moon', day ? 'sun' : 'moon', day ? 'partly' : 'partly_night',
                   day ? 'mostly_cloudy' : 'mostly_cloudy_night', 'cloudy'][sky];
  if ([45, 48].includes(c)) return 'fog';
  if ([71, 73, 75, 77, 85, 86].includes(c)) return 'snow';
  if (c < 51 || (pop ?? 0) < 30) return skyIcon;
  if (c >= 95) return pop >= 40 ? 'thunder' : (day ? 'sun_rain' : 'moon_rain');
  if (pop < 60) return day ? 'sun_rain' : 'moon_rain';
  if ([65, 67, 82].includes(c)) return 'rain_heavy';
  if ([51, 53, 56, 61, 80].includes(c)) return 'rain_light';
  return 'rain';
}
function hourLabel(iso) {
  const h = Number(iso.split('T')[1].slice(0, 2));
  return `${h % 12 || 12}${h >= 12 ? 'p' : 'a'}`;
}
const hours = [];
for (let i = 1; i <= 6; i++) {
  const t = (hourly.time || [])[i];
  if (!t) break;
  hours.push({
    t: hourLabel(t),
    icon: hourlyIcon(hourly.weather_code[i], hourly.precipitation_probability[i],
                     hourly.cloud_cover[i], hourly.is_day[i] !== 0),
    temp: Math.round(hourly.temperature_2m[i]),
    pop: hourly.precipitation_probability[i] ?? 0,
  });
}
// Radar summary for the page's rain line. src 'model' = radar stale.
const radar = nc ? {
  src: 'mrms',
  rate: rate,
  near_mi: nc.nearest_rain_mi ?? null,
  ltg: nc.lightning_prob_30min ?? null,
} : { src: 'model', rate: null, near_mi: null, ltg: null };
// ---- Home-screen weather line (2026-09-25) ------------------------------
// One short line (fits ~38 chars at 16px), ONLY for what's imminent
// (explicit request: "show only what is about to happen... like floods in
// downtown" - an always-on "No rain expected" was noise). Most important
// first: NWS alert (incl. coastal-flood/king-tide advisories for FLZ074) >
// rain here (radar) > rain within 5 mi (radar) > rain likely within ~3 h
// (pop >= 50%). Nothing imminent -> outlook null -> the device hides it.
// color: red/orange = alert, blue = raining here, amber = lightning or rain
// close/soon, green = all clear. icon (alerts only): warning | hurricane.
let outlook = null;
try {
  const row = $('Fetch Nowcast').first().json;
  const a = row && row.nws;
  if (a && a.text && row.nws_age_sec <= 10 * 60) {
    outlook = { text: a.text, color: a.severity === 'red' ? 'red' : 'orange', icon: a.icon || 'warning' };
  }
} catch (e) { /* no alert row yet */ }
if (!outlook && nc && intensity) {
  const what = intensity === 'light' ? 'Light rain' : intensity === 'moderate' ? 'Rain' : 'Heavy rain';
  outlook = (nc.lightning_prob_30min ?? 0) >= 30
    ? { text: `${what} \u00b7 Lightning ${Math.round(nc.lightning_prob_30min)}%`, color: 'amber' }
    : { text: `${what} here \u00b7 ${rate.toFixed(1)} mm/h`, color: 'blue' };
}
if (!outlook && nc && nc.nearest_rain_mi !== null && nc.nearest_rain_mi !== undefined && nc.nearest_rain_mi <= 5) {
  outlook = { text: `Dry here \u00b7 rain ${nc.nearest_rain_mi.toFixed(1)} mi away`,
              color: 'amber' };
}
if (!outlook) {
  const wet = hours.slice(0, 3).find(h => (h.pop ?? 0) >= 50);
  if (wet) outlook = { text: `Rain likely ~${wet.t}m (${Math.round(wet.pop)}%)`, color: 'amber' };
}

const today = {
  hi: Math.round((daily.temperature_2m_max || [])[0]),
  lo: Math.round((daily.temperature_2m_min || [])[0]),
  uv: Math.round((daily.uv_index_max || [])[0] ?? 0),
  pop: (daily.precipitation_probability_max || [])[0] ?? null,
};

function formatTime(iso) {
  if (!iso) return null;
  const [, hhmm] = iso.split('T');
  let [h, m] = hhmm.split(':').map(Number);
  const ampm = h >= 12 ? 'pm' : 'am';
  h = h % 12 || 12;
  return `${h}:${String(m).padStart(2, '0')}${ampm}`;
}
// Raw "HH:MM" (24h) alongside the pretty strings - the firmware picks the
// next sun event against its own clock every 3s (see PROJECT.md).
function formatTime24(iso) {
  if (!iso) return null;
  const [, hhmm] = iso.split('T');
  return hhmm;
}
const sunriseArr = Array.isArray(daily.sunrise) ? daily.sunrise : [];
const sunsetArr = Array.isArray(daily.sunset) ? daily.sunset : [];

return [{
  json: {
    icon: legacyIcon,
    icon_v2: icon,
    description: desc,
    temp_f: Math.round(current.temperature_2m * 10) / 10,
    feels_like_f: Math.round(current.apparent_temperature),
    humidity_pct: Math.round(current.relative_humidity_2m),
    wind_mph: Math.round(current.wind_speed_10m),
    wind_dir_deg: Math.round(current.wind_direction_10m),
    sunrise: formatTime(sunriseArr[0] || null),
    sunset: formatTime(sunsetArr[0] || null),
    sunrise_tomorrow: formatTime(sunriseArr[1] || null),
    sunrise_24h: formatTime24(sunriseArr[0] || null),
    sunset_24h: formatTime24(sunsetArr[0] || null),
    forecast,
    hourly: hours,
    today,
    gust_mph: current.wind_gusts_10m === undefined ? null : Math.round(current.wind_gusts_10m),
    radar,
    outlook,
    updated_at: new Date().toISOString(),
    // Stripped before MQTT (device payload stays small); logged to
    // skyops.weather_log so the fused decision can be audited later.
    _diag: {
      rain_source: rainSource,
      rain_rate_mmhr: rate,
      precip_type: nc ? nc.precip_type : null,
      lightning_prob: nc ? nc.lightning_prob_30min : null,
      sky_source: skySource,
      sky_rank: skyRank,
      model_code: code ?? null,
      model_cloud_cover: current.cloud_cover ?? null,
      metar_raw: metar ? metar.rawOb : null,
    },
  },
}];
""".strip()

strip_diag_js = """
const { _diag, ...payload } = $input.first().json;
return [{ json: payload }];
""".strip()

# Historical logging (skyops.weather_log). Values go in as query parameters
# ($1..$n via options.queryReplacement) - never pasted into the SQL text, so
# any character (e.g. a METAR remark) is safe. Diagnostic columns added
# 2026-09-24 for the fused rain/sky decision.
log_weather_query = (
    "INSERT INTO weather_log ("
    "  icon, description, temp_f, feels_like_f, humidity_pct, wind_mph, wind_dir_deg,"
    "  sunrise, sunset, icon_v2, rain_source, rain_rate_mmhr, precip_type, lightning_prob,"
    "  sky_source, sky_rank, model_code, model_cloud_cover, metar_raw, forecast"
    ") VALUES ("
    "  $1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14, $15, $16, $17, $18, $19, $20::jsonb"
    ");"
)
log_weather_params = (
    "={{ [$json.icon, $json.description, $json.temp_f ?? null, $json.feels_like_f ?? null,"
    " $json.humidity_pct ?? null, $json.wind_mph ?? null, $json.wind_dir_deg ?? null,"
    " $json.sunrise ?? null, $json.sunset ?? null, $json.icon_v2,"
    " $json._diag.rain_source, $json._diag.rain_rate_mmhr ?? null, $json._diag.precip_type ?? null,"
    " $json._diag.lightning_prob ?? null, $json._diag.sky_source, $json._diag.sky_rank ?? null,"
    " $json._diag.model_code ?? null, $json._diag.model_cloud_cover ?? null, $json._diag.metar_raw ?? null,"
    " JSON.stringify($json.forecast || [])] }}"
)

nodes = [
    {
        "id": node_id(), "name": "Every 5 min", "type": "n8n-nodes-base.scheduleTrigger",
        "typeVersion": 1.3, "position": [0, 0],
        "parameters": {"rule": {"interval": [{"field": "minutes", "minutesInterval": 5}]}},
    },
    # Instant refresh: PiLab's wx-nowcast service calls this the moment the
    # radar rain state changes (dry/light/moderate/heavy/lightning), so the
    # BOX-3 doesn't wait up to 5 min for the schedule. LAN-only.
    {
        "id": node_id(), "name": "Rain Change Webhook", "type": "n8n-nodes-base.webhook",
        "typeVersion": 2, "position": [0, 180], "webhookId": "7d3f0f2e-8a51-4c5e-9f7a-3c1b2e4d5f60",
        "parameters": {"path": "homeops-weather-refresh", "httpMethod": "GET", "responseMode": "onReceived", "options": {}},
    },
    {
        # 3 tries, 5s apart: a momentary Open-Meteo/network blip (one hit
        # 19:46 on day 1 and paged via the Error Handler) recovers silently;
        # only a real outage (all 3 fail) still alerts.
        "id": node_id(), "name": "Fetch Forecast", "type": "n8n-nodes-base.httpRequest",
        "typeVersion": 4.5, "position": [220, 0], "retryOnFail": True, "maxTries": 3, "waitBetweenTries": 5000,
        "parameters": {"url": weather_url, "options": {"timeout": 10000}},
    },
    # METAR / nowcast are optional inputs - a failure falls back to the
    # model inside Format Weather instead of failing the whole poll.
    {
        "id": node_id(), "name": "Fetch METAR", "type": "n8n-nodes-base.httpRequest",
        "typeVersion": 4.5, "position": [440, 0], "onError": "continueRegularOutput", "retryOnFail": True, "maxTries": 3, "waitBetweenTries": 5000,
        "alwaysOutputData": True, "executeOnce": True,
        "parameters": {"url": metar_url, "options": {"timeout": 10000}},
    },
    {
        "id": node_id(), "name": "Fetch Nowcast", "type": "n8n-nodes-base.postgres",
        "typeVersion": 2.6, "position": [660, 0], "onError": "continueRegularOutput",
        "alwaysOutputData": True, "executeOnce": True,
        "parameters": {"operation": "executeQuery", "query": nowcast_query, "options": {}},
        "credentials": {"postgres": POSTGRES_CRED},
    },
    {
        "id": node_id(), "name": "Format Weather", "type": "n8n-nodes-base.code",
        "typeVersion": 2, "position": [880, 0], "executeOnce": True,
        "parameters": {"language": "javaScript", "jsCode": format_js},
    },
    {
        "id": node_id(), "name": "Strip Diagnostics", "type": "n8n-nodes-base.code",
        "typeVersion": 2, "position": [1100, -100],
        "parameters": {"language": "javaScript", "jsCode": strip_diag_js},
    },
    {
        "id": node_id(), "name": "Publish Weather State", "type": "n8n-nodes-base.mqtt",
        "typeVersion": 1, "position": [1320, -100],
        "parameters": {"topic": "homeops/weather/state", "sendInputData": True, "options": {"retain": True}},
        "credentials": {"mqtt": MQTT_CRED},
    },
    {
        "id": node_id(), "name": "Log Weather", "type": "n8n-nodes-base.postgres",
        "typeVersion": 2.6, "position": [1100, 120],
        "parameters": {"operation": "executeQuery", "query": log_weather_query,
                       "options": {"queryReplacement": log_weather_params}},
        "credentials": {"postgres": POSTGRES_SKYOPS_CRED},
    },
]

connections = {
    "Every 5 min": {"main": [[{"node": "Fetch Forecast", "type": "main", "index": 0}]]},
    "Rain Change Webhook": {"main": [[{"node": "Fetch Forecast", "type": "main", "index": 0}]]},
    "Fetch Forecast": {"main": [[{"node": "Fetch METAR", "type": "main", "index": 0}]]},
    "Fetch METAR": {"main": [[{"node": "Fetch Nowcast", "type": "main", "index": 0}]]},
    "Fetch Nowcast": {"main": [[{"node": "Format Weather", "type": "main", "index": 0}]]},
    "Format Weather": {
        "main": [[
            {"node": "Strip Diagnostics", "type": "main", "index": 0},
            {"node": "Log Weather", "type": "main", "index": 0},
        ]]
    },
    "Strip Diagnostics": {"main": [[{"node": "Publish Weather State", "type": "main", "index": 0}]]},
}

workflow = {
    "name": "Home Ops - Weather Poll",
    "nodes": nodes,
    "connections": connections,
    "settings": {
        "executionOrder": "v1",
        "errorWorkflow": ERROR_WORKFLOW_ID,
        "callerPolicy": "workflowsFromSameOwner",
        # PUT merges settings - keep this explicit (see the aircraft poll
        # script: "none" broke execution cleanup on n8n 2.41).
        "saveDataSuccessExecution": "all",
    },
}

WF = open(os.path.expanduser("~/home-ops-dashboard/n8n/weather_poll_workflow_id.txt")).read().strip()

req = urllib.request.Request(
    f"{N8N_URL}/api/v1/workflows/{WF}",
    data=json.dumps(workflow).encode(),
    headers={"X-N8N-API-KEY": API_KEY, "Content-Type": "application/json"},
    method="PUT",
)

with urllib.request.urlopen(req) as resp:
    result = json.load(resp)
    print("Updated. Active:", result["active"])
