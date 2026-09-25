#pragma once

/* Audible beep for the Home screen's notification strip - explicit
 * request 2026-09-15 ("I need a beep or notification that something is
 * going on"). Call notification_sound_init() once at boot, after
 * lights_voice_init() (it reuses that call's already-initialized 16kHz
 * I2S bus rather than re-initializing it - see notification_sound.c's
 * own comment for why that matters). notification_sound_beep() is safe
 * to call from any task, including while already holding the display
 * lock - it just queues the request and returns immediately; a
 * dedicated task does the actual (blocking) playback. */
void notification_sound_init(void);
void notification_sound_beep(void);
