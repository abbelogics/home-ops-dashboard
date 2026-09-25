import json
import os
import uuid

import urllib.request

N8N_URL = "http://pilab.local:5678"
API_KEY = open(os.path.expanduser("~/home-ops-dashboard/n8n/.n8n_api_key")).read().strip()
# FlightAware AeroAPI - added 2026-09-12 as the primary route source.
# adsbdb.com's schedule-based route data has been repeatedly wrong for
# reused flight numbers (see ROUTE_OVERRIDES below and PROJECT.md) - AeroAPI
# is a real, paid flight-tracking data provider and should be materially
# more accurate. Read from a local dotfile, same pattern as .hue_api_key -
# gitignored, never committed.
FLIGHTAWARE_KEY = open(os.path.expanduser("~/home-ops-dashboard/n8n/.flightaware_api_key")).read().strip()
POSTGRES_CRED = {"id": "YOUR_N8N_CREDENTIAL_ID", "name": "Postgres account"}
# skyops - 2026-09-14, aircraft_log's own dedicated database (see
# log_sighting_query/build_log_entry_js) - separate from the shared
# streetwear/homeops Postgres credential above, which everything else in
# this workflow (MIA FIDS cache, FA usage cache, cacheV2 reads) still uses.
POSTGRES_SKYOPS_CRED = {"id": "YOUR_N8N_CREDENTIAL_ID", "name": "Postgres account (skyops)"}
MQTT_CRED = {"id": "YOUR_N8N_CREDENTIAL_ID", "name": "Mosquitto (PiLab)"}
ERROR_WORKFLOW_ID = "YOUR_WORKFLOW_ID"
WF = open(os.path.expanduser("~/home-ops-dashboard/n8n/aircraft_poll_workflow_id.txt")).read().strip()


def node_id():
    return str(uuid.uuid4())


