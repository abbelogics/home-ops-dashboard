#pragma once

#include <stdbool.h>

#include "lvgl.h"

/* Screens cycle in this order when the display is tapped. */
#define SCREEN_COUNT 11

/* Dimmed level for this always-on ambient display - shared between
 * main.c (the initial value at boot) and home_screen.c (restored on
 * every wake from motion-sleep). One constant, not two copies of the
 * same magic number - see the wake-from-sleep call site for why that
 * used to matter: bsp_display_backlight_on() hardcodes 100%, so before
 * this was fixed 2026-09-14, every wake silently discarded whatever dim
 * level was set at boot. 35 -> 25, explicit request 2026-09-15, "too
 * bright at night". */
#define DISPLAY_BRIGHTNESS_PCT 20 /* 25 -> 20, explicit request 2026-09-24 */
/* Night mode (2026-09-24, see wifi_time_is_night()): a motion wake between
 * 23:00 and 07:00 only comes up to this, not DISPLAY_BRIGHTNESS_PCT. */
#define DISPLAY_NIGHT_BRIGHTNESS_PCT 5
/* Idle (no motion/touch) levels - 2026-09-27, explicit request: day idle
 * dims to 5% (was 1%), night idle to 1%. Touch stays live at both. */
#define DISPLAY_IDLE_BRIGHTNESS_PCT 5
#define DISPLAY_NIGHT_IDLE_BRIGHTNESS_PCT 1

/* Secondary text (dates, subtitles, labels, footers) on every screen -
 * 2026-09-25, explicit request: LVGL's standard grey (#9E9E9E) was hard to
 * read on the device. #BDBDBD first (from a side-by-side mockup), then
 * lightened to #D0D0D0 the same day after seeing it on the panel. Text
 * only - page dots, bar tracks and button backgrounds keep their own greys. */
#define UI_TEXT_SECONDARY lv_color_hex(0xD0D0D0)

/* 2026-09-15: Claude Usage and FX screens removed entirely (both added
 * 2026-09-14) - freeing their permanently-resident LVGL widgets/images
 * was part of fixing a same-day internal-RAM exhaustion bug (systems_mqtt
 * and lights_mqtt's esp_mqtt_client_start() were failing outright - see
 * main.c's lights_voice_init() comment and PROJECT.md for the full
 * story). Claude Usage was never wired to real data anyway. FX was real
 * (see git history / PROJECT.md if reviving it) but not worth the
 * standing RAM cost on this device. */
typedef enum {
    SCREEN_HOME = 0,
    /* Added 2026-09-24, right after Home (explicit request) - detailed
     * weather: now + radar line + next 6 hours + 3 days. See
     * weather_screen.c. */
    SCREEN_WEATHER,
    SCREEN_SYSTEMS,
    SCREEN_LIGHTS,
    SCREEN_FLIGHTAWARE,
    /* Added 2026-09-15, at the end of the cycle - own screen for the
     * on-device 2FA code (see totp.h/totp_screen.c), explicit request. */
    SCREEN_TOTP,
    /* Added 2026-09-15, at the end of the cycle - own screen for Claude
     * Pro/Max usage (session/weekly %), explicit request. A second
     * attempt at this same idea as the original 2026-09-14 Claude Usage
     * screen (removed same day as part of the RAM-exhaustion fix) - that
     * one was never wired to real data because the reference UI turned
     * out to be claude.ai's own private authenticated page. This one
     * uses a genuinely different, real data source instead - see
     * claude_usage_mqtt.h. */
    SCREEN_CLAUDE_USAGE,
    /* Added 2026-09-22, right after SCREEN_CLAUDE_USAGE - OpenAI (Codex)
     * usage (5h/weekly %), same idea and layout as SCREEN_CLAUDE_USAGE,
     * explicit request to keep the two usage screens adjacent. See
     * codex_usage_mqtt.h for the real data source - internal file/topic
     * names stay "codex" (that's the actual product this data comes
     * from), only the on-screen title says "OpenAI Usage". */
    SCREEN_CODEX_USAGE,
    /* Restored 2026-09-24, right after the OpenAI usage screen (explicit
     * request) - 1 USD in COP (TRM) / EUR / GBP (ECB), see fx_mqtt.h. */
    SCREEN_FX,
    /* Added 2026-09-16, at the end of the cycle - Spotify Connect
     * transport controls (play/pause/next/previous/volume), targeting
     * the Bose Smart Soundbar (see ../n8n/build_spotify_control_
     * workflow.py). Now-playing itself already shows on the Home
     * screen's notification strip (see spotify_mqtt.h) - this screen is
     * for control, not just display. */
    SCREEN_SPOTIFY,
    /* Added 2026-09-17, at the end of the cycle - a history of unseen
     * green/red/orange notifications (see the home_screen.c badge
     * comment block and notification_unseen_entry_t below), each
     * dismissible on its own. Explicit request: a missed banner (already
     * reverted back to music/calendar by the time you looked) should
     * still leave a trace instead of just being gone. */
    SCREEN_NOTIFICATIONS,
} screen_id_t;

