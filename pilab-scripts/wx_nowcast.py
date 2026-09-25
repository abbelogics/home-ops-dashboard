#!~/wxenv/bin/python
"""Real-time "is it raining at the office right now" from NOAA MRMS radar.

Runs on PiLab as the always-on systemd service `wx-nowcast` (was a 2-min
cron job until later on 2026-09-24), inside ~/wxenv (a venv with numpy,
requests, eccodes + eccodeslib - nothing installed system-wide). Checks for
a new MRMS frame every POLL_SEC and processes it as soon as NOAA publishes
it. Writes one row to homeops.state_cache under key 'wx_nowcast', which the
Home Ops Weather Poll reads to decide the rain part of the weather icon.

Latency (explicit "reduce the time" request): NOAA publishes each frame
~2-3 min after its valid time - that part is out of our control. Everything
after it is near-zero: when the rain state changes (dry/light/moderate/
heavy, or lightning likely), this pings the Weather Poll's webhook so the
BOX-3 updates immediately instead of waiting for the 5-min schedule.

Why MRMS (2026-09-24): Open-Meteo is a model, and Miami's pop-up cells are
often < 2 km wide and gone in 15-30 min - models get them wrong in place
and time ("shows rain but it's sunny" and vice versa). The 2026-09-11
RainViewer attempt was spatially fine but ~10-15 min behind real time.
MRMS (NOAA's Multi-Radar Multi-Sensor mosaic, free on AWS) is 0.01 deg
(~1 km) cells, a new frame every 2 min, published ~2-3 min after valid time.

Products (all CONUS, same 7000x3500 0.01-deg grid):
  PrecipRate_00.00 - surface rain rate, mm/h (always fetched)
  PrecipFlag_00.00 - precip type (only fetched when rain is nearby)
  LightningProbabilityNext30minGrid_scale_1 - % chance of lightning within
      30 min (only fetched when rain is nearby)
Negative values are MRMS "no coverage / missing" flags.
"""
import datetime
import gzip
import json
import re
import subprocess
import sys
import time

import eccodes
import numpy as np
import requests

# User-calibrated office location (2026-09-24) - same point everywhere.
LAT, LON = 0.0, 0.0  # YOUR_LAT, YOUR_LON

BUCKET = "https://noaa-mrms-pds.s3.amazonaws.com"
DB_CONTAINER, DB_USER, DB_NAME = "lab-postgres-dev", "labuser", "streetwear"
CACHE_KEY = "wx_nowcast"
POLL_SEC = 20
# Home Ops - Weather Poll's webhook trigger (n8n on this same host).
WEATHER_REFRESH_URL = "http://localhost:5678/webhook/homeops-weather-refresh"

# ~1 km cells: radius 1 = 3x3 (~3 km box), radius 5 = 11x11 (~11 km box).
NEAR_RADIUS = 1
AREA_RADIUS = 5
# Nearest-rain search for the BOX-3 weather page ("rain 4 mi away"):
# radius 16 = a 33x33 box, ~16 km (~10 mi) each way.
NEAREST_RADIUS = 16
RAIN_MMHR = 0.2  # same "it's actually raining" floor as the Weather Poll

PRECIP_TYPES = {
    0: "none", 1: "warm stratiform rain", 3: "snow", 6: "convective rain",
    7: "rain mixed with hail", 10: "cool stratiform rain",
    91: "tropical/stratiform rain", 96: "tropical/convective rain",
}


def latest_key(product: str) -> str:
    """Newest object key for a product. Lists only the last ~30 min so the
    1000-key page limit never truncates to an old frame."""
    now = datetime.datetime.now(datetime.timezone.utc)
    for day in (now, now - datetime.timedelta(days=1)):
        prefix = f"CONUS/{product}/{day:%Y%m%d}/"
        start = (now - datetime.timedelta(minutes=30)).strftime("%Y%m%d-%H%M%S")
        xml = requests.get(
            f"{BUCKET}/?list-type=2&prefix={prefix}&start-after={prefix}MRMS_{product}_{start}",
            timeout=20,
        ).text
        keys = re.findall(r"<Key>([^<]+)</Key>", xml)
        if keys:
            return keys[-1]
    raise RuntimeError(f"no recent {product} frames found")