nearest_code_js = """
// Exact office coords (user-calibrated pin, 2026-09-24 - replaced the
// earlier phone-GPS <coord>/<coord>, ~80 m off) and window sightline (phone
// compass, pointed straight out the window: 93 deg = just south of due
// east). The earlier ~<coord>/<coord> approximation was off by ~0.9mi,
// which alone swung a nearby aircraft's computed bearing by 27+ degrees -
// exact coords matter a lot for anything within a few miles.
const OFFICE_LAT = 0.0; // YOUR_LAT
const OFFICE_LON = 0.0; // YOUR_LON

// "Visible from the window" = bearing within the window's sightline AND
// genuinely close/low enough to actually see with the naked eye - not
// just "somewhere in that direction". Originally distance-only (no
// altitude check at all) with a generous 25mi/±90deg envelope; tightened
// 2026-09-14 after live examples proved it too loose - ITA630 at
// 11.8mi/9825ft and an unlabeled A330 at 14.7mi/10200ft both passed the
// old filter and were confirmed NOT actually visible from the window.
// Real airliners only read as genuinely visible within a few miles and a
// few thousand feet in practice (MIA approaches only get low in their
// final few miles), so both caps came down hard rather than a small
// trim. First pass: 6mi/5000ft, bearing corridor 30-150deg (a true
// compass heading of 90deg dead ahead, narrower than the previous
// 3-180deg guess). Narrowed same day to 3mi/4000ft, corridor 45-135deg.
// Opened back up 2026-09-17 to 5mi/5000ft (corridor unchanged) - a real
// 747 overhead wasn't showing/beeping, and heavies routinely cruise
// above 4000ft even fairly close in - not a final answer, expect to
// keep tuning all of these against real sightings vs. what's actually
// visible.
const VIEW_BEARING_DEG = 90;
const BEARING_TOLERANCE_DEG = 45;
const MAX_VISIBLE_DISTANCE_MI = 5;
const MAX_VISIBLE_ALT_FT = 5000;

// readsb keeps an aircraft in its output for up to ~60s after its last
// actual position report (seen_pos = seconds since that report). Without
// this cutoff, a plane that's lost signal or drifted off keeps being
// "seen" at its last known spot - if that spot happened to be in the
// window's corridor, it looks frozen on screen long after it's gone.
const MAX_POSITION_AGE_SEC = 10;

// 747s/A380s/military get a looser envelope (2026-09-22, MPH6121 747-400F
// departing east: crossed into the window corridor right as it climbed
// through ~5000ft, and the indoor antenna's position fixes on it went 45s
// stale - never alerted). Heavies stay plainly visible well above 5000ft,
// and a climbing departure is exactly when reception gets patchy, so both
// the altitude cap and the stale-position cutoff relax for these only.
// Everything else keeps the tighter numbers above (the stale cutoff exists
// to stop a gone plane "sticking" on screen - 45s of that is an acceptable
// trade for a rare heavy/military pass).
const SPECIAL_MAX_VISIBLE_ALT_FT = 8000;
const SPECIAL_MAX_POSITION_AGE_SEC = 45;

// Fallback military detection by ICAO type code (see specialKind) for
// airframes readsb's db doesn't flag - defined up here, not next to
// specialKind, because isVisibleFromWindow/the stale filter call
// specialKind before that point and a const isn't hoisted.
const MILITARY_TYPE_CODES = [
  'C130', 'C30J', 'C17', 'C5M', 'K35R', 'KC46', 'KC10', 'E3CF', 'E6', 'P8',
  'F35', 'F16', 'F18H', 'F18S', 'F22', 'F15', 'A10', 'B52', 'B1', 'B2',
  'V22', 'H60', 'H47', 'C2', 'E2', 'C12', 'C40', 'C32', 'C37A', 'KC39',
];

// Matched by callsign prefix, plus an optional ICAO type prefix. (An old
// note here said this feed never populates "t" - no longer true since
// readsb got --db-file; specialKind() relies on it too.) typePrefix fails
// open when "t" is missing, so a plane isn't dropped just for lacking db
// data. Cargolux/Atlas added 2026-09-22 on request, 747s only - Atlas
// (GTI) also flies 767s/777s under the same callsign prefix. DLH limited
// to 747s the same day (previously any Lufthansa, e.g. an A350, was
// labeled "Lufthansa 747").
const WATCHLIST = [
  { operatorPrefix: 'BAW', label: 'British Airways A380' },
  { operatorPrefix: 'DLH', typePrefix: 'B74', label: 'Lufthansa 747' },
  { operatorPrefix: 'CLX', typePrefix: 'B74', label: 'Cargolux 747' },
  { operatorPrefix: 'GTI', typePrefix: 'B74', label: 'Atlas 747' },
];

function watchlistTypeOk(w, a) {
  if (!w.typePrefix || !a.t) return true;
  return a.t.toUpperCase().startsWith(w.typePrefix);
}

function toRad(deg) { return deg * Math.PI / 180; }

function haversineMiles(lat1, lon1, lat2, lon2) {
  const R = 3958.8;
  const dLat = toRad(lat2 - lat1);
  const dLon = toRad(lon2 - lon1);
  const a = Math.sin(dLat / 2) ** 2 +
            Math.cos(toRad(lat1)) * Math.cos(toRad(lat2)) * Math.sin(dLon / 2) ** 2;
  return R * 2 * Math.atan2(Math.sqrt(a), Math.sqrt(1 - a));
}

function bearingDeg(lat1, lon1, lat2, lon2) {
  const y = Math.sin(toRad(lon2 - lon1)) * Math.cos(toRad(lat2));
  const x = Math.cos(toRad(lat1)) * Math.sin(toRad(lat2)) -
            Math.sin(toRad(lat1)) * Math.cos(toRad(lat2)) * Math.cos(toRad(lon2 - lon1));
  return (Math.atan2(y, x) * 180 / Math.PI + 360) % 360;
}

const resp = $('Fetch aircraft.json').first().json;
const aircraft = Array.isArray(resp.aircraft) ? resp.aircraft : [];

// Network callsign fill-in (2026-09-22). ~1 in 4 sightings was first seen
// with NO callsign: this indoor, east-facing antenna often first hears a
// MIA departure only ~0.6mi out, and the callsign (a separate ADS-B
// message, every ~5s) lands 6-30s later - after the plane is already
// behind the building across the street (live: AAL2124 blank at 17:58:21,
// callsign 17:58:30). No callsign = no logo (keyed off the callsign
// prefix on-device) and no route. "Blank Callsign Check" asks adsb.lol
// by hex for the nearest such plane; its answer (hit or miss) is cached
// here per hex so the API sees one call per plane, not one per poll.
const csStatic = $getWorkflowStaticData('global');
if (!csStatic.netCallsigns) csStatic.netCallsigns = {};
try {
  // Throws when the lookup branch didn't run this poll - that's the
  // normal no-blank-callsign case, not an error.
  const lookup = $('Network Callsign Lookup').first().json;
  const hex = $('Blank Callsign Check').first().json.blank_hex;
  const ac = Array.isArray(lookup.ac) ? lookup.ac[0] : null;
  const flight = ac && typeof ac.flight === 'string' ? ac.flight.trim() : '';
  if (hex) csStatic.netCallsigns[hex] = { flight, at: Date.now() };
} catch (e) {
  // lookup didn't run this poll - carry on without it
}
// A hit is good for the whole pass; a miss (timeout/API error/network
// doesn't know it either) is retried after a minute rather than asked
// again every 3s poll.
const NET_CALLSIGN_HIT_TTL_MS = 30 * 60 * 1000;
const NET_CALLSIGN_MISS_TTL_MS = 60 * 1000;
for (const [hex, v] of Object.entries(csStatic.netCallsigns)) {
  const ttl = v.flight ? NET_CALLSIGN_HIT_TTL_MS : NET_CALLSIGN_MISS_TTL_MS;
  if (Date.now() - v.at > ttl) delete csStatic.netCallsigns[hex];
}
for (const a of aircraft) {
  if ((a.flight || '').trim()) continue;
  const cached = csStatic.netCallsigns[a.hex];
  if (cached && cached.flight) {
    a.flight = cached.flight;
    a.callsign_source = 'network';
  }
}

// Flight number = callsign with its 3-letter ICAO airline prefix stripped
// (e.g. "AAL833" -> "833"). Computed locally so it's always available
// regardless of whether adsbdb has a route for this specific flight.
function flightNumber(flight) {
  if (!flight || flight.length <= 3) return null;
  return flight.slice(3).replace(/^0+/, '') || flight.slice(3);
}

function angleDiff(a, b) {
  const diff = Math.abs(a - b) % 360;
  return diff > 180 ? 360 - diff : diff;
}

function isVisibleFromWindow(a) {
  if (a.distance_mi > MAX_VISIBLE_DISTANCE_MI) return false;
  // Fails open (doesn't reject) when altitude is missing/non-numeric
  // (e.g. "ground" for a taxiing aircraft, or just not reported) - same
  // fail-open convention already used elsewhere in this file for missing
  // data, and a plane with no altitude data is very unlikely to be the
  // "too high to see" case this check exists for anyway.
  const maxAlt = specialKind(a) ? SPECIAL_MAX_VISIBLE_ALT_FT : MAX_VISIBLE_ALT_FT;
  if (typeof a.alt_baro === 'number' && a.alt_baro > maxAlt) return false;
  return angleDiff(a.bearing_deg, VIEW_BEARING_DEG) <= BEARING_TOLERANCE_DEG;
}

// Signed angle (degrees) off the window's centerline - 0 means "look
// straight out the window", positive means look to the right of that,
// negative to the left.
function lookAngle(bearing) {
  return Math.round(((bearing - VIEW_BEARING_DEG + 540) % 360) - 180);
}

const withDistance = aircraft
  .filter(a => typeof a.lat === 'number' && typeof a.lon === 'number')
  .filter(a => typeof a.seen_pos !== 'number' ||
               a.seen_pos <= (specialKind(a) ? SPECIAL_MAX_POSITION_AGE_SEC : MAX_POSITION_AGE_SEC))
  .map(a => ({
    ...a,
    distance_mi: haversineMiles(OFFICE_LAT, OFFICE_LON, a.lat, a.lon),
    bearing_deg: Math.round(bearingDeg(OFFICE_LAT, OFFICE_LON, a.lat, a.lon)),
  }))
  .filter(isVisibleFromWindow)
  .sort((a, b) => a.distance_mi - b.distance_mi);

const nearestRaw = withDistance[0] || null;
const nearest = nearestRaw ? {
  flight: (nearestRaw.flight || '').trim() || null,
  flight_number: flightNumber((nearestRaw.flight || '').trim()),
  hex: nearestRaw.hex || null,
  // type/desc/owner now come straight from readsb's own db-file (see
  // sky's /etc/default/readsb) - no external lookup needed for these.
  type: nearestRaw.t || null,
  desc: nearestRaw.desc || null,
  owner: nearestRaw.ownOp || null,
  alt_ft: typeof nearestRaw.alt_baro === 'number' ? nearestRaw.alt_baro : null,
  heading_deg: typeof nearestRaw.track === 'number' ? Math.round(nearestRaw.track) : null,
  distance_mi: Math.round(nearestRaw.distance_mi * 10) / 10,
  bearing_deg: nearestRaw.bearing_deg,
  look_angle_deg: lookAngle(nearestRaw.bearing_deg),
} : null;

const overhead = !!nearest;

let watchlistMatch = null;
for (const a of withDistance) {
  const flight = (a.flight || '').trim();
  for (const w of WATCHLIST) {
    if (flight.startsWith(w.operatorPrefix) && watchlistTypeOk(w, a)) {
      watchlistMatch = {
        label: w.label,
        flight,
        flight_number: flightNumber(flight),
        hex: a.hex || null,
        type: a.t || null,
        desc: a.desc || null,
        owner: a.ownOp || null,
        alt_ft: typeof a.alt_baro === 'number' ? a.alt_baro : null,
        heading_deg: typeof a.track === 'number' ? Math.round(a.track) : null,
        distance_mi: Math.round(a.distance_mi * 10) / 10,
        bearing_deg: a.bearing_deg,
        look_angle_deg: lookAngle(a.bearing_deg),
      };
      break;
    }
  }
  if (watchlistMatch) break;
}

// Sound-alert trigger for the box3 speaker - a 747, an A380, or a military
// aircraft, REGARDLESS of airline (unlike WATCHLIST above, which is
// callsign-prefix-specific for the on-screen display target). Any
// currently-visible match fires, not just the nearest one. Type prefix
// checked against readsb's own "t" field (ICAO type code, e.g. "B744",
// "A388") - covers every 747/A380 variant without listing each one.
// Military comes from readsb's dbFlags bit 0 (wiedehopf's tar1090-db
// military/PIA/LADD flag set, confirmed present on this feed via a live
// aircraft.json sample - bit 0 = military, bit 1 = interesting, bit 2 =
// PIA, bit 3 = LADD).
function specialKind(a) {
  const type = (a.t || '').toUpperCase();
  if (type.startsWith('B74')) return '747';
  if (type.startsWith('A38')) return 'a380';
  if (typeof a.dbFlags === 'number' && (a.dbFlags & 1)) return 'military';
  // Fallback for military airframes the db doesn't flag (dbFlags missing
  // or 0) - common US military types by ICAO type code.
  if (MILITARY_TYPE_CODES.includes(type)) return 'military';
  return null;
}

let specialAircraft = null;
for (const a of withDistance) {
  const kind = specialKind(a);
  if (!kind) continue;
  specialAircraft = {
    kind,
    flight: (a.flight || '').trim() || null,
    hex: a.hex || null,
    type: a.t || null,
    desc: a.desc || null,
    distance_mi: Math.round(a.distance_mi * 10) / 10,
  };
  break;
}

// Whichever one will actually be displayed is the one worth enriching
// (airline/type/route via adsbdb.com) - avoid looking up aircraft that
// won't even be shown.
const displayTarget = watchlistMatch || nearest;

const staticData = $getWorkflowStaticData('global');
if (!staticData.cacheV2) staticData.cacheV2 = {};

// Same hex+flight key can legitimately mean a DIFFERENT actual route on a
// later day - regional carriers (e.g. Envoy/"ENY") routinely reuse a
// flight number for a different city-pair leg. Without a TTL, a route
// cached from a much earlier sighting would keep getting served back
// forever (the only other eviction is a >200-entries size cap), which
// showed up as a stale/wrong route for a repeat flight number.
const CACHE_TTL_MS = 6 * 60 * 60 * 1000; // 6h - well past any single continuous sighting, short of a next-day reuse

// FlightAware AeroAPI hard cost cap - real dollar figure, not a call-
// count proxy. Was a call-COUNT safety valve (FA_MONTHLY_CAP=500 calls,
// added 2026-09-12) assuming the per-query price stays constant -
// replaced 2026-09-13, explicit request for a hard monthly budget ceiling
// tied to the actual number, not an assumption. The real check against
// FlightAware's own free GET /account/usage (fetched every 10 min, see
// "Match MIA FIDS") happens downstream, once it's actually needed - this
// node doesn't have that cached usage data available yet at this point
// in the pipeline, so faAllowed is left undecided (null) here and
// resolved later. adsbdb (free) is never gated by any of this - only the
// paid FlightAware call is.
let lookup = { needed: 0, key: null, hex: null, callsign: null, cached: null, faAllowed: false };

if (displayTarget && displayTarget.hex && displayTarget.flight) {
  const key = displayTarget.hex + ':' + displayTarget.flight;
  const cached = staticData.cacheV2[key];
  const fresh = cached && (Date.now() - cached.cachedAt) < CACHE_TTL_MS;
  if (fresh) {
    lookup = { needed: 0, key, hex: displayTarget.hex, callsign: displayTarget.flight, cached, faAllowed: false };
  } else {
    // No paid FlightAware lookup for local recreational/training traffic
    // (explicit request 2026-09-24): a US tail-number callsign (N + digit)
    // below 1500ft - helicopters, Cessnas, pattern work. They almost never
    // file a flight plan, so FA returned only a departure airport
    // ("HWO-?") while still costing a call (~10/day on east-flow days).
    // Business jets on tail numbers fly the approach at ~2000ft+ and keep
    // their lookups. adsbdb (free) still runs for these.
    // Coast Guard (same request): callsign "C" + 4 digits (C65xx MH-65
    // helicopters, C23xx HC-144s out of OPF) at ANY altitude - no filed
    // plans, FA returned only "OPF-?" on every sighting so far. Airline
    // callsigns always start with a 3-letter ICAO prefix, so this can't
    // catch one.
    const localGA = (/^N[0-9]/.test(displayTarget.flight) &&
      typeof displayTarget.alt_ft === 'number' && displayTarget.alt_ft < 1500) ||
      /^C[0-9]{4}$/.test(displayTarget.flight);
    lookup = { needed: 1, key, hex: displayTarget.hex, callsign: displayTarget.flight, cached: null, faAllowed: null, localGA };
  }
}

return [{
  json: {
    nearest,
    overhead,
    watchlist_match: watchlistMatch,
    special_aircraft: specialAircraft,
    aircraft_count: withDistance.length,
    updated_at: new Date().toISOString(),
    _lookup: lookup,
  },
}];
""".strip()