/* Each *_screen_create() builds a standalone LVGL screen object (not yet
 * loaded) and returns it. Caller must hold the display lock while calling
 * these and while registering event callbacks on the returned object. */
lv_obj_t *home_screen_create(void);
lv_obj_t *systems_screen_create(void);
lv_obj_t *lights_screen_create(void);
lv_obj_t *flightaware_screen_create(void);
lv_obj_t *totp_screen_create(void);
lv_obj_t *claude_usage_screen_create(void);
lv_obj_t *spotify_screen_create(void);
lv_obj_t *notification_screen_create(void);
lv_obj_t *codex_usage_screen_create(void);
lv_obj_t *fx_screen_create(void);
lv_obj_t *weather_screen_create(void);
/* Weather icon key ("sun", "rain_light", ...) -> compiled image; falls back
 * to the sun. Shared by the Home and Weather screens (home_screen.c). */
const lv_image_dsc_t *wx_icon_lookup(const char *name);
typedef enum { FX_COP = 0, FX_EUR, FX_GBP } fx_currency_t;
/* Takes the display lock. date is "YYYY-MM-DD" (the rate's validity date). */
void fx_screen_set_rate(fx_currency_t which, double rate, bool has_prev, double prev, const char *date);

/* Called from the MQTT client's own task, so this takes the display lock
 * internally. session_pct/week_pct are 0-100 (already rounded).
 * session_reset_min/week_reset_min are minutes until each window resets -
 * the firmware formats these itself (e.g. "4h 53m" / "5d 1h") rather than
 * showing a raw wall-clock reset time, since minutes-from-now stays
 * correct regardless of clock sync timing, same reasoning as the Home
 * screen's own sunrise/sunset picking logic. */
void claude_usage_screen_set_usage(int session_pct, int session_reset_min, int week_pct, int week_reset_min);

/* Same idea as claude_usage_screen_set_usage() above, for the Codex
 * (OpenAI) usage screen - pct_5h/pct_weekly are 0-100, reset_min_5h/
 * reset_min_weekly are minutes until each window resets. Called from the
 * MQTT client's own task, takes the display lock internally. */
void codex_usage_screen_set_usage(int pct_5h, int reset_min_5h, int pct_weekly, int reset_min_weekly);

/* Draws a row of small dots at the bottom of `screen`, highlighting the dot
 * at `active_index`. Shared across all screens for a consistent page indicator. */
void screens_add_page_dots(lv_obj_t *screen, int active_index);

/* Aircraft info shown on the Home screen. Pass -1 for alt_ft/heading_deg
 * when unknown (not every aircraft broadcasts them). String fields may be
 * NULL. Display falls back in this order: watchlist label, then
 * "<airline> <flight_number>", then raw_flight, then "Aircraft". */
