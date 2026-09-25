#pragma once

/* Non-blocking - enqueues a short, pleasant three-note chime for the
 * office speaker. `kind` (e.g. "747", "a380", "military") is only used
 * for logging - same sound for all three, by request. Exception: kind
 * "f1" plays the F1 "start lights" sequence instead (home_screen.c's F1
 * session alerts, 2026-09-24). Safe to
 * call from the MQTT client's own task.
 *
 * No separate init function - the worker task and speaker are both
 * created lazily, on the first call, rather than at boot. Two real bugs
 * forced this: an eager task created right after lights_voice_init()
 * couldn't find a free contiguous block of internal RAM (AFE/MultiNet
 * leave it chronically tight - confirmed via logging, not assumed), and
 * putting that task's stack in internal RAM at all was fragile regardless
 * of timing (a too-small stack overflowed into a full system panic once
 * it actually ran the speaker-driver code). The stack now lives in PSRAM
 * (xTaskCreateWithCaps, MALLOC_CAP_SPIRAM) instead of internal DRAM, and
 * creation is deferred to first use - both because a real aircraft
 * sighting happens well after boot has settled, and because there's no
 * reason to pay any setup cost before it's actually needed. */
void aircraft_alert_notify(const char *kind);