merge_enrichment_js = """
// Type/desc/owner already come from readsb's own db-file (see
// "Nearest + Watchlist"). Airline name still comes from adsbdb.com's
// callsign endpoint (cheap, free, and its name accuracy was never the
// complaint). Route (origin/destination) now prefers FlightAware AeroAPI,
// added 2026-09-12 - adsbdb's schedule-based route was repeatedly wrong
// for reused flight numbers (see ROUTE_OVERRIDES below, kept as a final
// safety net over BOTH sources). adsbdb's own route is still the fallback
// whenever AeroAPI has nothing usable for this callsign.
// Reads $('Match MIA FIDS'), not $('Nearest + Watchlist') - real bug
// caught before deploying: "Nearest + Watchlist"'s own stored output
// predates fa_usage being attached (that happens downstream in "Match
// MIA FIDS"), so reading it directly would silently drop fa_usage here.
// On this branch (Needs Lookup? true = MIA didn't match) "Match MIA
// FIDS" carries the identical nearest/watchlist/_lookup fields anyway,
// plus the fa_usage this node needs to pass through below.
const original = $('Match MIA FIDS').first().json;
const routeResp = $('Lookup Route').first().json;

// "Lookup Route (FlightAware)" is gated behind the monthly call cap (see
// "Nearest + Watchlist"/_lookup.faAllowed and the new "Under FlightAware
// Cap?" IF node) - when the cap is hit that node never executes at all
// for this run, and referencing it via $() throws rather than returning
// anything. Treat that the same as "FlightAware had nothing" (falls
// through to adsbdb's route below), not an error.
let faResp = null;
try {
  faResp = $('Lookup Route (FlightAware)').first().json;
} catch (e) {
  faResp = null;
}

const route = routeResp && routeResp.response && routeResp.response.flightroute;

// AeroAPI's /flights/{ident} returns that flight NUMBER's/tail number's
// whole recent schedule - past AND future instances, often alternating
// routes day to day (confirmed live: AAL1173 alone returned MIA<->LAS,
// LAS<->MIA, and even unrelated PIT<->ORD/KMQ<->CTS legs from other days
// reusing the same number). Naively taking the first result is wrong -
// it's often a future scheduled leg, not the one actually in the air
// right now. The list comes back time-descending (future-first), so the
// correct "this is the one currently flying" pick is the most recent
// entry whose departure timestamp is already in the past.
//
// Real bug found via a live test against a private/GA tail number
// (N70AP): airline entries populate `scheduled_out` (gate departure), but
// GA/private IFR flight plans routinely leave that field null and only
// populate `scheduled_off` (takeoff)/`actual_off` - a version keyed only
// on scheduled_out skipped every real GA leg and picked a stale one from
// over a day earlier instead of the actual current "En Route" leg.
// Checking actual_off first (strongest signal - it's genuinely departed),
// falling back to scheduled_off then scheduled_out, covers both airline
// and GA/private entries correctly.
function legDepartureTime(f) {
  return f.actual_off || f.scheduled_off || f.scheduled_out || null;
}
function currentFlightAwareLeg(resp) {
  const flights = resp && Array.isArray(resp.flights) ? resp.flights : [];
  const now = Date.now();
  for (const f of flights) {
    const ts = legDepartureTime(f);
    if (!ts) continue;
    if (Date.parse(ts) <= now) return f;
  }
  return null;
}
const faLeg = currentFlightAwareLeg(faResp);
const faOrigin = faLeg && faLeg.origin ? faLeg.origin.code_iata : null;
const faDestination = faLeg && faLeg.destination ? faLeg.destination.code_iata : null;

// ROUTE_OVERRIDES (AAL1173/2436/909) removed 2026-09-12 now that
// FlightAware AeroAPI is the primary route source - those existed only to
// patch around adsbdb's specific known-wrong routes for those three
// flight numbers, and are no longer needed as a safety net now that a
// real flight-tracking provider is in the loop.
const adsbdbOrigin = route && route.origin ? route.origin.iata_code : null;
const adsbdbDestination = route && route.destination ? route.destination.iata_code : null;

// Attribution is a single best-effort label (whichever source actually
// supplied origin_iata), not a per-field breakdown - FlightAware and
// adsbdb disagreeing on just one leg of a route is a rare enough edge
// case that a combined "mixed" label isn't worth the complexity.
const routeSource = faOrigin ? 'flightaware' : (adsbdbOrigin ? 'adsbdb' : null);

const enrichment = {
  airline: route && route.airline ? route.airline.name : null,
  origin_iata: faOrigin || adsbdbOrigin,
  destination_iata: faDestination || adsbdbDestination,
  route_source: routeSource,
};

const staticData = $getWorkflowStaticData('global');
if (!staticData.cacheV2) staticData.cacheV2 = {};
if (original._lookup && original._lookup.key) {
  staticData.cacheV2[original._lookup.key] = { ...enrichment, cachedAt: Date.now() };
  // Bound cache growth - drop the oldest half once it gets large.
  const keys = Object.keys(staticData.cacheV2);
  if (keys.length > 200) {
    keys.sort((a, b) => staticData.cacheV2[a].cachedAt - staticData.cacheV2[b].cachedAt);
    for (const k of keys.slice(0, 100)) delete staticData.cacheV2[k];
  }
}

function apply(target) {
  if (!target) return target;
  return { ...target, ...enrichment };
}

const isWatchlist = !!original.watchlist_match;
return [{
  json: {
    nearest: isWatchlist ? original.nearest : apply(original.nearest),
    overhead: original.overhead,
    watchlist_match: isWatchlist ? apply(original.watchlist_match) : original.watchlist_match,
    special_aircraft: original.special_aircraft,
    aircraft_count: original.aircraft_count,
    updated_at: original.updated_at,
    // Real gotcha, already hit once before in this exact file (see the
    // special_aircraft history above): this object is reconstructed
    // field-by-field rather than spreading the input through, so any
    // field left off this list gets silently dropped. fa_usage (see
    // "Match MIA FIDS") needs to be listed explicitly here too.
    fa_usage: original.fa_usage,
  },
}];
""".strip()