typedef struct {
    bool is_watchlist;
    const char *label;         /* watchlist label, e.g. "British Airways A380" */
    const char *airline;       /* e.g. "Avianca" - from ICAO callsign prefix lookup */
    const char *flight_number; /* e.g. "008" */
    const char *raw_flight;    /* raw callsign fallback, e.g. "AAL1610" */
    const char *type;          /* short ICAO type code, e.g. "B772" */
    const char *type_desc;     /* long description, e.g. "BOEING 777-200" */
    const char *origin_iata;      /* e.g. "MIA" - from route lookup, may be NULL */
    const char *destination_iata; /* e.g. "MDE" - from route lookup, may be NULL */
    /* Raw source string from n8n: "mia_fids" | "flightaware" | "adsbdb" |
     * NULL. home_screen.c maps this to a short on-screen tag - see
     * route_source_tag(). */
    const char *route_source;
    int alt_ft;
    int heading_deg;
    double distance_mi;
    int bearing_deg;
    int look_angle_deg; /* signed degrees off the window's centerline - 0 = look straight out */
    bool overhead;
    /* "747" | "a380" | "military" when this displayed plane is the one the
     * chime fired for (n8n's special_aircraft, matched by hex), else NULL.
     * Drives the special display - same rule as the beep, by request. */
    const char *special_kind;
} aircraft_info_t;

/* Updates the aircraft display on the Home screen. Called from the MQTT
 * client's own task, so this takes the display lock internally - do not
 * call while already holding it. */
void home_screen_set_aircraft(const aircraft_info_t *info);
void home_screen_clear_aircraft(void);

/* Weather shown on the Home screen whenever no aircraft is being displayed
 * (see ../n8n/rebuild_weather_poll_workflow.py). icon selects one of the
 * bundled S:/spiffs/wx_*.png assets: "sun", "moon", "partly", "cloudy",
 * "fog", "rain", "snow", "thunder".
 * Redesigned 2026-09-14, explicit request, to a denser layout (compact
 * wind/humidity/next-sun-event row, condition icon beside the temperature
 * instead of above it) - only ever shows whichever sun event (sunrise or
 * sunset) hasn't happened yet *today*, not both. That decision is made by
 * the firmware itself against its own live clock (see home_screen.c's
 * pick_next_sun_event()), not by n8n, since this data only refreshes
 * every 15 min and picking server-side could show the wrong event for up
 * to 15 min around the actual sunrise/sunset moment - sunrise/sunset_24h
 * ("HH:MM", 24h) exist purely for that live comparison; sunrise/sunset/
 * sunrise_tomorrow are the pretty 12h strings actually displayed. */
typedef struct {
    const char *icon;
    const char *description; /* e.g. "Partly Cloudy" */
    double temp_f; /* one decimal shown, e.g. 55.9 - was a rounded int */
    int feels_like_f;
    int humidity_pct;
    int wind_mph;
    int wind_dir_deg; /* -1 if unknown; 0 = north, 90 = east, ... */
    const char *sunrise;          /* e.g. "7:04am" - today's */
    const char *sunset;           /* e.g. "7:30pm" - today's */
    const char *sunrise_tomorrow; /* e.g. "7:05am" - for after today's sunset */
    const char *sunrise_24h;      /* e.g. "07:04" - for live comparison only */
    const char *sunset_24h;       /* e.g. "19:30" - for live comparison only */
    /* Home-screen weather line (2026-09-25): one short sentence, most
     * important first (NWS alert > rain here > rain nearby > rain later >
     * "No rain expected..."). NULL text = hide the line. color: red |
     * orange | blue | amber | green. icon: NULL (dot) | warning | hurricane. */
    const char *outlook_text;
    const char *outlook_color;
    const char *outlook_icon;
} weather_info_t;

