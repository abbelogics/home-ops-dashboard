#!/bin/bash
# PiLab Wi-Fi self-heal watchdog - runs every minute as root from
# wifi-watchdog.timer (systemd, on PiLab itself, so a Wi-Fi reset never
# depends on an SSH session that the reset would kill - see the 2026-08-26
# incident in PROJECT.md).
#
# Why (2026-09-27): PiLab went "half-deaf" on Wi-Fi - up, containers fine,
# still answering sky (which had it cached in ARP), but new ARP lookups from
# the Mac and the BOX-3 got no answer, so the BOX-3 sat on "PiLab is offline"
# for 30 min until a manual power-cycle. Wi-Fi power save was on (now off,
# persisted in the YOUR_WIFI_CONNECTION connection) and is the likely cause; this is
# the backstop if it ever happens again.
#
# Probes:
#   1. gateway: arping the router - catches a full Wi-Fi drop.
#   2. half-deaf: the BOX-3 has been off Mosquitto > BOX3_GRACE_SEC while
#      PiLab can still ping it (outbound works, inbound doesn't). PiLab
#      pinging it proves the BOX-3 is alive, so the missing MQTT session
#      points at PiLab's side.
# Actions (escalating):
#   - reconnect Wi-Fi (at most once per RECONNECT_COOLDOWN_SEC)
#   - reboot if still failing REBOOT_AFTER_SEC after a reconnect (at most
#     once per REBOOT_COOLDOWN_SEC, never inside the close/backup windows)
# DRY_RUN=1 logs what it would do without doing it.

set -u
GATEWAY=YOUR_GATEWAY_IP
BOX3_IP=box3.local
WIFI_CONN=YOUR_WIFI_CONNECTION
GATEWAY_FAIL_LIMIT=3          # consecutive 1-min failures before acting
BOX3_GRACE_SEC=300
RECONNECT_COOLDOWN_SEC=1800
REBOOT_AFTER_SEC=600
REBOOT_COOLDOWN_SEC=21600
STATE_DIR=/var/lib/wifi-watchdog
DRY_RUN=${DRY_RUN:-0}

mkdir -p "$STATE_DIR"
now=$(date +%s)
get() { cat "$STATE_DIR/$1" 2>/dev/null || echo "${2:-0}"; }
put() { echo "$2" > "$STATE_DIR/$1"; }
log() { echo "wifi-watchdog: $*"; }

# Keep power save off even if something re-enables it.
if /usr/sbin/iw dev wlan0 get power_save 2>/dev/null | grep -q "on"; then
    log "power save was ON - turning it off"
    [ "$DRY_RUN" = 1 ] || /usr/sbin/iw dev wlan0 set power_save off
fi

# --- probe 1: gateway -------------------------------------------------------
problem=""
if arping -q -c 2 -w 4 -I wlan0 "$GATEWAY" >/dev/null 2>&1; then
    put gw_fail 0
else
    n=$(( $(get gw_fail) + 1 )); put gw_fail "$n"
    log "gateway $GATEWAY not answering ARP ($n/$GATEWAY_FAIL_LIMIT)"
    [ "$n" -ge "$GATEWAY_FAIL_LIMIT" ] && problem="gateway unreachable for ${n} min"
fi

# --- probe 2: half-deaf (BOX-3 alive but off the broker) --------------------
if [ -z "$problem" ]; then
    last=$(docker exec lab-mosquitto sh -c 'grep -a "box3-dashboard" /mosquitto/log/mosquitto.log | tail -1' 2>/dev/null)
    last_ts=${last%%:*}
    if [ -n "$last" ] && ! echo "$last" | grep -q "New client connected" \
       && [ $(( now - last_ts )) -gt "$BOX3_GRACE_SEC" ] \
       && ping -c 2 -W 2 "$BOX3_IP" >/dev/null 2>&1; then
        problem="BOX-3 pingable but off MQTT for $(( (now - last_ts) / 60 )) min"
    fi
fi

if [ -z "$problem" ]; then
    if [ "$(get reconnected_at)" != 0 ]; then
        log "healthy again after reconnect at $(date -d @"$(get reconnected_at)" +%H:%M)"
        put reconnected_at 0
    fi
    exit 0
fi

# --- act ----------------------------------------------------------------------
reconnected_at=$(get reconnected_at)
if [ "$reconnected_at" = 0 ]; then
    if [ $(( now - $(get last_reconnect) )) -lt "$RECONNECT_COOLDOWN_SEC" ]; then
        log "$problem - reconnect cooldown active, waiting"
        exit 0
    fi
    log "$problem - reconnecting Wi-Fi"
    put last_reconnect "$now"; put reconnected_at "$now"; put gw_fail 0
    if [ "$DRY_RUN" != 1 ]; then
        nmcli connection up "$WIFI_CONN" ifname wlan0 >/dev/null 2>&1 \
            || { nmcli radio wifi off; sleep 3; nmcli radio wifi on; }
        sleep 5
        /usr/sbin/iw dev wlan0 set power_save off 2>/dev/null
    fi
    exit 0
fi

if [ $(( now - reconnected_at )) -lt "$REBOOT_AFTER_SEC" ]; then
    log "$problem - reconnected $(( (now - reconnected_at) / 60 )) min ago, giving it time"
    exit 0
fi

hm=$(date +%H%M)
if [ "$hm" -ge 2350 ] || [ "$hm" -le 15 ] || { [ "$hm" -ge 255 ] && [ "$hm" -le 330 ]; }; then
    log "$problem - reboot deferred (financial close / backup window)"
    exit 0
fi
if [ $(( now - $(get last_reboot) )) -lt "$REBOOT_COOLDOWN_SEC" ]; then
    log "$problem - still failing after reconnect, but rebooted < 6 h ago; not rebooting again"
    exit 0
fi
log "$problem - still failing $(( (now - reconnected_at) / 60 )) min after reconnect, REBOOTING"
put last_reboot "$now"; put reconnected_at 0
[ "$DRY_RUN" = 1 ] || systemctl reboot