use_cached_js = """
const original = $input.first().json;
const cached = original._lookup && original._lookup.cached;

function apply(target) {
  if (!target || !cached) return target;
  const { cachedAt, ...fields } = cached;
  return { ...target, ...fields };
}

const isWatchlist = !!original.watchlist_match;
return [{
  json: {
    nearest: isWatchlist ? original.nearest : apply(original.nearest),
    overhead: original.overhead,
    watchlist_match: isWatchlist ? apply(original.watchlist_match) : original.watchlist_match,
    special_aircraft: original.special_aircraft,
    aircraft_count: original.aircraft_count,
    updated_at: original.updated_at,
    fa_usage: original.fa_usage,
  },
}];
""".strip()

# Values go in as Postgres query parameters ($1, $2 via options.queryReplacement),
# never pasted into the SQL text - 2026-09-24: an aircraft owner named
# "L'EAGLE AIR II INC" broke the old '{{ JSON.stringify($json) }}' literal
# (apostrophe ended the string -> syntax error). Parameters accept any character.
pg_query = (
    "INSERT INTO homeops.state_cache (key, value) "
    "VALUES ('aircraft', $1::jsonb) "
    "ON CONFLICT (key) DO UPDATE SET value = EXCLUDED.value, updated_at = now();"
)

fetch_mia_fids_query = "SELECT value FROM homeops.state_cache WHERE key = 'mia_fids';"
fetch_fa_usage_query = "SELECT value FROM homeops.state_cache WHERE key = 'fa_usage';"

# Historical/BI logging, added 2026-09-13, explicit request - every other
# write in this project so far has been an UPSERT onto a single "latest
# state" row (homeops.state_cache), so NOTHING about past sightings was
# ever retained - every 3s the same row just got overwritten. This is a
# real append-only table (homeops.aircraft_log, created directly via
# psql - see PROJECT.md) capturing one row per genuinely NEW sighting
# (same 6h dedup window as cacheV2, checked here independently via SQL
# rather than sharing that in-memory cache, since this fires from the
# final merged/published data regardless of which branch produced it).
build_log_entry_js = """
const original = $input.first().json;
const isWatchlist = !!original.watchlist_match;
const target = isWatchlist ? original.watchlist_match : original.nearest;

if (!target || !target.hex) {
  return []; // nothing currently visible this cycle - nothing to log
}

// 2026-09-14: aircraft_log moved off the shared 'streetwear' database
// entirely into its own dedicated 'skyops' database (see
// n8n/rebuild_weather_poll_workflow.py for the weather_log counterpart),
// explicit request - a real independent database, not just a separate
// schema, so it's easy to see/browse/query directly (pgAdmin, a BI tool)
// without another project's data in the way. Existing history (1429
// rows) migrated by hand via a COPY pipe between the two databases, not
// through this workflow. Real typed columns now instead of a single
// jsonb blob, also explicit request - each field below is pre-formatted
// as a ready-to-embed SQL literal (dollar-quoted text, or a bare number/
// NULL) rather than relying on the query's own quoting, same reasoning
// the old jsonb blob used $logtag$ for: free text (an airline name, a
// watchlist label) could in principle contain an apostrophe.
function sqlText(v) {
  if (v === null || v === undefined || v === '') return 'NULL';
  return '$fld$' + String(v) + '$fld$';
}
function sqlNum(v) {
  if (v === null || v === undefined || typeof v !== 'number' || Number.isNaN(v)) return 'NULL';
  return String(v);
}

return [{
  json: {
    hex: target.hex,
    // Empty string, not null, when a sighting has no callsign - keeps
    // the dedup query below a plain string comparison, no SQL NULL
    // handling needed.
    flight: target.flight || '',
    flight_number: sqlText(target.flight_number),
    airline: sqlText(target.airline),
    type: sqlText(target.type),
    type_desc: sqlText(target.desc),
    owner: sqlText(target.owner),
    origin_iata: sqlText(target.origin_iata),
    destination_iata: sqlText(target.destination_iata),
    // 'mia_fids' | 'flightaware' | 'adsbdb' | NULL (no route resolved at
    // all) - explicit request to log where each route actually came
    // from, not just what it says.
    route_source: sqlText(target.route_source),
    distance_mi: sqlNum(target.distance_mi),
    bearing_deg: sqlNum(target.bearing_deg),
    alt_ft: sqlNum(target.alt_ft),
    heading_deg: sqlNum(target.heading_deg),
    is_watchlist: isWatchlist,
    watchlist_label: isWatchlist ? sqlText(target.label) : 'NULL',
    overhead: !!original.overhead,
    special_kind: sqlText(original.special_aircraft ? original.special_aircraft.kind : null),
  },
}];
""".strip()

log_sighting_query = (
    "INSERT INTO aircraft_log ("
    "  hex, flight, flight_number, airline, type, type_desc, owner,"
    "  origin_iata, destination_iata, route_source, distance_mi,"
    "  bearing_deg, alt_ft, heading_deg, is_watchlist, watchlist_label,"
    "  overhead, special_kind"
    ") "
    "SELECT '{{ $json.hex }}', '{{ $json.flight }}', {{ $json.flight_number }},"
    "  {{ $json.airline }}, {{ $json.type }}, {{ $json.type_desc }}, {{ $json.owner }},"
    "  {{ $json.origin_iata }}, {{ $json.destination_iata }}, {{ $json.route_source }},"
    "  {{ $json.distance_mi }}, {{ $json.bearing_deg }}, {{ $json.alt_ft }},"
    "  {{ $json.heading_deg }}, {{ $json.is_watchlist }}, {{ $json.watchlist_label }},"
    "  {{ $json.overhead }}, {{ $json.special_kind }} "
    "WHERE NOT EXISTS ("
    "  SELECT 1 FROM aircraft_log"
    "  WHERE hex = '{{ $json.hex }}' AND flight = '{{ $json.flight }}'"
    "    AND seen_at > now() - interval '6 hours'"
    ");"
)