/* Called from the MQTT client's own task, so this takes the display lock
 * internally - do not call while already holding it. Safe to call whether
 * or not an aircraft is currently showing - the weather content only
 * becomes visible once the screen goes idle. */
void home_screen_set_weather(const weather_info_t *info);

/* Home screen's bottom notification strip - a shared, priority-ordered
 * API any subsystem can call, whether from an MQTT handler (see
 * notification_mqtt.c, fed by n8n) or directly in-firmware (see
 * claude_usage_mqtt.c's threshold check). Each source keeps its own
 * independent slot; whichever active one has the best priority is shown
 * (see home_screen.c's severity_priority()). severity is "red",
 * "orange", "green", "f1", "spotify", or "blue" (case-sensitive,
 * highest to lowest priority in that order; any other value is a no-op).
 * ttl_min > 0 auto-clears the notification after that many minutes with
 * no further update (for one-off events like a package delivery);
 * ttl_min <= 0 means the caller is responsible for eventually calling
 * home_screen_clear_notification() itself once whatever it's reporting
 * is resolved (for level-triggered conditions like a system being
 * offline or usage over a threshold).
 * source is a short caller-chosen tag (e.g. "claude_usage", "systems",
 * "package") - home_screen_clear_notification() only actually clears if
 * it names the source that's currently showing, so one source's clear
 * call can never stomp on a different, still-active notification from
 * another source. Both take the display lock internally. */
void home_screen_set_notification(const char *source, const char *message, const char *severity, int ttl_min);
/* Same, plus an optional small icon group at the left of the strip and an
 * optional sound - added 2026-09-24 for the F1 session alerts. icon is
 * "practice" (car), "quali" (car + stopwatch), "race" (car + checkered
 * flag), or NULL/anything else for none. play_sound plays the F1 "start
 * lights" sound (aircraft_alert.c) on a fresh occurrence only - n8n
 * decides quiet hours and sends false then. */
/* F1 "session live" line under the date row (2026-09-24). code is the
 * short label ("P1", "Q", "Race", ...), gp the short GP name ("Azerbaijan
 * GP"); empty/NULL code clears. ends_at is a
 * Unix time - the device hides the badge itself once its clock passes it.
 * Takes the display lock. */
void home_screen_set_f1_session(const char *code, const char *gp, long long ends_at);
void home_screen_set_notification_ex(const char *source, const char *message, const char *severity, int ttl_min,
                                     const char *icon, bool play_sound);
void home_screen_clear_notification(const char *source);

/* Notification-history support for the new Notification screen
 * (notification_screen.c), added 2026-09-17 - separate from the banner
 * API above. A source's notification becomes "unseen" the moment it
 * fires (red/orange/green only - calendar/spotify are steady-state
 * status, not events - see home_screen_set_notification()'s
 * fresh_occurrence gate), and stays that way - independent of the
 * banner's own 60s display window or the underlying condition later
 * resolving - until explicitly dismissed here.
 *
 * NOTIFICATION_HISTORY_MAX is 5, not home_screen.c's full
 * NOTIFICATION_MAX_SOURCES=10 - real, hard-learned lesson 2026-09-17:
 * the first attempt at this screen pre-allocated a full 10 rows' worth
 * of static lv_obj_t* tracking arrays (~370 bytes) and broke
 * esp_mqtt_client_start() outright (this device's whole safety margin
 * is only ~400 bytes of internal SRAM - see homeops_mqtt.c's own
 * stack-size history for the same lesson learned a different way).
 * Static globals cost real internal SRAM regardless of what LVGL does
 * with its own widget objects (those DO live in the spare 16MB PSRAM -
 * confirmed by this project's own history - but a plain C array you
 * declare yourself does not get that treatment automatically). Only
 * red/orange/green sources ever count as unseen - nas, pilab, sky,
 * abbe, claude_usage is the real, current list, so 5 is already
 * headroom, not a tight fit. */
#define NOTIFICATION_HISTORY_MAX 5