def sample(product: str, key: str = None):
    """Returns (frame_time_utc, point_value, near_max, area_max, area_box,
    nearest_mi) - nearest_mi = distance to the closest cell >= RAIN_MMHR
    within NEAREST_RADIUS, or None."""
    key = key or latest_key(product)
    raw = gzip.decompress(requests.get(f"{BUCKET}/{key}", timeout=30).content)
    g = eccodes.codes_new_from_message(raw)
    try:
        ni, nj = eccodes.codes_get(g, "Ni"), eccodes.codes_get(g, "Nj")
        la1 = eccodes.codes_get(g, "latitudeOfFirstGridPointInDegrees")
        lo1 = eccodes.codes_get(g, "longitudeOfFirstGridPointInDegrees")
        di = eccodes.codes_get(g, "iDirectionIncrementInDegrees")
        dj = eccodes.codes_get(g, "jDirectionIncrementInDegrees")
        vals = eccodes.codes_get_values(g).reshape(nj, ni)
    finally:
        eccodes.codes_release(g)
    j = round((la1 - LAT) / dj)
    i = round((LON % 360 - lo1) / di)

    def box_max(r):
        b = vals[j - r:j + r + 1, i - r:i + r + 1]
        return float(b.max())

    stamp = re.search(r"_(\d{8}-\d{6})\.grib2", key).group(1)
    frame = datetime.datetime.strptime(stamp, "%Y%m%d-%H%M%S").replace(tzinfo=datetime.timezone.utc)
    area_box = vals[j - AREA_RADIUS:j + AREA_RADIUS + 1, i - AREA_RADIUS:i + AREA_RADIUS + 1].copy()
    r = NEAREST_RADIUS
    big = vals[j - r:j + r + 1, i - r:i + r + 1]
    ys, xs = np.nonzero(big >= RAIN_MMHR)
    nearest_mi = None
    if ys.size:
        km_per_deg = 111.2
        dy_km = (ys - r) * dj * km_per_deg
        dx_km = (xs - r) * di * km_per_deg * np.cos(np.radians(LAT))
        nearest_mi = round(float(np.sqrt(dy_km ** 2 + dx_km ** 2).min()) / 1.609, 1)
    return frame, float(vals[j, i]), box_max(NEAR_RADIUS), box_max(AREA_RADIUS), area_box, nearest_mi


def clean(v):
    """MRMS negative values are 'no coverage / missing' flags, not data."""
    return None if v is None or v < 0 else round(v, 2)


def write_state_cache(value: dict):
    # JSON goes in as a psql variable (:'v' is quoted by psql itself), never
    # pasted into the SQL text - any character in the value is safe.
    sql = (
        f"INSERT INTO homeops.state_cache (key, value, updated_at) VALUES ('{CACHE_KEY}', :'v'::jsonb, now()) "
        "ON CONFLICT (key) DO UPDATE SET value = EXCLUDED.value, updated_at = now();"
    )
    subprocess.run(
        ["docker", "exec", "-i", DB_CONTAINER, "psql", "-v", "ON_ERROR_STOP=1", "-v", f"v={json.dumps(value)}",
         "-U", DB_USER, "-d", DB_NAME],
        input=sql, check=True, capture_output=True, text=True,
    )


def rain_state(out: dict) -> str:
    """Coarse state the display actually cares about - a change in this is
    what triggers an immediate weather refresh (same thresholds as the
    Weather Poll's Format Weather node)."""
    rate = out["rate_mmhr"] or 0
    cls = "dry" if rate < 0.2 else "light" if rate < 2.5 else "moderate" if rate <= 7.6 else "heavy"
    if cls != "dry" and (out["lightning_prob_30min"] or 0) >= 50:
        cls += "+lightning"
    return cls


def process(key: str) -> dict:
    now = datetime.datetime.now(datetime.timezone.utc)
    frame, point, near, area, _, nearest_mi = sample("PrecipRate_00.00", key)
    out = {
        "frame_time": frame.isoformat(),
        "frame_age_sec": int((now - frame).total_seconds()),
        "rate_mmhr": clean(point),          # the office's own ~1 km cell
        "rate_near_max_mmhr": clean(near),  # ~3 km box around it
        "rate_area_max_mmhr": clean(area),  # ~11 km box - "rain nearby"
        "nearest_rain_mi": nearest_mi,      # closest rain within ~10 mi
        "precip_type": None,
        "lightning_prob_30min": None,
        "checked_at": now.isoformat(),
    }
    if (clean(area) or 0) > 0:
        try:
            _, flag, _, _, box, _ = sample("PrecipFlag_00.00")
            # The office cell's own type when it's precipitating there;
            # otherwise the most common type among nearby precipitating
            # cells (the flag is a category, so max() would be meaningless).
            if flag <= 0:
                wet = box[box > 0].astype(int)
                flag = float(np.bincount(wet).argmax()) if wet.size else flag
            if flag > 0:
                out["precip_type"] = PRECIP_TYPES.get(int(flag), f"code {int(flag)}")
        except Exception as e:  # type is a nice-to-have, rate is what matters
            print(f"PrecipFlag failed: {e}", file=sys.stderr)
        try:
            _, lp, lp_near, _, _, _ = sample("LightningProbabilityNext30minGrid_scale_1")
            out["lightning_prob_30min"] = clean(max(lp, lp_near))
        except Exception as e:
            print(f"Lightning failed: {e}", file=sys.stderr)
    write_state_cache(out)
    return out


def main():
    last_key, last_state = None, None
    while True:
        try:
            key = latest_key("PrecipRate_00.00")
            if key != last_key:
                out = process(key)
                last_key = key
                state = rain_state(out)
                print(f"{out['frame_time']} age={out['frame_age_sec']}s rate={out['rate_mmhr']} "
                      f"area_max={out['rate_area_max_mmhr']} state={state}", flush=True)
                if last_state is not None and state != last_state:
                    try:
                        requests.get(WEATHER_REFRESH_URL, timeout=10)
                        print(f"rain state {last_state} -> {state}: weather refresh triggered", flush=True)
                    except Exception as e:
                        print(f"weather refresh webhook failed: {e}", file=sys.stderr, flush=True)
                last_state = state
        except Exception as e:
            # Network/NOAA blip: keep the last good row, try again next tick.
            # The Weather Poll falls back to the model once the row is stale.
            print(f"cycle failed: {e}", file=sys.stderr, flush=True)
        time.sleep(POLL_SEC)


if __name__ == "__main__":
    main()