match_mia_fids_js = """
// Free-first route lookup, added 2026-09-12: MIA's own public FIDS feed
// (n8n/rebuild_mia_fids_poll_workflow.py, polled every 10 min, cached in
// Postgres) covers most of this project's office-window traffic - it's
// ordinary MIA-bound/outbound commercial flights, not private/cargo -
// for free, no per-query cost. Checking it here, BEFORE the paid
// FlightAware/adsbdb branch, means a MIA-matched flight never triggers
// either external call at all. Only runs the match when a lookup would
// otherwise be needed (_lookup.needed === 1 from "Nearest + Watchlist") -
// a cacheV2 hit already skipped needing any of this. Reads
// $('Nearest + Watchlist') directly rather than $input - this node's
// direct predecessor is "Fetch FA Usage" (a Postgres node, whose output
// REPLACES the item with just the query result columns, not a passthrough
// of the incoming aircraft data - same reason "Fetch MIA FIDS" is read
// via $() too, not $input).
// fa_usage rides through every path out of this node regardless of
// whether a lookup is even needed this cycle - it's for the on-device
// display (see aircraft_mqtt.c/systems_screen.c), not the lookup
// decision itself, so it's attached unconditionally right here rather
// than duplicated across this node's three separate return statements.
let faUsage = null;
try {
  const raw = $('Fetch FA Usage').first().json.value;
  faUsage = typeof raw === 'string' ? JSON.parse(raw) : raw;
} catch (e) {
  faUsage = null; // no cached row yet - not an error
}
// Only the two fields the firmware actually reads go out on MQTT. The full
// FlightAware usage object (resource_details etc., ~500 bytes) pushed a
// watchlist/747 payload to ~1.2KB - past esp-mqtt's default 1024-byte rx
// buffer, so the BOX-3 got it split across two DATA events, failed to
// parse the first half, and silently dropped the whole update (no chime,
// no screen) - DLH463 overhead 2026-09-22. faUsage itself stays full for
// the cost check below.
const faUsageSlim = faUsage ? { total_calls: faUsage.total_calls, total_cost: faUsage.total_cost } : null;
const original = { ...$('Nearest + Watchlist').first().json, fa_usage: faUsageSlim };

if (!original._lookup || original._lookup.needed !== 1) {
  return [{ json: original }];
}

// MIA's feed keys most entries by 2-letter IATA airline code + bare
// flight number (e.g. "AA552"), but readsb's callsigns are ICAO-prefixed
// (e.g. "AAL552") - this bridges the two. A handful of smaller/charter
// carriers without a real IATA designator show up keyed by their ICAO
// code directly instead (identity entries below) - confirmed by reading
// MIA's own airlineName field live rather than guessing.
//
// 2026-09-13: the six codes this table originally left unmapped out of
// caution (BBQ, BF, CSQ, G6, WAL, AZ) turned out all identifiable via
// MIA's own airlineName field + a quick web check, not actually
// unknowable - explicit request to maximize free-source coverage rather
// than defaulting to the paid API out of caution. ITY (ITA Airways, "AZ"
// in the feed - Alitalia's ICAO code before its 2021 rebrand, uncertain
// at the time) is now confirmed. BBQ (Eastern Air Express), CSQ (IBC
// Air - a MIA-based cargo carrier, a nice bonus beyond this feed's
// expected passenger-airline coverage), and WAL (World Atlantic) all use
// their own ICAO code as the feed's CXR value directly (no separate
// IATA designator these three actually use here) - identity entries.
// 2026-09-13, second pass: caught live via VOI1790 (Volaris) showing up
// FlightAware-sourced while clearly present on MIA's own board under its
// real IATA code Y4 - the 27-code table above still only covered a subset
// of what's actually flying into MIA. Diffed every distinct CXR code on
// the live board against this table's values and found 15 more gaps,
// covering the rest of the current board exactly (AS/RAM/BA/BWA/DM/H8/
// LH/LX/LY/QR/TP/UX/VB/VS/Y4) - each ICAO code confirmed via web search
// before adding (not guessed), same standard as the six added earlier.
const ICAO_TO_IATA = {
  AAL: 'AA', ACA: 'AC', AFR: 'AF', AMX: 'AM', ARG: 'AR', AVA: 'AV',
  CMP: 'CM', DAL: 'DL', UAE: 'EK', FFT: 'F9', GLO: 'G3', IBE: 'IB',
  CAY: 'KX', LAN: 'LA', ENY: 'MQ', SLM: 'PY', SCX: 'SY', THY: 'TK',
  UAL: 'UA', BHS: 'UP', SWA: 'WN',
  ITY: 'AZ', FBU: 'BF', GXA: 'G6',
  BBQ: 'BBQ', CSQ: 'CSQ', WAL: 'WAL',
  ASA: 'AS',  // Alaska Airlines
  RAM: 'AT',  // Royal Air Maroc
  BAW: 'BA',  // British Airways
  BWA: 'BW',  // Caribbean Airlines
  DWI: 'DM',  // Arajet
  SKX: 'H8',  // Sky Airline Peru
  DLH: 'LH',  // Lufthansa
  SWR: 'LX',  // SWISS
  ELY: 'LY',  // El Al
  QTR: 'QR',  // Qatar Airways
  TAP: 'TP',  // TAP Air Portugal
  AEA: 'UX',  // Air Europa
  VIV: 'VB',  // Viva Aerobus
  VIR: 'VS',  // Virgin Atlantic
  VOI: 'Y4',  // Volaris

  // 2026-09-13, third pass, caught live: TAI396 ("Avianca El Salvador" per
  // readsb's own db-file ownOp) still fell through to FlightAware even
  // though AVA was already mapped - a different class of gap than passes
  // one/two. Avianca Holdings' regional subsidiaries broadcast their OWN
  // legacy ICAO callsign prefix (pre-2013 brand unification: TACA/LACSA/
  // Aviateca), but MIA's board lists all of them under the unified
  // mainline Avianca commercial code (confirmed live for both of these by
  // checking the actual board entry for the exact flight: TAI396 -> CXR
  // AV, and LRC604 -> CXR AV, both plain "avianca" in airlineName, not a
  // separate subsidiary code) - so both need to map to 'AV', same as AVA,
  // not to their own legacy IATA codes (TA/LR). GUG (Avianca Guatemala/
  // Aviateca) added on the same pattern - not separately confirmed against
  // a live board entry the way the other two were (no Guatemala flight was
  // on the board at check time), but it's the third and last of Avianca
  // Holdings' regional units unified under the same brand per the same
  // 2013 restructuring, so treating it identically is a safe bet: if
  // MIA's board turns out to actually list it differently, this just
  // stays a harmless miss (falls through to FlightAware exactly as it
  // does today), not a misroute.
  TAI: 'AV',  // Avianca El Salvador (ex-TACA)
  LRC: 'AV',  // Avianca Costa Rica (ex-LACSA)
  GUG: 'AV',  // Avianca Guatemala (ex-Aviateca)

  // 2026-09-13, fourth pass, caught live: LNE1456 - same group-subsidiary
  // pattern as the Avianca fix above, this time LATAM. Confirmed directly
  // against the live board for this exact flight: LNE1456 -> CXR "LA",
  // airlineName plain "LATAM" (LATAM Ecuador, callsign prefix LNE, isn't
  // listed under its own designator). ARE (LATAM Colombia) had already
  // been seen going to FlightAware earlier today (ARE4400) - same
  // reasoning applies and confirmed here too. LPE (LATAM Peru), TAM
  // (LATAM Brasil) and LAP (LATAM Paraguay) added on the same pattern -
  // each subsidiary keeps its own legacy ICAO/IATA pair commercially
  // (e.g. TAM's own IATA is JJ, not LA), but the live board only ever
  // showed a bare "LA"/"LATAM" for every LATAM-group sighting checked
  // during this session, no separate JJ/4C/PZ code seen even once - not
  // independently confirmed per-subsidiary the way LNE/ARE were (no
  // Brazil/Paraguay flight happened to be on the board at check time),
  // same harmless-miss-if-wrong reasoning as GUG above.
  LNE: 'LA',  // LATAM Ecuador
  ARE: 'LA',  // LATAM Colombia
  LPE: 'LA',  // LATAM Peru
  TAM: 'LA',  // LATAM Brasil
  LAP: 'LA',  // LATAM Paraguay

  // 2026-09-14, caught live via ROU1645: Air Canada Rouge (ACA's own
  // leisure-brand subsidiary, own ICAO callsign prefix ROU) - same
  // group-subsidiary-uses-parent's-code pattern as Avianca/LATAM above.
  // Confirmed against the live board for this exact flight: ROU1645 ->
  // CXR "AC", airlineName plain "Air Canada", no separate Rouge code.
  ROU: 'AC',  // Air Canada Rouge

  // 2026-09-15, caught live via KLM628 (MIA -> AMS): confirmed via the
  // real MIA departures board (KLM 628 to Amsterdam, gate J11) that the
  // free-source data was right there in cache the whole time under its
  // IATA key "KL628" - it just never got looked up, because KLM's own
  // ICAO code was missing from this table entirely, not a subsidiary/
  // rebrand edge case like the others above. A plain oversight, not a
  // "no board entry" miss - the cache row existed
  // (homeops.state_cache -> 'mia_fids' -> "KL628") and was confirmed
  // directly via psql before adding this fix.
  KLM: 'KL',  // KLM Royal Dutch Airlines

  // 2026-09-15, proactive pass triggered by the KLM miss above: rather
  // than wait for each of these to cause its own live miss, diffed every
  // distinct IATA prefix actually present in the current mia_fids cache
  // (923 flights) against this table's values and found 5 more airlines
  // on the board with no ICAO entry at all - same class of gap as KLM,
  // not a subsidiary/rebrand edge case. Each ICAO code confirmed via web
  // search before adding, same standard as every other entry here.
  LVL: 'LL',  // Level (Spain, IAG) - its own ICAO/callsign since gaining
              // operational independence in late 2024, not still riding
              // on Iberia's certificate.
  SKU: 'H2',  // Sky Airline (Chile) - distinct from Sky Airline Peru
              // (SKX -> H8, already mapped above) - same "Sky" branding,
              // two unrelated airlines, two different ICAO/IATA pairs.
  LOT: 'LO',  // LOT Polish Airlines
  GLG: 'AV',  // Avianca Ecuador (ex-AeroGal) - 2026-09-24 audit: GLG7377/7378
              // MIA<->GYE sit on the board as AV7377/AV7378.
  BOV: 'OB',  // BoA (Boliviana de Aviacion)

  // XL (LATAM Ecuador) deliberately NOT added here: the cache has both
  // an "XL214" entry (airline "LATAM Ecuador") and the already-confirmed
  // LNE1456 -> CXR "LA" case above, under the same ICAO prefix (LNE).
  // A single static ICAO->IATA table can't resolve both - MIA's board
  // itself is inconsistent about which code it uses for this airline
  // across different flights/legs, not something guessable without
  // seeing which specific live callsign produced the XL214 entry. Left
  // as a harmless miss (falls through to FlightAware) rather than risk
  // breaking the already-confirmed LNE->LA case with a guess.
};

// Deliberately a separate copy from "Nearest + Watchlist"'s flightNumber()
// (used for on-screen display) - this one only needs to build MIA's exact
// lookup key, and MIA's own TRN field is always bare digits (confirmed
// live against the board: TP223/224/225/226 all plain numeric, no
// suffix). Caught live 2026-09-13 via TAP22MI (TAP Portugal's real LIS-MIA
// flight, callsign broadcasts a "MI" suffix readsb doesn't strip) still
// missing MIA despite TAP already being mapped - the display copy's
// leading-zero-only strip left "22MI" intact, which can never equal
// MIA's "22". Matching only the leading digit run (still consuming any
// leading zeros the same way) fixes this without touching what's shown
// on screen, which may legitimately want to keep a suffix like this.
// 2026-09-13, caught live again via AVA004: MIA's own TRN field isn't
// consistently padded across airlines - this exact flight's board entry
// is literally "004" (confirmed live), not "4", so the old single-value
// leading-zero-strip built the wrong key (AV4 instead of the real AV004)
// and missed a match that genuinely existed. But TP223/AA1816/etc. all
// have plain unpadded TRNs, so stripping can't just be dropped either -
// there's no one padding rule that covers every airline on this board.
// Returns both forms (raw digits as broadcast, and zero-stripped) so the
// caller can try each - order matters: raw first, since "004" is the more
// literal reading of what was actually broadcast and covers this case,
// falling back to stripped for the (more common) unpadded case.
// 2026-09-14, caught live again via TAP22MI - a genuinely different
// problem from the suffix/padding fixes above, not fixable by any digit
// -extraction regex. Checked the live board for this exact flight (same
// hex today as yesterday's TAP22MI, still broadcasting the identical
// callsign) and MIA only lists TAP's LIS-MIA round trip as TP223/TP224 -
// there's no "TP22" on the board at all, today or yesterday. "22MI" isn't
// a truncated/suffixed form of "223" (they don't even share a common
// digit prefix), so this must be a distinct ATC/operational callsign TAP
// files for this tail, unrelated by any string transform to the
// commercial flight number MIA actually publishes. A direct override is
// the only fix - same as identity-mapping GUG/BBQ/etc. above, just at the
// full-callsign level instead of the ICAO-prefix level. Checked first,
// ahead of the regex-based extraction, since it's a hard confirmed fact
// about this one callsign rather than a general rule.
// IBE03TM added 2026-09-14, same reasoning - confirmed live against the
// board (IB333, MAD arrival, scheduled 15:05 that day, sighted ~40min
// prior at 14:25 - consistent with a real inbound approach).
// IBE03TH added 2026-09-14, same reasoning, a different tail's
// operational callsign - confirmed live: sighted 21:09 UTC (5:09pm
// local), 0.7mi/1950ft/bearing 59deg (low, climbing, close - a plane a
// few minutes past takeoff, not on approach). The board's departures
// list that hour only had two IB flights: IB334 (MAD, ATD 4:50pm,
// scheduled 4:55pm) and IB338 (MAD, not yet departed, scheduled 10pm) -
// only IB334 was plausibly airborne and this close at 5:09pm, ~19min
// after its actual departure time. Other IBE03XX-shaped callsigns seen
// in the log (03LW, 03GR) are still NOT added - Iberia's operational-
// callsign scheme looks broader than just these two tails, but without
// live-board confirmation for those specific sightings (the board only
// shows the current day/window, not what it showed when those past
// sightings happened), guessing which commercial flight each maps to
// risks a wrong match (a real misroute, not just a harmless miss) - add
// those individually once actually confirmed live, same as these two.
const CALLSIGN_FLIGHT_NUMBER_OVERRIDES = {
  TAP22MI: '223',
  IBE03TM: '333',
  IBE03TH: '334',
  // 2026-09-24: BA's MIA->LHR operational callsigns. BAW6R = BA206 (A380,
  // ~17:15 dep, seen on 3 different tails), BAW34N = BA208 (787, evening).
  BAW6R: '206',
  BAW34N: '208',
  // BAW3G = BA207 LHR->MIA (A380, ~2:30pm arrival; seen 13:56/14:11,
  // user confirmed BA207 2:30pm vs BA209 8:30pm).
  BAW3G: '207',
  // 2026-09-24, from skyops history (all were FlightAware-sourced, paid):
  // IBE03GR = IB337 MAD->MIA (~19:30-20:00, user: IB337 lands 8:05pm);
  // IBE03LW = IB338 MIA->MAD (~22:20, only other IB departure - 334 is
  // IBE03TH); ITY6MO = AZ631 MIA->FCO (~20:00, only other AZ departure -
  // 633 broadcasts as-is ~23:15).
  IBE03GR: '337',
  IBE03LW: '338',
  ITY6MO: '631',
  // LVL230K = LL2630 MIA->BCN (~22:15, 4 sightings / 3 tails, board only
  // lists LL2630 for MIA->BCN; the digit run '230' never matched).
  LVL230K: '2630',
  // 2026-09-24 full audit (FlightAware-sourced repeats vs the board, by
  // route + time of day + aircraft type):
  // THY4JD = TK157 IST->MIA (787, ~07:00); THY41J = TK158 MIA->IST (787,
  // ~11:00, same aircraft turning); THY99D = TK078 MIA->IST (77W, ~23:00,
  // turn of THY77J = TK077 which already matches by padding).
  THY4JD: '157',
  THY41J: '158',
  THY99D: '078',
  FBU98B: '743',   // French bee MIA->ORY ~22:00 (board: BF743 only)
  LOT3PK: '030',   // LOT MIA->WAW ~20:00-22:30 (board: LO030 only)
  AAL870Q: '1870', // AA MSP->MIA ~10:30 (board: AA1870, only AA from MSP)
};

// 2026-09-15, caught live via VIR6J (Virgin Atlantic MIA->LHR): the
// opposite direction from the AV004 case above. That case was a
// callsign broadcasting WITH leading zeros ("004") that the old code
// incorrectly stripped away; this one broadcasts WITHOUT them ("6")
// while the board lists it zero-padded ("VS006") - confirmed directly
// via psql, the cache already had "VS006", the lookup just never tried
// that form. Same underlying fact as AV004's own comment (MIA's TRN
// field isn't consistently padded across airlines) just hitting the
// unpadded-to-padded direction this existing raw/stripped pair never
// covered. Adds a third candidate, zero-padded to 3 digits, only when
// the raw digit run is shorter than that - tried last since it's the
// least certain of the three (an assumption about this board's padding
// width, not something read directly off the callsign).
function miaFlightNumbers(flight) {
  if (!flight || flight.length <= 3) return [];
  const override = CALLSIGN_FLIGHT_NUMBER_OVERRIDES[flight];
  const m = flight.slice(3).match(/^(\\d+)/);
  if (!m) return override ? [override] : [];
  const raw = m[1];
  const stripped = raw.replace(/^0+/, '') || raw;
  const padded = raw.length < 3 ? raw.padStart(3, '0') : null;
  const numbers = [raw];
  if (stripped !== raw) numbers.push(stripped);
  if (padded && padded !== raw) numbers.push(padded);
  return override ? [override, ...numbers] : numbers;
}

let miaData = null;
try {
  const raw = $('Fetch MIA FIDS').first().json.value;
  miaData = typeof raw === 'string' ? JSON.parse(raw) : raw;
} catch (e) {
  miaData = null; // no cached row yet (e.g. before the poll workflow's first run) - not an error
}

const callsign = original._lookup.callsign || '';
const icaoPrefix = callsign.slice(0, 3);
const iataPrefix = ICAO_TO_IATA[icaoPrefix];
const bareNumbers = miaFlightNumbers(callsign);

let enrichment = null;
if (miaData && miaData.table && iataPrefix) {
  for (const num of bareNumbers) {
    const hit = miaData.table[iataPrefix + num];
    if (hit) {
      enrichment = hit;
      break;
    }
  }
}

if (!enrichment) {
  // No free MIA match - decide whether FlightAware is even allowed to
  // fire, using the REAL dollar figure fetched above (fa_usage.total_cost),
  // not a call-count guess. Hard monthly budget ceiling, explicit request -
  // stop just under it to leave a buffer for the up-to-10-minute staleness
  // between usage refreshes (this project's own observed call rate is
  // low enough that ~10 min of unaccounted calls is negligible,
  // nowhere near the buffer). Fails open (allowed) if fa_usage isn't
  // available yet (e.g. before the poll workflow's first fetch) - same
  // fail-open convention already used elsewhere in this file for a
  // missing cache row.
  const FA_MAX_COST_USD = 0.0; // your monthly FlightAware budget in USD (0 = paid lookups off)
  const underCap = !faUsage || typeof faUsage.total_cost !== 'number' || faUsage.total_cost < FA_MAX_COST_USD;
  const faAllowed = underCap && !original._lookup.localGA;
  return [{ json: { ...original, _lookup: { ...original._lookup, faAllowed } } }];
}

// Matched - behaves exactly like a fresh external-API enrichment from
// here on: written into the same cacheV2 the FlightAware/adsbdb path
// uses (so a continuously-visible sighting doesn't re-match MIA every
// cycle either) and needed flipped to 0 so "Needs Lookup?" routes to the
// existing "Use Cached Enrichment" path unchanged - no other node needed
// to know a MIA match happened at all.
const staticData = $getWorkflowStaticData('global');
if (!staticData.cacheV2) staticData.cacheV2 = {};
// route_source rides along in `cached`/cacheV2 the same way the route
// fields themselves do (via apply()'s spread in merge_enrichment_js/
// use_cached_js) - explicit request to log where each route actually
// came from, not just what it says.
const cached = { ...enrichment, route_source: 'mia_fids', cachedAt: Date.now() };
if (original._lookup.key) {
  staticData.cacheV2[original._lookup.key] = cached;
}

return [{
  json: {
    ...original,
    _lookup: { ...original._lookup, needed: 0, cached },
  },
}];
""".strip()