typedef struct {
    char source[24];   /* matches notification_slot_t.source's size */
    char message[200]; /* matches home_screen.c's own NOTIFICATION_MESSAGE_MAX_LEN */
    lv_color_t color;  /* matches severity_color() - red/orange/green */
} notification_unseen_entry_t;

/* Fills out[] (up to max entries) with every currently-unseen
 * notification. Returns how many were written. Takes the display lock
 * internally. */
int home_screen_get_unseen_notifications(notification_unseen_entry_t *out, int max);

/* Marks one source's notification as seen (or every source's, for a
 * "clear all"). Only affects the history/badge tracking above - does
 * NOT touch the source's live banner state, so if the underlying
 * condition is still real it can still show/re-fire on the banner same
 * as always. Takes the display lock internally. */
void home_screen_dismiss_notification(const char *source);
void home_screen_dismiss_all_notifications(void);

/* One row on the Systems screen (see ../n8n/rebuild_health_check_workflow.py).
 * ip may be NULL (shown as "--"). temp_c is the device's own CPU/board
 * sensor reading in Celsius (vcgencmd on the Pis, TrueNAS's reporting API
 * on the NAS) - has_temp false when a box doesn't have one wired up yet
 * (shows just the IP, same as before this field existed). */
typedef struct {
    bool online;
    const char *ip;
    bool has_temp;
    double temp_c;
} system_status_t;

/* key must be one of "nas", "pilab", "abbe", "sky" - matches the row names
 * systems_screen_create() builds internally. Called from the MQTT client's
 * own task, so this takes the display lock internally. */
void systems_screen_set_status(const char *key, const system_status_t *status);

/* FlightAware AeroAPI usage, its own screen (see flightaware_screen.c) -
 * real numbers from FlightAware's own free GET /account/usage endpoint
 * (see n8n/rebuild_mia_fids_poll_workflow.py), not this firmware's own
 * estimate. Was a Systems-screen row originally; moved to its own screen
 * 2026-09-13, explicit request - didn't fit that row's value column.
 * Called from aircraft_mqtt.c's own task, so this takes the display lock
 * internally. */
void flightaware_screen_set_usage(int calls, double cost);

/* Reflects the office lamp's actual Hue state on the Lights screen's
 * button (see ../n8n/rebuild_lights_control_workflow.py). Called from the
 * MQTT client's own task, so this takes the display lock internally. */
void lights_screen_set_state(bool on);

/* Same as lights_screen_set_state(), but positions the brightness slider -
 * `bri` is Hue's native 1-254 range. A no-op while the user is actively
 * dragging the slider themselves, so a mid-drag MQTT state update (the
 * 30s poll, or the post-set re-poll from a previous request) can't fight
 * the in-progress gesture. */
void lights_screen_set_brightness(int bri);

/* Brief on-screen feedback for the voice pipeline (see lights_voice.c) -
 * e.g. "Listening..." right after the wake word, then either the
 * recognized command or nothing once it times out. NULL/empty clears it.
 * Called from the voice recognition task, takes the display lock
 * internally. */
void lights_screen_set_voice_status(const char *status);

/* Reflects "Home Ops - Spotify Now Playing"'s state (see spotify_mqtt.h)
 * on the Spotify screen's play/pause button, track/artist label, device
 * label, and volume slider/label. track/artist/device may be empty
 * strings (nothing playing, or those specific fields weren't present)
 * but never NULL. volume_percent is -1 when unknown (no active device to
 * read it from), 0-100 otherwise - same "-1 means unknown, don't just
 * treat it as a real 0%" convention as system_status_t's has_temp field
 * elsewhere in this file, just as a sentinel value instead of a separate
 * bool (one less field to keep in sync). Called from the MQTT client's
 * own task, so this takes the display lock internally. */
void spotify_screen_set_state(bool is_playing, const char *track, const char *artist, const char *device,
                               int volume_percent);