# Picks the nearest fresh-position aircraft within BLANK_CALLSIGN_RADIUS_MI
# that has no callsign yet and hasn't already been asked about (see the
# netCallsigns cache in nearest_code_js). Radius is a bit wider than the
# 5mi visibility filter so the answer is usually back before the plane is
# actually in view; bearing isn't checked, same reason.
blank_callsign_check_js = """
const BLANK_CALLSIGN_RADIUS_MI = 6;
const OFFICE_LAT = 0.0; // YOUR_LAT
const OFFICE_LON = 0.0; // YOUR_LON
const resp = $input.first().json;
const aircraft = Array.isArray(resp.aircraft) ? resp.aircraft : [];
const cache = ($getWorkflowStaticData('global').netCallsigns) || {};

function toRad(d) { return d * Math.PI / 180; }
function miles(lat1, lon1, lat2, lon2) {
  const dLat = toRad(lat2 - lat1), dLon = toRad(lon2 - lon1);
  const a = Math.sin(dLat / 2) ** 2 + Math.cos(toRad(lat1)) * Math.cos(toRad(lat2)) * Math.sin(dLon / 2) ** 2;
  return 3958.8 * 2 * Math.atan2(Math.sqrt(a), Math.sqrt(1 - a));
}

let best = null;
for (const a of aircraft) {
  if ((a.flight || '').trim() || !a.hex || cache[a.hex]) continue;
  if (typeof a.lat !== 'number' || typeof a.lon !== 'number') continue;
  if (typeof a.seen_pos === 'number' && a.seen_pos > 45) continue;
  const d = miles(OFFICE_LAT, OFFICE_LON, a.lat, a.lon);
  if (d <= BLANK_CALLSIGN_RADIUS_MI && (!best || d < best.d)) best = { hex: a.hex, d };
}
return [{ json: { blank_count: best ? 1 : 0, blank_hex: best ? best.hex : '' } }];
"""

nodes = [
    {
        "id": node_id(), "name": "Every 2s", "type": "n8n-nodes-base.scheduleTrigger",
        "typeVersion": 1.3, "position": [0, 0],
        # 3s -> 2s 2026-09-22: the view from the window lasts only ~3s before
        # the building across the street hides the plane, and the alert can
        # trail first receiver contact by up to one full interval. Measured
        # first: runs p50 126ms / p99 420ms, 2 of 7021 over 2s (the blank-
        # callsign lookup adds at most its 1.5s timeout, rarely).
        "parameters": {"rule": {"interval": [{"field": "seconds", "secondsInterval": 2}]}},
    },
    {
        "id": node_id(), "name": "Fetch aircraft.json", "type": "n8n-nodes-base.httpRequest",
        "typeVersion": 4.5, "position": [200, 0], "onError": "continueRegularOutput",
        # Static IP, not the "sky" hostname (confirmed 2026-09-11: PiLab
        # resolves it through the ISP router's DHCP-hostname DNS, which
        # dropped the mapping and silently broke aircraft tracking - the
        # device itself was fine the whole time, reachable instantly by
        # IP). Same reasoning as why Abbe/NAS were already checked by IP
        # in the health-check workflow.
        "parameters": {"url": "http://sky.local/tar1090/data/aircraft.json", "options": {"timeout": 2500}},
    },
    {
        "id": node_id(), "name": "Blank Callsign Check", "type": "n8n-nodes-base.code",
        "typeVersion": 2, "position": [300, 0],
        "parameters": {"language": "javaScript", "jsCode": blank_callsign_check_js},
    },
    {
        # Number comparison on purpose - boolean If conditions are unreliable
        # on this n8n (see Needs Lookup? / PROJECT.md).
        "id": node_id(), "name": "Has Blank Callsign?", "type": "n8n-nodes-base.if",
        "typeVersion": 2, "position": [400, 0],
        "parameters": {
            "conditions": {
                "options": {"caseSensitive": True, "leftValue": "", "typeValidation": "strict"},
                "conditions": [{
                    "id": node_id(),
                    "leftValue": "={{ $json.blank_count }}",
                    "rightValue": 0,
                    "operator": {"type": "number", "operation": "gt"},
                }],
                "combinator": "and",
            },
            "options": {},
        },
    },
    {
        # Free public ADS-B aggregator, no key. Tight timeout: this sits in
        # the 3s poll's critical path, and a slow answer is worth less than
        # a prompt unenriched publish (onError -> carry on without it).
        "id": node_id(), "name": "Network Callsign Lookup", "type": "n8n-nodes-base.httpRequest",
        "typeVersion": 4.5, "position": [500, -100], "onError": "continueRegularOutput",
        "parameters": {
            "url": "=https://api.adsb.lol/v2/hex/{{ $json.blank_hex }}",
            "options": {"timeout": 1500},
        },
    },
    {
        "id": node_id(), "name": "Nearest + Watchlist", "type": "n8n-nodes-base.code",
        "typeVersion": 2, "position": [400, 0],
        "parameters": {"language": "javaScript", "jsCode": nearest_code_js},
    },
    {
        # alwaysOutputData: true is load-bearing here, not cosmetic - a
        # zero-row SELECT (e.g. before the MIA FIDS poll workflow's very
        # first run) would otherwise produce zero output items and halt
        # this entire linear chain for the cycle, silently breaking the
        # immediate publish/cache path for whatever aircraft IS currently
        # visible. Always emitting one item (empty on no match) keeps the
        # rest of the pipeline running regardless.
        "id": node_id(), "name": "Fetch MIA FIDS", "type": "n8n-nodes-base.postgres",
        "typeVersion": 2.6, "position": [500, 200], "alwaysOutputData": True,
        "parameters": {"operation": "executeQuery", "query": fetch_mia_fids_query, "options": {}},
        "credentials": {"postgres": POSTGRES_CRED},
    },
    {
        # Same alwaysOutputData reasoning as "Fetch MIA FIDS" - a missing
        # row (e.g. before the poll workflow's first FlightAware-usage
        # fetch) must not stall this pipeline. Chained after "Fetch MIA
        # FIDS" (not parallel) - same execution-order lesson learned
        # building the MIA FIDS poll workflow itself: two Postgres reads
        # both feeding "Match MIA FIDS" in parallel would race the same
        # way the two HTTP fetches did there.
        "id": node_id(), "name": "Fetch FA Usage", "type": "n8n-nodes-base.postgres",
        "typeVersion": 2.6, "position": [530, 200], "alwaysOutputData": True,
        "parameters": {"operation": "executeQuery", "query": fetch_fa_usage_query, "options": {}},
        "credentials": {"postgres": POSTGRES_CRED},
    },
    {
        "id": node_id(), "name": "Match MIA FIDS", "type": "n8n-nodes-base.code",
        "typeVersion": 2, "position": [560, 0],
        "parameters": {"language": "javaScript", "jsCode": match_mia_fids_js},
    },
    {
        "id": node_id(), "name": "Cache Immediate", "type": "n8n-nodes-base.postgres",
        "typeVersion": 2.6, "position": [600, -220],
        "parameters": {"operation": "executeQuery", "query": pg_query, "options": {"queryReplacement": "={{ [JSON.stringify($json)] }}"}},
        "credentials": {"postgres": POSTGRES_CRED},
    },
    {
        "id": node_id(), "name": "Publish Immediate", "type": "n8n-nodes-base.mqtt",
        "typeVersion": 1, "position": [600, -340],
        "parameters": {"topic": "homeops/aircraft/state", "sendInputData": True, "options": {"retain": True}},
        "credentials": {"mqtt": MQTT_CRED},
    },
    {
        "id": node_id(), "name": "Needs Lookup?", "type": "n8n-nodes-base.if",
        "typeVersion": 2, "position": [600, 0],
        "parameters": {
            "conditions": {
                "options": {"caseSensitive": True, "leftValue": "", "typeValidation": "strict"},
                "conditions": [{
                    "id": node_id(),
                    "leftValue": "={{ $json._lookup.needed }}",
                    "rightValue": 0,
                    "operator": {"type": "number", "operation": "gt"},
                }],
                "combinator": "and",
            },
            "options": {},
        },
    },
    {
        "id": node_id(), "name": "Lookup Route", "type": "n8n-nodes-base.httpRequest",
        "typeVersion": 4.5, "position": [820, -100], "onError": "continueRegularOutput",
        "parameters": {
            "url": "=https://api.adsbdb.com/v0/callsign/{{ $('Nearest + Watchlist').first().json._lookup.callsign }}",
            "options": {"timeout": 3000},
        },
    },
    {
        # Hard monthly budget ceiling (see "Match MIA FIDS" -
        # FA_MAX_COST_USD, checked against FlightAware's own real
        # total_cost, not a call-count guess) - explicit request 2026-09-13,
        # replacing the earlier call-count-based cap. Only gates the paid
        # FlightAware call; adsbdb (free) always fires regardless.
        "id": node_id(), "name": "Under FlightAware Cap?", "type": "n8n-nodes-base.if",
        "typeVersion": 2, "position": [820, -280],
        "parameters": {
            "conditions": {
                "options": {"caseSensitive": True, "leftValue": "", "typeValidation": "strict"},
                "conditions": [{
                    "id": node_id(),
                    "leftValue": "={{ $json._lookup.faAllowed }}",
                    "rightValue": True,
                    "operator": {"type": "boolean", "operation": "true", "singleValue": True},
                }],
                "combinator": "and",
            },
            "options": {},
        },
    },
    {
        # Primary route source (see merge_enrichment_js) - a dead-end
        # branch read via $('Lookup Route (FlightAware)') in Merge
        # Enrichment, same cross-reference pattern already used there for
        # "Nearest + Watchlist". onError continues so a bad/unknown ident
        # (400/404) or an AeroAPI outage falls through to adsbdb instead
        # of failing the whole execution. Only reached when "Under
        # FlightAware Cap?" is true - the monthly call cap is checked
        # before ever making this paid call, not after.
        "id": node_id(), "name": "Lookup Route (FlightAware)", "type": "n8n-nodes-base.httpRequest",
        "typeVersion": 4.5, "position": [1020, -280], "onError": "continueRegularOutput",
        "parameters": {
            "url": "=https://aeroapi.flightaware.com/aeroapi/flights/{{ $('Nearest + Watchlist').first().json._lookup.callsign }}",
            "sendHeaders": True,
            "headerParameters": {"parameters": [{"name": "x-apikey", "value": FLIGHTAWARE_KEY}]},
            "sendQuery": True,
            "queryParameters": {"parameters": [{"name": "max_pages", "value": "1"}]},
            "options": {"timeout": 3000},
        },
    },
    {
        "id": node_id(), "name": "Merge Enrichment", "type": "n8n-nodes-base.code",
        "typeVersion": 2, "position": [1220, -100],
        "parameters": {"language": "javaScript", "jsCode": merge_enrichment_js},
    },
    {
        "id": node_id(), "name": "Use Cached Enrichment", "type": "n8n-nodes-base.code",
        "typeVersion": 2, "position": [820, 100],
        "parameters": {"language": "javaScript", "jsCode": use_cached_js},
    },
    {
        "id": node_id(), "name": "Merge", "type": "n8n-nodes-base.merge",
        "typeVersion": 3, "position": [1420, 0],
        "parameters": {"mode": "append"},
    },
    {
        "id": node_id(), "name": "Cache Latest", "type": "n8n-nodes-base.postgres",
        "typeVersion": 2.6, "position": [1620, -80],
        "parameters": {"operation": "executeQuery", "query": pg_query, "options": {"queryReplacement": "={{ [JSON.stringify($json)] }}"}},
        "credentials": {"postgres": POSTGRES_CRED},
    },
    {
        "id": node_id(), "name": "Publish Aircraft State", "type": "n8n-nodes-base.mqtt",
        "typeVersion": 1, "position": [1620, 80],
        "parameters": {"topic": "homeops/aircraft/state", "sendInputData": True, "options": {"retain": True}},
        "credentials": {"mqtt": MQTT_CRED},
    },
    {
        "id": node_id(), "name": "Build Log Entry", "type": "n8n-nodes-base.code",
        "typeVersion": 2, "position": [1620, 220],
        "parameters": {"language": "javaScript", "jsCode": build_log_entry_js},
    },
    {
        "id": node_id(), "name": "Log Sighting", "type": "n8n-nodes-base.postgres",
        "typeVersion": 2.6, "position": [1840, 220],
        "parameters": {"operation": "executeQuery", "query": log_sighting_query, "options": {}},
        "credentials": {"postgres": POSTGRES_SKYOPS_CRED},
    },
]

connections = {
    "Every 2s": {"main": [[{"node": "Fetch aircraft.json", "type": "main", "index": 0}]]},
    "Fetch aircraft.json": {"main": [[{"node": "Blank Callsign Check", "type": "main", "index": 0}]]},
    "Blank Callsign Check": {"main": [[{"node": "Has Blank Callsign?", "type": "main", "index": 0}]]},
    "Has Blank Callsign?": {
        "main": [
            [{"node": "Network Callsign Lookup", "type": "main", "index": 0}],
            [{"node": "Nearest + Watchlist", "type": "main", "index": 0}],
        ]
    },
    "Network Callsign Lookup": {"main": [[{"node": "Nearest + Watchlist", "type": "main", "index": 0}]]},
    "Nearest + Watchlist": {"main": [[{"node": "Fetch MIA FIDS", "type": "main", "index": 0}]]},
    "Fetch MIA FIDS": {"main": [[{"node": "Fetch FA Usage", "type": "main", "index": 0}]]},
    "Fetch FA Usage": {"main": [[{"node": "Match MIA FIDS", "type": "main", "index": 0}]]},
    "Match MIA FIDS": {"main": [[{"node": "Needs Lookup?", "type": "main", "index": 0}]]},
    "Needs Lookup?": {
        "main": [
            [
                {"node": "Lookup Route", "type": "main", "index": 0},
                {"node": "Under FlightAware Cap?", "type": "main", "index": 0},
                {"node": "Cache Immediate", "type": "main", "index": 0},
                {"node": "Publish Immediate", "type": "main", "index": 0},
            ],
            [{"node": "Use Cached Enrichment", "type": "main", "index": 0}],
        ]
    },
    "Lookup Route": {"main": [[{"node": "Merge Enrichment", "type": "main", "index": 0}]]},
    "Under FlightAware Cap?": {
        "main": [
            [{"node": "Lookup Route (FlightAware)", "type": "main", "index": 0}],
            [],
        ]
    },
    "Merge Enrichment": {"main": [[{"node": "Merge", "type": "main", "index": 0}]]},
    "Use Cached Enrichment": {"main": [[{"node": "Merge", "type": "main", "index": 1}]]},
    "Merge": {
        "main": [
            [
                {"node": "Cache Latest", "type": "main", "index": 0},
                {"node": "Publish Aircraft State", "type": "main", "index": 0},
                {"node": "Build Log Entry", "type": "main", "index": 0},
            ]
        ]
    },
    "Build Log Entry": {"main": [[{"node": "Log Sighting", "type": "main", "index": 0}]]},
}

workflow = {
    "name": "Home Ops - Aircraft Poll",
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

req = urllib.request.Request(
    f"{N8N_URL}/api/v1/workflows/{WF}",
    data=json.dumps(workflow).encode(),
    headers={"X-N8N-API-KEY": API_KEY, "Content-Type": "application/json"},
    method="PUT",
)

with urllib.request.urlopen(req) as resp:
    result = json.load(resp)
    print("Updated. Active:", result["active"])
