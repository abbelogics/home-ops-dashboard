#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "bsp/esp-box-3.h"
#include "esp_log.h"
#include "aircraft_alert.h"
#include "notification_sound.h"
#include "screens.h"
#include "sensor_accessory.h"
#include "wifi_time.h"

#include "airline_logos/airline_logos.h"
#include "ui_icons/ui_icons.h"

/* Custom-generated (fonts/lv_font_montserrat_16_latin_ext.c), same
 * accented-Latin range as the 14/18 companions. Used by
 * s_notification_label (the generic single-line path) and as the artist
 * weight in the Spotify title/artist marquee below. Bumped from 14 to 18
 * - explicit "the text on the banner ... is too small" request
 * 2026-09-16 - then dialed back to a newly generated 16 ("is there any
 * size letter between the one we have now and the one we had before"),
 * once 18 turned out larger than wanted. */
LV_FONT_DECLARE(lv_font_montserrat_16_latin_ext);
/* Bold companion (fonts/lv_font_montserrat_16_latin_ext_bold.c) - only
 * the Spotify now-playing title uses this; see s_notification_title_label
 * below. */
LV_FONT_DECLARE(lv_font_montserrat_16_latin_ext_bold);

static const char *TAG = "home_screen";

static lv_obj_t *s_clock_label;
static lv_obj_t *s_date_label;
static lv_obj_t *s_conditions_label;
/* Notification-history envelope + unseen-count badge in the header row -
 * see home_screen_dismiss_notification()/screens.h. s_unseen_badge_group
 * is the parent that gets shown/hidden as a unit (envelope + badge
 * together); hidden whenever an aircraft sighting is active (explicit
 * "only on this page, not on the plane spotter" request) or whenever the
 * unseen count is 0. */
static lv_obj_t *s_unseen_badge_group;
static lv_obj_t *s_unseen_badge_count_label;
/* F1 "session live" entry (2026-09-24): car + session code (P1/P2/P3/SQ/
 * Sprint/Q/Race) + pulsing "live" dot (green; orange for the last 10 min)
 * + "|" + GP name. Final placement (after trying beside the envelope and
 * under the date row): the bottom notification strip, as its own
 * calendar-style slot ("f1_live" source/severity, no background - the
 * cyan was dropped for it by request) - music
 * outranks it, it outranks the calendar, and the strip falls back to the
 * calendar/empty when the session ends. s_f1_session_group is a row inside
 * the strip, shown by recompute_notification_display() in place of the
 * generic label when the f1_live slot wins. Driven by the retained
 * homeops/f1/session topic (n8n), but also removed by this device's own
 * clock at ends_at, so it goes away on time even if PiLab is down. Hidden
 * during an aircraft sighting, same as the envelope. */
static lv_obj_t *s_f1_session_group;
static lv_obj_t *s_f1_session_label;
static lv_obj_t *s_f1_session_gp_label;
static lv_obj_t *s_f1_live_dot;
static bool s_f1_dot_pulsing;
static bool s_f1_dot_orange;
/* Set by home_screen_set_f1_session() so update_f1_session_badge() only
 * touches the strip on a real change - recomputing every 3s tick would
 * keep resetting the calendar label's scroll. */
static bool s_f1_session_dirty;
static char s_f1_session_code[12];
static char s_f1_session_gp[40];
static time_t s_f1_session_ends_at;
static lv_obj_t *s_airline_logo;
static lv_obj_t *s_aircraft_main;
#define SPECIAL_GOLD 0xFFD700
static lv_obj_t *s_aircraft_type;
static lv_obj_t *s_aircraft_detail;
static lv_obj_t *s_look_direction;
/* Bottom notification strip, same slot the scrolling news ticker and
 * then the abbe/pilab/sky status dots used to occupy (both removed
 * 2026-09-15 - see home_ops history in PROJECT.md). A generic,
 * color-coded notification bar - one line of text, background color set
 * by severity - fed by any subsystem, whether a direct in-firmware check
 * (see claude_usage_mqtt.c's threshold check) or an external MQTT
 * message from an n8n workflow (see notification_mqtt.c). Hidden
 * whenever nothing is active - explicit "otherwise the bar is not
 * shown" request.
 * 2026-09-15: replaced the original single "last call wins" slot with a
 * real priority system, explicit request once a 4th source (calendar,
 * blue) was added alongside systems/claude-usage (red/orange) and NAS
 * (green) - "calendar takes priority" over the routine green
 * confirmation, but a real red/orange alert should still interrupt
 * everything. Priority is derived directly from severity itself
 * (severity_priority() below: red > orange > spotify > blue > green -
 * "spotify" added 2026-09-16 so now-playing beats the calendar's routine
 * line without needing a real alert) rather than hardcoding "calendar" as
 * a special source - any future severity gets the same behavior
 * automatically. Each named source now
 * keeps its own independent slot (NOTIFICATION_MAX_SOURCES of them, one
 * per distinct source string seen so far) instead of a single shared
 * one, so a source's own clear can never stomp a different, still-active
 * source - the old bug this was fixed for (see git history) is now
 * structurally impossible rather than guarded against with a source-name
 * check. On every set/clear/tick, the display shows whichever active
 * slot has the best (lowest-numbered) priority. */
/* 8 distinct sources currently in play: calendar, nas, spotify, pilab,
 * sky, abbe, claude_session, claude_week (see claude_usage_mqtt.c and
 * n8n/rebuild_health_check_workflow.py's pilab/sky/abbe split) - bumped
 * from 6 with headroom to spare, isolated from any other change and
 * soak-tested alone, after a build combining this bump with several
 * other changes at once hung on boot 2026-09-16 and the actual cause was
 * never isolated (see git history/session notes). */
#define NOTIFICATION_MAX_SOURCES 10
/* No notification (any severity) squats on the strip for more than this
 * long without an explicit clear or a genuinely new occurrence - explicit
 * "the time they remain on the main screen should not be more than 60
 * seconds" request 2026-09-16, tightened down from the earlier separate
 * 3-minute green / 5-minute alert caps once those still felt too long in
 * practice. Applied as a hard cap regardless of the caller's own ttl_min,
 * not just a default. Spotify is exempt - it's a continuous live "what's
 * playing now" status, not a one-off alert, so it has no display cap at
 * all (see the ttl_sec assignment below). This is a DISPLAY cap only, not
 * a "problem resolved" signal - a still-ongoing issue is expected to
 * re-assert itself periodically (the caller's own responsibility - see
 * rebuild_health_check_workflow.py's periodic re-publish). But only a
 * genuinely NEW occurrence (see last_notified_message below) reopens the
 * display window - an unchanged republish must not, or an ongoing-but-
 * unchanged condition (e.g. Claude usage sitting at 80% across several
 * ~2-min polls) would reappear on screen every 60 seconds forever. */
#define NOTIFICATION_DISPLAY_MAX_TTL_SEC (60)
/* A source with more than one thing to say (e.g. several events on
 * today's calendar) packs them into one message, all events on one line
 * separated by a bullet, instead of collapsing everything past the first
 * into a static "(+N more)" suffix - explicit "I think this should run
 * as a ticker" request, once a 2-event day made that suffix the common
 * case rather than an edge case. The strip itself doesn't do anything
 * special with this - s_notification_label runs in LVGL's own
 * LV_LABEL_LONG_MODE_SCROLL_CIRCULAR (see its creation below), which
 * scrolls automatically, continuously, and only when the text is
 * actually wider than the label - a short single-event message just
 * sits there static exactly as before. An earlier version drove a
 * hand-rolled swap-between-segments cycle instead (a fixed-size buffer
 * plus a periodic timer tick advancing an index) - reverted the same
 * day after live testing looked like separate messages silently
 * replacing each other rather than an actual moving ticker; LVGL's
 * built-in mode does the real thing for less code. Buffer sized for a
 * handful of short lines, not arbitrary text - this is a plain static
 * global (internal SRAM, the scarce resource on this device - see
 * PROJECT.md's MQTT-consolidation history), so it stays deliberately
 * modest rather than generous. */
#define NOTIFICATION_MESSAGE_MAX_LEN 200

typedef struct {
    bool active;
    char source[24];
    char message[NOTIFICATION_MESSAGE_MAX_LEN];
    lv_color_t color;
    /* see severity_text_color() - white by default, but Spotify's bright
     * brand green is too light for white text to read well against */
    lv_color_t text_color;
    /* Spotify's message is "title | artist", not a flat one-off line -
     * rendered through the separate bold-title/regular-artist marquee
     * (see update_spotify_marquee()) instead of s_notification_label's
     * generic single-line path. Set from severity at
     * home_screen_set_notification() time so recompute_notification_
     * display() doesn't need to re-parse/compare strings on every tick. */
    bool is_spotify;
    int priority; /* lower = higher priority; see severity_priority() */
    bool is_f1_live; /* F1 live-session row instead of the label - see s_f1_session_group */
    /* F1 session alerts (2026-09-24): which icon group to draw at the left
     * of the strip - see NOTIF_ICON_* and update_notification_icons(). */
    uint8_t icon;
    int ttl_sec;  /* 0 = no auto-expiry (explicit clear only) */
    int age_sec;
    /* Separate from `message` above: this is the text that was actually
     * last SHOWN (beeped + display window opened for), not just the last
     * text received. A republish with unchanged content only updates
     * `message`/`active`/`age_sec` in the ordinary way - it does NOT touch
     * these, so it can't reopen the display window or re-beep. Only a
     * genuine change (or the source clearing and later coming back) does.
     * See home_screen_set_notification()'s fresh_occurrence logic. */
    char last_notified_message[NOTIFICATION_MESSAGE_MAX_LEN];
    bool ever_notified;
    /* Independent of active/ttl_sec above - this is the Notification
     * page's own "haven't looked at this yet" flag, added 2026-09-17.
     * Set on every fresh green/red/orange occurrence (see
     * home_screen_set_notification()), cleared only by an explicit
     * dismiss from the Notification screen (home_screen_dismiss_
     * notification()/_all()) - NOT by the underlying condition
     * resolving (home_screen_clear_notification()) or by the banner's
     * own 60s display window expiring, so a real event that happened
     * while away from the device still leaves a trace even after the
     * banner itself has already reverted back to music/calendar. */
    bool unseen;
} notification_slot_t;

static lv_obj_t *s_notification_strip;
static lv_obj_t *s_notification_label;
/* F1 icon group (2026-09-24) - a small flex row pinned to the strip's left
 * edge: always the car, plus a stopwatch (qualifying) or checkered flag
 * (race). Opaque, same color as the strip, and drawn after the label so a
 * scrolling message passes underneath it instead of through it; the
 * label's own left padding keeps static (non-scrolling) text clear of it.
 * Icons are compiled-in (tools/icon_src/f1_*.png via convert_ui_icons.py),
 * no runtime decode. */
enum { NOTIF_ICON_NONE = 0, NOTIF_ICON_PRACTICE, NOTIF_ICON_QUALI, NOTIF_ICON_RACE, NOTIF_ICON_NWS_WARNING,
       NOTIF_ICON_NWS_HURRICANE };
static lv_obj_t *s_notification_icons;
static lv_obj_t *s_notification_icon_extra;
/* First icon in the group: the F1 car, or the NWS alert icon (2026-09-24). */
static lv_obj_t *s_notification_icon_main;
/* Spotify-only marquee (title bold, artist regular, "|" between) - see
 * update_spotify_marquee(). s_notification_label above handles every
 * other source unchanged; these three only get shown/positioned when the
 * active notification is Spotify's. s_notification_clip is a fixed-width,
 * non-scrollable viewport (LVGL clips a child's overflow to its parent's
 * bounds by default) that s_notification_row - sized to its own natural
 * content width, which can exceed the viewport - is manually x-animated
 * within, the same start/end/gap idea LV_LABEL_LONG_MODE_SCROLL_CIRCULAR
 * itself uses (see lv_label.c), just driving a whole row of widgets
 * instead of one label's internal text offset (LVGL has no built-in way
 * to mix a bold and a regular-weight label in a single scrolling label). */
static lv_obj_t *s_notification_clip;
static lv_obj_t *s_notification_row;
static lv_obj_t *s_notification_title_label;
static lv_obj_t *s_notification_sep_label;
static lv_obj_t *s_notification_artist_label;
static notification_slot_t s_notification_slots[NOTIFICATION_MAX_SOURCES];
/* Forward declaration - home_timer_cb() (defined ahead of the full
 * notification implementation below) needs to call this on every
 * TTL-expiry tick. */
static void recompute_notification_display(void);
static notification_slot_t *find_or_alloc_slot(const char *source);
static int severity_priority(const char *severity);
static lv_color_t severity_color(const char *severity);
/* Forward declaration - home_screen_set_aircraft()/home_screen_clear_
 * aircraft() (also defined ahead of the full notification
 * implementation) need to call this whenever s_aircraft_active changes,
 * since the unseen badge is hidden while a sighting is active. */
static void update_unseen_badge(void);
/* Weather widgets redesigned 2026-09-14, explicit request, into a denser
 * layout: a compact icon-led row (wind/humidity/next-sun-event) above the
 * condition icon + temperature (now side by side, not stacked), freeing
 * up vertical space below for future additions. s_weather_wind/
 * s_weather_sun (old full-sentence text rows) are gone entirely, replaced
 * by the compact row's own widgets. */
static lv_obj_t *s_weather_icon;
static lv_obj_t *s_weather_main;
static lv_obj_t *s_weather_detail;
/* Compact row: wind, humidity, next sun event, all together - briefly
 * split into two rows (wind+humidity, sunrise/sunset) 2026-09-15, undone
 * the same evening ("go back to" an earlier reference screenshot). Kept
 * the s_wind_humidity_row name even though sun-event widgets live in it
 * too now, rather than a broader rename. */
static lv_obj_t *s_wind_humidity_row;
static lv_obj_t *s_wind_icon;
static lv_obj_t *s_wind_label;
static lv_obj_t *s_wind_dir_label; /* e.g. "SE" - smaller font than the speed number, explicit request 2026-09-14 */
static lv_obj_t *s_humidity_icon;
static lv_obj_t *s_humidity_label;
static lv_obj_t *s_sun_icon;
static lv_obj_t *s_sun_time_label;
static lv_obj_t *s_sun_arrow_icon; /* real arrow shape, not LV_SYMBOL_UP/DOWN's chevron - see update_sun_event_label() */

/* Weather line (2026-09-25, explicit request - fills the gap between the
 * date row and the temperature row): first row of the info column, a dot
 * (or the NWS alert icon) + one short sentence from n8n's "outlook". Part
 * of the weather group: hidden during aircraft sightings, hidden when the
 * payload has no outlook. */
static lv_obj_t *s_outlook_row;
static lv_obj_t *s_outlook_dot;
static lv_obj_t *s_outlook_icon;
static lv_obj_t *s_outlook_label;
static bool s_have_outlook = false;

/* Cached across weather updates so the "which sun event is next" decision
 * (see update_sun_event_label()) can be re-evaluated every 3s against the
 * live clock, not just whenever a new 15-min weather poll happens to
 * land - the pointers on a weather_info_t are only valid for the
 * duration of the MQTT callback that delivered them (cJSON owns that
 * memory), so the actual strings need copying out, not just referencing. */
static char s_sunrise_pretty[16] = "";
static char s_sunset_pretty[16] = "";
static char s_sunrise_tomorrow_pretty[16] = "";
static char s_sunrise_24h[8] = "";
static char s_sunset_24h[8] = "";
static bool s_have_sun_data = false;

/* Weather only ever shows up while idle - track both so a weather message
 * that arrives while a plane is on screen updates the widgets but stays
 * hidden until the screen actually goes idle. */
static bool s_aircraft_active = false;
static bool s_have_weather = false;
static bool s_display_asleep = false;

/* Motion-based sleep: the AT581X radar's digital output was assumed (from
 * esp-box's factory_demo reference) to re-trigger/pulse periodically during
 * continued presence, so edge-detection seemed like the right way to tell
 * "still here" from "just arrived". Live testing showed that's wrong for
 * this sensor/config - the output just holds continuously high the whole
 * time someone's present, so edge-only resets let the idle timer keep
 * counting down (and eventually sleep) even with someone standing right in
 * front of it, and once asleep with the level stuck high there was never
 * another rising edge to wake it. Using the raw level directly fixes both:
 * high always means "reset the timer, wake if asleep", low means "count
 * toward sleep". */
#define IDLE_SLEEP_TIMEOUT_SEC   120
/* Night mode (23:00-07:00, wifi_time_is_night()): back to 1% after 10s of
 * no motion instead of 2 min (30 -> 10 by request after the live test),
 * and forced to sleep at 23:00 itself. Counted in 3s ticks, and only once
 * the 12s motion window stops confirming, so ~12-20s after you leave. */
#define NIGHT_IDLE_SLEEP_TIMEOUT_SEC 10
static int s_idle_sec = 0;
static bool s_was_night = false;

static int active_brightness_pct(bool night)
{
    return night ? DISPLAY_NIGHT_BRIGHTNESS_PCT : DISPLAY_BRIGHTNESS_PCT;
}

static int idle_brightness_pct(bool night)
{
    return night ? DISPLAY_NIGHT_IDLE_BRIGHTNESS_PCT : DISPLAY_IDLE_BRIGHTNESS_PCT;
}

/* Hardware sensitivity (gain_cfg/delta_cfg in sensor_accessory.c) was
 * tuned down substantially after live testing showed the radar reading
 * "motion" continuously even with the user ~12m away in another room -
 * that got it from permanently-stuck-high down to occasional isolated
 * blips. Requiring several recent high readings (a real, sustained
 * presence signal) before counting as "motion" for either waking or
 * resetting the sleep timer filters an isolated blip out as noise, at
 * the cost of a small delay recognizing genuine motion - a fine trade
 * for a display that's supposed to be judged on "does it correctly go
 * to sleep", not first-detection latency.
 * 2026-09-13: raised from 2-in-a-row to 3-in-a-row after a real "away
 * the whole time" baseline test (at GAIN_B, one gain step more sensitive
 * than the confirmed-too-insensitive max) still never reached
 * IDLE_SLEEP_TIMEOUT_SEC - the false-trigger gap had roughly doubled
 * (39s ceiling at GAIN_A -> 78-84s at GAIN_B) but never got past ~85s,
 * and gain alone was already bracketed between confirmed-too-sensitive
 * and confirmed-too-insensitive with nothing further to try there.
 * 2026-09-15: switched from strict-consecutive to a 2-of-last-4 sliding
 * window (still a 12s window, same as the old 3x3s-in-a-row) - explicit
 * "standing in front of it and it's not waking up" report, root-caused
 * with a live capture (MOTION_HISTORY_LEN below) showing the raw signal
 * genuinely toggles almost every 3s sample even during real, continuous
 * presence (`1,1,0,1,1,1,1,1,0,1,...`), not the sustained-high signal
 * the strict-consecutive check assumed - any single blip reset the whole
 * run to zero, so confirming took up to 15s and dropped again after just
 * one missed sample. A sliding window tolerates that same blippiness
 * while still rejecting real noise: the documented false-positive blips
 * above were isolated (20-84s apart), so two of them landing inside any
 * one 12s window is unlikely - confirmed via the same live capture this
 * fix was based on, not assumed. */
#define MOTION_HISTORY_LEN 4
#define MOTION_HISTORY_MIN_HIGH 2
static bool s_motion_history[MOTION_HISTORY_LEN];
static int s_motion_history_idx = 0;

/* 8-point compass direction, e.g. "Northeast", from a real-world bearing in
 * degrees (0 = north, 90 = east, ...). Plain text turned out much simpler
 * and more reliable than trying to draw a rotating arrow shape. */
static const char *compass_direction(int bearing_deg)
{
    static const char *DIRECTIONS[] = {
        "North", "Northeast", "East", "Southeast",
        "South", "Southwest", "West", "Northwest",
    };
    int normalized = ((bearing_deg % 360) + 360) % 360;
    int index = ((normalized + 22) / 45) % 8;
    return DIRECTIONS[index];
}

/* Short 2-letter form for the compact wind row (e.g. "SW") - separate
 * from compass_direction() above, which stays full-word for the aircraft
 * look-direction display. Added 2026-09-14 for the weather redesign. */
static const char *compass_abbrev(int bearing_deg)
{
    static const char *DIRECTIONS[] = {
        "N", "NE", "E", "SE", "S", "SW", "W", "NW",
    };
    int normalized = ((bearing_deg % 360) + 360) % 360;
    int index = ((normalized + 22) / 45) % 8;
    return DIRECTIONS[index];
}

/* Redraws the compact row's sun-event time to whichever of
 * sunrise/sunset/tomorrow's sunrise hasn't happened yet, per the cached
 * "HH:MM" strings from the last weather update - called both right after
 * a weather update and every 3s from home_timer_cb, since the correct
 * choice depends on the current time, not on when weather last polled.
 * wifi_time_get_hhmm() and n8n's sunrise/sunset_24h are both zero-padded
 * 24h "HH:MM", so a plain string compare orders them correctly without
 * needing real date/time parsing here. */
static void update_sun_event_label(void)
{
    if (!s_have_sun_data) {
        lv_label_set_text(s_sun_time_label, "");
        return;
    }

    char hhmm[6];
    if (!wifi_time_get_hhmm(hhmm, sizeof(hhmm))) {
        return; /* no time synced yet - leave whatever was last shown */
    }

    /* Icon is always the sun regardless of sunrise/sunset - explicit
     * request 2026-09-15, superseding the earlier sun/moon split (was
     * 2026-09-14's request). Real arrow icon (arrow_up.png/arrow_down.png)
     * after the time text, not LV_SYMBOL_UP/DOWN - those render as a plain
     * chevron in the bundled icon font, not an actual arrow shape.
     * sun_icon.png is drawn natively at the 24x24 size the wind/humidity
     * icons already use (fixing the earlier "yellow box" bug - see
     * try_logo_code()'s comment for the full center-crop story), sized so
     * the center-crop shows the whole glyph. */
    const char *label_time;
    const lv_image_dsc_t *arrow_src;
    if (strcmp(hhmm, s_sunrise_24h) < 0) {
        label_time = s_sunrise_pretty;
        arrow_src = &arrow_up;
    } else if (strcmp(hhmm, s_sunset_24h) < 0) {
        label_time = s_sunset_pretty;
        arrow_src = &arrow_down;
    } else {
        label_time = s_sunrise_tomorrow_pretty;
        arrow_src = &arrow_up;
    }
    lv_label_set_text(s_sun_time_label, label_time);
    /* Compiled-in image, not a runtime SPIFFS decode - see
     * tools/convert_ui_icons.py. This call used to re-decode sun_icon.png
     * from flash every 3 seconds, forever (this function runs on every
     * home_timer_cb tick) - by far the most frequent hit against the
     * decode-needs-a-contiguous-heap-allocation bug of anything in this
     * file, airline logos included. Live-caught 2026-09-17: the sun/
     * wind/humidity/condition icons all went blank at once while their
     * text values kept updating fine. */
    lv_image_set_src(s_sun_icon, &sun_icon);
    lv_image_set_src(s_sun_arrow_icon, arrow_src);
}

/* Short on-screen tag for where a route came from - explicit request
 * 2026-09-13, so it's visible at a glance which source resolved each
 * sighting, not just logged in Postgres. Naming is the user's own pick:
 * "FIDS" for MIA's public feed, "FA" for FlightAware, "DB" for adsbdb.com
 * (a flight database, hence "DB" rather than confusing it with FlightAware
 * itself). Returns NULL (caller omits the tag entirely) for an unknown or
 * absent source - deliberately not "N/A"/"--" clutter for what's already a
 * secondary detail. */
static const char *route_source_tag(const char *route_source)
{
    if (!route_source) {
        return NULL;
    }
    if (strcmp(route_source, "mia_fids") == 0) {
        return "FIDS";
    }
    if (strcmp(route_source, "flightaware") == 0) {
        return "FA";
    }
    if (strcmp(route_source, "adsbdb") == 0) {
        return "DB";
    }
    return NULL;
}

/* Callsign-prefix codes whose bundled logo is dark navy/black ink that
 * reads as near-invisible directly on this screen's near-black background
 * - identified 2026-09-13 by measuring opacity-weighted average luminance
 * per bundled PNG and visually confirming the borderline ones. Not
 * exhaustive by construction - a newly bundled logo that turns out dark
 * just needs its code added here, same as any of these.
 * `jbu` added 2026-09-14 after a real on-hardware report - the original
 * analysis (a digital crop against pure black) judged it fine, but the
 * actual 45%-dimmed physical LCD washed it out enough to be genuinely
 * hard to see. Real hardware reports beat a digital preview - don't
 * re-litigate this one without a new on-device report either way.
 * `afr` (Air France) added 2026-09-16, explicit live request. `aal`
 * (American Airlines) was briefly added the same day for a wordmark
 * replacement image that turned out to have its own decode problems
 * (see the compiled-image rewrite in airline_logos/) - removed again
 * 2026-09-17 once reverted back to the original tail-icon crop, which
 * has real color/contrast of its own and looked worse with a white chip
 * forced behind it (explicit live report - the chip's own semi-
 * transparent edge pixels were blending into a washed-out pale square).
 * `rou` (Air Canada Rouge), `voi` (Volaris), `gxa` (GlobalX) added
 * 2026-09-22, explicit live request. El Al, BA, ITA, Qatar, LATAM (all four
 * codes), Royal Air Maroc, Eastern Air Express added 2026-09-24, same;
 * FedEx (`fdx`) and `vnt` (private operator, user-supplied logo, first
 * seen as VNT452 C56X) the same day. */
static bool logo_needs_white_bg(const char *code)
{
    static const char *const DARK_LOGO_CODES[] = {
        "amx", "gec", "fin", "dlh", "eny", "cks", "abx", "rpa",
        "azg", "lot", "gti", "cjt", "cmp", "atn", "jbu", "eja", "afr",
        "rou", "voi", "gxa",
        "ely", "baw", "ity", "qtr", "lan", "tam", "lpe", "lne", "ram", "bbq",
        "fdx", "vnt",
    };
    for (size_t i = 0; i < sizeof(DARK_LOGO_CODES) / sizeof(DARK_LOGO_CODES[0]); i++) {
        if (strcmp(code, DARK_LOGO_CODES[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* Looks up code in the compiled-in airline_logo_lookup() table (see
 * tools/convert_logos.py / main/airline_logos/) and points the logo
 * widget at the already-decoded image descriptor. Returns false (leaving
 * the widget untouched) if there's no bundled logo for this code, so
 * callers can try a fallback code.
 *
 * Redone 2026-09-17, replacing a runtime SPIFFS-PNG-decode version:
 * three separate live incidents (American Airlines, Delta, UPS - three
 * different images, so not a per-file problem) all showed the exact same
 * signature - this function's own diagnostic logging reported a clean
 * load (file found, correct size, not hidden) while the actual logo was
 * invisible on the real screen. Root cause was the runtime PNG decode
 * itself: it needs a real contiguous heap allocation on a device that
 * runs at ~8.5KB free internal heap, and a transient allocation failure
 * at the wrong moment silently produces blank/garbage pixels with no
 * error anywhere in the pipeline (the header, which is cheap to read,
 * still parses fine, so size/hidden/bg_opa all look correct even when
 * the pixel data didn't decode). Pre-decoding every logo at build time
 * into a compiled lv_image_dsc_t removes the decode step - and its heap
 * dependency - entirely; there is nothing left here that can fail based
 * on how busy the device happens to be at this exact moment. */
static bool try_logo_code(const char *code)
{
    if (!code || code[0] == '\0') {
        return false;
    }

    const lv_image_dsc_t *dsc = airline_logo_lookup(code);
    if (!dsc) {
        return false;
    }

    lv_image_set_src(s_airline_logo, dsc);
    lv_obj_clear_flag(s_airline_logo, LV_OBJ_FLAG_HIDDEN);

    /* Real bug found 2026-09-14 via live diagnostic logging: this widget
     * uses LV_SIZE_CONTENT, and lv_image_set_src() (above) does call
     * lv_obj_refresh_self_size() internally - but that only marks the
     * layout dirty (lv_obj_mark_layout_as_dirty()), it doesn't recompute
     * synchronously. The actual resize only happens on LVGL's own next
     * layout pass, which runs in the lvgl_port task - a different task
     * than this one (called from the MQTT client's task, while already
     * holding the display lock). Forcing the layout pass here,
     * synchronously, closes that gap before the caller ever sees a
     * stale/tiny size. Unrelated to the decode-timing bug above (this one
     * is about layout, not pixels) and still applies with a compiled-in
     * image source exactly as it did with a SPIFFS path. */
    lv_obj_update_layout(s_airline_logo);

    /* Per-logo white chip, not a blanket one (see logo_needs_white_bg) -
     * must be set on every successful load, not just once, since this
     * widget is reused across sightings and a previous dark logo's white
     * chip would otherwise stay applied behind the next (already-visible)
     * one. */
    if (logo_needs_white_bg(code)) {
        lv_obj_set_style_bg_color(s_airline_logo, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(s_airline_logo, LV_OPA_COVER, 0);
    } else {
        lv_obj_set_style_bg_opa(s_airline_logo, LV_OPA_TRANSP, 0);
    }

    ESP_LOGI(TAG, "logo loaded code=\"%s\" size=%dx%d hidden=%d bg_opa=%d", code,
             (int)lv_obj_get_width(s_airline_logo), (int)lv_obj_get_height(s_airline_logo),
             lv_obj_has_flag(s_airline_logo, LV_OBJ_FLAG_HIDDEN),
             (int)lv_obj_get_style_bg_opa(s_airline_logo, 0));

    return true;
}

/* Airline logos matched by callsign prefix (see ../tools/fetch_logos.py).
 * Airlines only, deliberately - manufacturer logos (mfr_<name>.png, e.g.
 * for a GA Cessna/Bombardier/Gulfstream with no airline callsign to match)
 * were removed 2026-09-11, explicit request. Shows text only, no image,
 * for any aircraft with no bundled airline logo.
 *
 * Real bug caught live 2026-09-17: a Delta sighting's logo appeared and
 * disappeared repeatedly across several consecutive polls of the SAME
 * still-overhead aircraft - this function used to re-decode the PNG from
 * SPIFFS on every single aircraft-state MQTT message regardless of
 * whether the airline actually changed (a flight overhead for 60-90s at
 * a ~3s poll interval means 20-30 redundant re-decodes of an unchanged
 * image). Each decode is a real allocation on a device already
 * documented to run at ~8.5KB free internal heap (see the lights/TOTP
 * heap-exhaustion writeup) - a transient allocation failure on any given
 * re-decode reads as exactly this "shows, then vanishes, then shows
 * again" flicker. Fix: skip the decode entirely when the resolved code
 * hasn't changed since the last call - the already-rendered widget just
 * stays as-is. */
static char s_last_logo_code[4] = "";

static void update_aircraft_logo(const char *raw_flight)
{
    char airline_code[4] = {0};
    if (raw_flight && strlen(raw_flight) >= 3) {
        for (int i = 0; i < 3; i++) {
            airline_code[i] = (char)tolower((unsigned char)raw_flight[i]);
        }
    }
    if (strcmp(airline_code, s_last_logo_code) == 0) {
        return;
    }
    snprintf(s_last_logo_code, sizeof(s_last_logo_code), "%s", airline_code);

    if (try_logo_code(airline_code)) {
        return;
    }

    /* Pure logging, no new widgets/UI/allocation - safe to leave in
     * permanently. Live misses (TAP224, AAL52, AA1357) were never
     * diagnosable before because nothing recorded what raw_flight/code
     * actually was at the moment of the miss - logo matching is keyed to
     * the live ADS-B callsign (e.g. "AAL1357"), not the resolved/pretty
     * flight number shown on screen, so an empty or unexpected raw_flight
     * (callsign not yet decoded, or sourced from a lookup that isn't
     * ICAO-prefixed) silently produces no logo with no visible reason
     * why. */
    ESP_LOGI(TAG, "no logo for raw_flight=\"%s\" (tried code=\"%s\")", raw_flight ? raw_flight : "(null)",
             airline_code);

    lv_obj_add_flag(s_airline_logo, LV_OBJ_FLAG_HIDDEN);
}

/* Updates the clock/date + room conditions every few seconds. Runs as an
 * LVGL timer, so it executes inside the lvgl_port task with the lock
 * already held - no separate locking needed here. */
/* Live dot pulse (explicit "make the green dot flicker" request) - a
 * smooth fade between full and ~25% opacity, ~1.2s per cycle. Only runs
 * while the line is visible (started/stopped in update_f1_session_
 * badge()), so it costs nothing outside a session; each frame just
 * redraws the 12x12 dot. */
static void f1_dot_opa_cb(void *obj, int32_t v)
{
    lv_obj_set_style_bg_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static void set_f1_dot_pulse(bool on)
{
    if (on == s_f1_dot_pulsing) {
        return;
    }
    s_f1_dot_pulsing = on;
    if (!on) {
        lv_anim_delete(s_f1_live_dot, f1_dot_opa_cb);
        lv_obj_set_style_bg_opa(s_f1_live_dot, LV_OPA_COVER, 0);
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_f1_live_dot);
    lv_anim_set_exec_cb(&a, f1_dot_opa_cb);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_20);
    lv_anim_set_duration(&a, 600);
    lv_anim_set_reverse_duration(&a, 600);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

/* Lock must already be held (LVGL timer context or a caller that took it).
 * Syncs the "f1_live" strip slot with the current session, expires it by
 * this device's own clock, and drives the dot's pulse/color. */
static void update_f1_session_badge(void)
{
    time_t now = time(NULL);
    bool clock_ok = now > 1700000000; /* only trust the clock once SNTP set it */
    if (s_f1_session_code[0] != '\0' && clock_ok && now >= s_f1_session_ends_at) {
        s_f1_session_code[0] = '\0';
        s_f1_session_dirty = true;
    }
    bool live = s_f1_session_code[0] != '\0';

    if (s_f1_session_dirty) {
        s_f1_session_dirty = false;
        notification_slot_t *slot = find_or_alloc_slot("f1_live");
        if (slot) {
            snprintf(slot->source, sizeof(slot->source), "f1_live");
            slot->priority = severity_priority("f1_live");
            slot->color = severity_color("f1_live");
            slot->text_color = lv_color_black();
            slot->is_spotify = false;
            slot->is_f1_live = true;
            slot->icon = NOTIF_ICON_NONE;
            slot->ttl_sec = 0;
            slot->age_sec = 0;
            snprintf(slot->message, sizeof(slot->message), "%s | %s", s_f1_session_code, s_f1_session_gp);
            slot->active = live;
            lv_label_set_text(s_f1_session_label, s_f1_session_code);
            lv_label_set_text(s_f1_session_gp_label, s_f1_session_gp);
            recompute_notification_display();
        }
    }

    set_f1_dot_pulse(live);
    bool orange = live && clock_ok && s_f1_session_ends_at - now <= 10 * 60;
    if (orange != s_f1_dot_orange) {
        s_f1_dot_orange = orange;
        lv_obj_set_style_bg_color(s_f1_live_dot,
                                  lv_palette_main(orange ? LV_PALETTE_ORANGE : LV_PALETTE_GREEN), 0);
    }
}

void home_screen_set_f1_session(const char *code, const char *gp, long long ends_at)
{
    if (!ui_lock()) {
        return;
    }
    snprintf(s_f1_session_code, sizeof(s_f1_session_code), "%s", code ? code : "");
    snprintf(s_f1_session_gp, sizeof(s_f1_session_gp), "%s", gp ? gp : "");
    s_f1_session_ends_at = (time_t)ends_at;
    s_f1_session_dirty = true;
    update_f1_session_badge();
    bsp_display_unlock();
}

static void home_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    update_f1_session_badge();

    char hhmm[6];
    if (wifi_time_get_hhmm(hhmm, sizeof(hhmm))) {
        time_t now = time(NULL);
        struct tm timeinfo;
        localtime_r(&now, &timeinfo);

        /* 12h with am/pm (e.g. "6:27pm"), not the 24h "18:27" wifi_time_
         * get_hhmm() itself returns - explicit request 2026-09-14, "in
         * pro of uniformity" with the rest of the device's 12h times
         * (weather's sunrise/sunset labels, etc.). hhmm/wifi_time_get_hhmm
         * stays 24h - still needed as-is for the zero-padded string
         * comparisons in update_sun_event_label(). strftime's %I is
         * zero-padded ("06:27PM") - trim the leading zero and lowercase
         * am/pm to match the reference format. */
        char clock_str[12];
        strftime(clock_str, sizeof(clock_str), "%I:%M%p", &timeinfo);
        char *clock_display = (clock_str[0] == '0') ? clock_str + 1 : clock_str;
        size_t clock_len = strlen(clock_display);
        clock_display[clock_len - 2] = (char)tolower((unsigned char)clock_display[clock_len - 2]);
        clock_display[clock_len - 1] = (char)tolower((unsigned char)clock_display[clock_len - 1]);
        lv_label_set_text(s_clock_label, clock_display);

        char date_str[16];
        strftime(date_str, sizeof(date_str), "%a %b %d", &timeinfo);
        lv_label_set_text(s_date_label, date_str);
    }

    /* Motion-based sleep, replacing the old fixed 11pm-7am clock schedule:
     * the room gets used at all hours, so "asleep" now tracks whether
     * anyone's actually in the room rather than the time of day. Sleeps
     * after IDLE_SLEEP_TIMEOUT_SEC with no new detection, wakes instantly
     * on one. If the sensor accessory isn't attached, this is a no-op and
     * the display just stays awake. */
    if (sensor_accessory_present()) {
        /* Brightness levels, explicit request 2026-09-27: day 20% active /
         * 5% idle, night (23:00-07:00) 5% active / 1% idle, touch live at
         * every level. "Asleep" now only means "at the idle level" - the
         * touch indev is never disabled any more (bsp_display_enter_sleep()
         * is still never called, see git history for the GT911 I2C abort
         * that ruled it out). At 23:00 the screen drops straight to the
         * night idle level; at 07:00 it steps up to the day level for
         * whichever state it's in. */
        bool night = wifi_time_is_night();
        if (night != s_was_night) {
            s_was_night = night;
            if (night) {
                s_display_asleep = true;
                /* Needs fresh motion to wake - older samples don't count. */
                memset(s_motion_history, 0, sizeof(s_motion_history));
            }
            bsp_display_brightness_set(s_display_asleep ? idle_brightness_pct(night) : active_brightness_pct(night));
            ESP_LOGI(TAG, "night mode %s", night ? "on" : "off");
        }

        bool level = sensor_accessory_motion_level();
        s_motion_history[s_motion_history_idx] = level;
        s_motion_history_idx = (s_motion_history_idx + 1) % MOTION_HISTORY_LEN;
        int high_count = 0;
        for (int i = 0; i < MOTION_HISTORY_LEN; i++) {
            if (s_motion_history[i]) {
                high_count++;
            }
        }
        bool confirmed = high_count >= MOTION_HISTORY_MIN_HIGH;
        /* Temporary - live-diagnosing "didn't sleep after ~2h away"
         * 2026-09-15. Remove once resolved. */
        ESP_LOGI(TAG, "motion raw=%d high_count=%d confirmed=%d idle_sec=%d asleep=%d", level, high_count, confirmed,
                 s_idle_sec, s_display_asleep);

        /* A touch counts as presence too - LVGL resets its inactivity
         * timer on every press, and this tick runs every 3s. */
        bool touched = lv_display_get_inactive_time(NULL) < 3000;

        if (confirmed || touched) {
            s_idle_sec = 0;
            if (s_display_asleep) {
                /* bsp_display_brightness_set(), not bsp_display_backlight_on()
                 * - the latter hardcodes 100% (bug found 2026-09-14). */
                bsp_display_brightness_set(active_brightness_pct(night));
                s_display_asleep = false;
                ESP_LOGI(TAG, "waking display (%d%%, %s)", active_brightness_pct(night), touched ? "touch" : "motion");
            }
        } else if (!s_display_asleep) {
            s_idle_sec += 3;
            if (s_idle_sec >= (night ? NIGHT_IDLE_SLEEP_TIMEOUT_SEC : IDLE_SLEEP_TIMEOUT_SEC)) {
                /* Dim rather than backlight off (2026-09-15: the radar is
                 * about as noisy in an empty room as an occupied one, so a
                 * false wake should be a small step, not a flash). */
                bsp_display_brightness_set(idle_brightness_pct(night));
                s_display_asleep = true;
                ESP_LOGI(TAG, "idle - display dimmed to %d%%", idle_brightness_pct(night));
            }
        }
    } else {
        ESP_LOGI(TAG, "sensor accessory not present - motion sleep disabled");
    }

    /* Notification auto-expiry - ticks regardless of display-asleep state
     * below, so a stale notification doesn't reappear on wake. Every
     * active slot ages independently; recompute_notification_display()
     * at the end picks up whatever's left. */
    {
        bool any_expired = false;
        for (int i = 0; i < NOTIFICATION_MAX_SOURCES; i++) {
            notification_slot_t *slot = &s_notification_slots[i];
            if (!slot->active || slot->ttl_sec <= 0) {
                continue;
            }
            slot->age_sec += 3;
            if (slot->age_sec >= slot->ttl_sec) {
                slot->active = false;
                any_expired = true;
            }
        }
        if (any_expired) {
            recompute_notification_display();
        }
    }

    update_sun_event_label();

    float temp_c = 0, humidity = 0;
    if (!sensor_accessory_get_humiture(&temp_c, &humidity)) {
        lv_label_set_text(s_conditions_label, "--\xC2\xB0\x46  --%");
        return;
    }

    float temp_f = temp_c * 9.0f / 5.0f + 32.0f;
    lv_label_set_text_fmt(s_conditions_label, "%.0f\xC2\xB0\x46  %.0f%%", temp_f, humidity);
}

/* Called from the MQTT client's task - takes the display lock itself.
 *
 * Real bug found live 2026-09-22: the aircraft chime (aircraft_alert.c) is
 * queued on its own task with no display-lock dependency at all, so it
 * always fires. This function used to try the lock for only 100ms and
 * silently give up on timeout - if a screen transition (200ms animation,
 * see main.c's screen_tap_cb) or another screen's own periodic redraw
 * happened to hold the lock at that instant, the chime played but the
 * on-screen info for that exact sighting was dropped with no retry and no
 * log, and by the next MQTT message the aircraft had often already left
 * the visibility window - reported live as "it beeped but showed nothing"
 * for a Cargolux 747 and an Atlas 747 seen directly overhead. bsp_display_
 * lock() is a real blocking mutex-take with a timeout (not a poll - see
 * esp-box-3.h), so raising the timeout genuinely waits out that
 * contention instead of just giving up faster; 400ms comfortably covers a
 * full 200ms screen transition plus normal jitter while staying well
 * short of the 3s poll interval. 2026-09-29: now ui_lock() (screens.h),
 * which waits with no timeout - 400ms could still lose to a long render, and
 * a lost sighting is exactly the bug above. The failure log stays in case
 * ui_lock() is ever bounded again. */
void home_screen_set_aircraft(const aircraft_info_t *info)
{
    if (!ui_lock()) {
        ESP_LOGW(TAG, "Display lock timed out - dropped aircraft update for %s",
                 info->raw_flight ? info->raw_flight : "(unknown)");
        return;
    }

    s_aircraft_active = true;
    lv_obj_add_flag(s_outlook_row, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_wind_humidity_row, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_weather_icon, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_weather_main, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_weather_detail, LV_OBJ_FLAG_HIDDEN);
    /* Hidden while an aircraft sighting is showing - explicit request
     * 2026-09-15 (originally for the news ticker this strip replaced;
     * same reasoning still applies - don't compete for space/attention
     * with the aircraft alert). Re-shown on clear only if a notification
     * is still actually active - see home_screen_clear_aircraft(). */
    lv_obj_add_flag(s_notification_strip, LV_OBJ_FLAG_HIDDEN);
    /* Same "don't compete with the aircraft alert" reasoning as the
     * notification strip above - explicit "only on this page, not on
     * the plane spotter" request. */
    update_unseen_badge();
    lv_obj_clear_flag(s_aircraft_main, LV_OBJ_FLAG_HIDDEN);

    /* Special display for every plane that chimes (747/A380/military) -
     * explicit request 2026-09-22 ("all the ones that beep"), replacing an
     * Atlas-only gold from earlier the same evening: gold headline (plus
     * the MILITARY tag on the type line below). A gold frame around the
     * column was tried too and removed after a live look - text only. */
    bool is_special = info->special_kind != NULL;
    lv_color_t color = is_special         ? lv_color_hex(SPECIAL_GOLD)
                       : info->is_watchlist ? lv_palette_main(LV_PALETTE_ORANGE)
                       : info->overhead   ? lv_palette_main(LV_PALETTE_BLUE)
                                          : lv_color_white();
    lv_obj_set_style_text_color(s_aircraft_main, color, 0);

    /* Plain ASCII only - the bundled font doesn't have glyphs for things
     * like the airplane emoji or a middle-dot separator (they rendered as
     * tofu boxes). Color alone (set above) signals overhead/watchlist. */
    char main_line[64];
    if (info->is_watchlist && info->label) {
        snprintf(main_line, sizeof(main_line), "%s", info->label);
    } else if (info->airline && info->flight_number) {
        snprintf(main_line, sizeof(main_line), "%s %s", info->airline, info->flight_number);
    } else if (info->raw_flight) {
        snprintf(main_line, sizeof(main_line), "%s", info->raw_flight);
    } else {
        snprintf(main_line, sizeof(main_line), "Aircraft");
    }
    lv_label_set_text(s_aircraft_main, main_line);
    update_aircraft_logo(info->raw_flight);

    /* Type is the priority info to show - prefer the full description
     * ("BOEING 777-200") over the short code ("B772") when it fits. */
    /* Military gets a spelled-out tag - a bare "C30J"/"H60" doesn't say
     * it. 747/A380 already read as such from the type itself. */
    bool is_military = info->special_kind && strcmp(info->special_kind, "military") == 0;
    const char *type_text = info->type_desc ? info->type_desc : info->type;
    if (type_text || is_military) {
        char type_line[64];
        snprintf(type_line, sizeof(type_line), "%s%s%s", type_text ? type_text : "",
                 (type_text && is_military) ? " - " : "", is_military ? "MILITARY" : "");
        lv_label_set_text(s_aircraft_type, type_line);
        lv_obj_clear_flag(s_aircraft_type, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_aircraft_type, LV_OBJ_FLAG_HIDDEN);
    }

    char detail_line[64];
    int written = snprintf(detail_line, sizeof(detail_line), "%.1fmi", info->distance_mi);
    if (written < 0) {
        written = 0;
    } else if ((size_t)written >= sizeof(detail_line)) {
        written = sizeof(detail_line) - 1;
    }
    if (info->alt_ft >= 0 && (size_t)written < sizeof(detail_line)) {
        int n = snprintf(detail_line + written, sizeof(detail_line) - written, "  %dft", info->alt_ft);
        written += (n > 0) ? n : 0;
        if ((size_t)written >= sizeof(detail_line)) {
            written = sizeof(detail_line) - 1;
        }
    }
    if (info->heading_deg >= 0 && (size_t)written < sizeof(detail_line)) {
        int n = snprintf(detail_line + written, sizeof(detail_line) - written, "  hdg %d\xC2\xB0", info->heading_deg);
        written += (n > 0) ? n : 0;
        if ((size_t)written >= sizeof(detail_line)) {
            written = sizeof(detail_line) - 1;
        }
    }
    if (info->origin_iata && info->destination_iata && (size_t)written < sizeof(detail_line)) {
        const char *tag = route_source_tag(info->route_source);
        if (tag) {
            snprintf(detail_line + written, sizeof(detail_line) - written, "\n%s -> %s (%s)",
                     info->origin_iata, info->destination_iata, tag);
        } else {
            snprintf(detail_line + written, sizeof(detail_line) - written, "\n%s -> %s",
                     info->origin_iata, info->destination_iata);
        }
    }
    lv_label_set_text(s_aircraft_detail, detail_line);
    lv_obj_clear_flag(s_aircraft_detail, LV_OBJ_FLAG_HIDDEN);

    lv_label_set_text(s_look_direction, compass_direction(info->bearing_deg));
    lv_obj_clear_flag(s_look_direction, LV_OBJ_FLAG_HIDDEN);

    bsp_display_unlock();
}

void home_screen_clear_aircraft(void)
{
    /* Same lock-contention reasoning as home_screen_set_aircraft() above -
     * a dropped clear would leave stale aircraft info stuck on screen
     * indefinitely (or until the next successful sighting overwrites it),
     * which is worse than a dropped set. */
    if (!ui_lock()) {
        ESP_LOGW(TAG, "Display lock timed out - failed to clear aircraft display");
        return;
    }

    s_aircraft_active = false;
    lv_obj_add_flag(s_aircraft_type, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_aircraft_detail, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_airline_logo, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_look_direction, LV_OBJ_FLAG_HIDDEN);
    /* Widget is now hidden regardless - clear the dedup tracker (see
     * update_aircraft_logo()) so the next sighting always re-decodes,
     * even if it turns out to be the same airline as before the gap. */
    s_last_logo_code[0] = '\0';
    /* Only re-shown if a notification is actually active - unlike the
     * old always-on status strip, this one stays hidden with nothing to
     * say. */
    recompute_notification_display();
    /* Back in the weather/idle state - the unseen badge is allowed to
     * show again now, if there's actually a count to show. */
    update_unseen_badge();

    if (s_have_weather) {
        /* Weather itself already says "nothing to show you here" - the
         * grey placeholder caption would just be redundant on top of it. */
        lv_obj_add_flag(s_aircraft_main, LV_OBJ_FLAG_HIDDEN);
        if (s_have_outlook) {
            lv_obj_clear_flag(s_outlook_row, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_clear_flag(s_wind_humidity_row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_weather_icon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_weather_main, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_weather_detail, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_set_style_text_color(s_aircraft_main, UI_TEXT_SECONDARY, 0);
        lv_label_set_text(s_aircraft_main, "No aircraft nearby");
        lv_obj_clear_flag(s_aircraft_main, LV_OBJ_FLAG_HIDDEN);
    }

    bsp_display_unlock();
}

/* red > orange > green > spotify > blue - see the priority-system
 * comment above the static declarations for why this lives on severity
 * itself rather than a per-source table. Returns -1 for an unrecognized
 * severity.
 *
 * green (currently just "NAS is on") moved above spotify/blue 2026-09-17
 * - explicit request, "having the nas on is a special event": originally
 * green sat at the very bottom, deliberately outranked by calendar
 * ("calendar takes priority over the routine green confirmation" - see
 * git history) so a real alert still interrupted everything but the
 * routine NAS confirmation never got to. That meant green could never
 * show at all whenever Calendar (or Spotify) had anything active - live
 * report confirmed the NAS banner was never actually appearing. Now
 * green gets its own 60s interrupt window same as red/orange, just one
 * tier below them, before reverting to whichever of spotify/calendar is
 * still active underneath (unchanged mechanism - see
 * NOTIFICATION_DISPLAY_MAX_TTL_SEC below). */
static int severity_priority(const char *severity)
{
    if (strcmp(severity, "red") == 0) {
        return 0;
    }
    if (strcmp(severity, "orange") == 0) {
        return 1;
    }
    if (strcmp(severity, "green") == 0) {
        return 2;
    }
    /* F1 session countdown (2026-09-24) - calendar-colored by request
     * ("it is a calendar event"), but time-critical and only ever up for
     * 15 minutes, so it outranks now-playing and the calendar's own
     * all-day line. Below the interrupt tier: never beats a real alert. */
    if (strcmp(severity, "f1") == 0) {
        return 3;
    }
    /* Spotify now-playing - explicit "if is playing music it would
     * display" request 2026-09-16, wants the track/artist to actually
     * show rather than get quietly outranked by the calendar's routine
     * "something's on today" line. Sits above blue so playback beats the
     * calendar, below the interrupt tier above. */
    if (strcmp(severity, "spotify") == 0) {
        return 4;
    }
    /* F1 session in progress - "music takes over the space", but it beats
     * the calendar (explicit request 2026-09-24). */
    if (strcmp(severity, "f1_live") == 0) {
        return 5;
    }
    if (strcmp(severity, "blue") == 0) {
        return 6;
    }
    return -1;
}

static lv_color_t severity_color(const char *severity)
{
    if (strcmp(severity, "red") == 0) {
        return lv_palette_main(LV_PALETTE_RED);
    }
    if (strcmp(severity, "orange") == 0) {
        return lv_palette_main(LV_PALETTE_ORANGE);
    }
    if (strcmp(severity, "spotify") == 0) {
        /* Spotify's own brand green (#1DB954), not LVGL's palette green -
         * deliberately distinct from "green" (NAS-is-on) below so the two
         * don't read as the same source at a glance. */
        return lv_color_hex(0x1DB954);
    }
    if (strcmp(severity, "blue") == 0 || strcmp(severity, "f1") == 0 || strcmp(severity, "f1_live") == 0) {
        /* F1 shares the calendar's cyan (explicit request 2026-09-24).
         * Cyan, explicit request 2026-09-16 - replaced the earlier purple
         * (0x6E63B0, matching the Claude Usage screen's badge pills).
         * Text is black against this (see severity_text_color() below),
         * not white like the purple version was. */
        return lv_palette_main(LV_PALETTE_CYAN);
    }
    return lv_palette_main(LV_PALETTE_GREEN); /* only reached for "green" - severity_priority() gates the rest */
}

/* Black across every severity, including calendar ("blue") - explicit
 * "the notifications appear with white lettering we should change all of
 * them to black" request 2026-09-16, broadened from the Spotify-only fix
 * earlier the same day (Spotify's bright brand green specifically was
 * hard to read in white - see git history). Calendar's background was
 * purple at that point, so it got a white-text exception; now that it's
 * cyan (see severity_color() above, same-day change), black reads
 * better and the exception is gone - `severity` is unused again, kept as
 * a parameter matching severity_color()'s own signature in case a future
 * severity needs its own exception. */
static lv_color_t severity_text_color(const char *severity)
{
    (void)severity;
    return lv_color_black();
}

/* Finds this source's own slot, creating one on first use. Each source
 * keeps its slot for the lifetime of the app (never freed back to the
 * pool even once inactive) - with only a handful of real sources
 * (systems, claude_usage, calendar, nas) NOTIFICATION_MAX_SOURCES has
 * plenty of headroom, so there's no need for the complexity of actually
 * reclaiming slots. Returns NULL only if genuinely out of slots. */
static notification_slot_t *find_or_alloc_slot(const char *source)
{
    notification_slot_t *free_slot = NULL;
    for (int i = 0; i < NOTIFICATION_MAX_SOURCES; i++) {
        if (s_notification_slots[i].source[0] != '\0' && strcmp(s_notification_slots[i].source, source) == 0) {
            return &s_notification_slots[i];
        }
        if (s_notification_slots[i].source[0] == '\0' && !free_slot) {
            free_slot = &s_notification_slots[i];
        }
    }
    return free_slot;
}

/* message is "title | artist" (n8n's build_notification_js in
 * rebuild_spotify_now_playing_workflow.py) - splits on the first '|',
 * trimming the whitespace n8n pads it with on both sides. Falls back to
 * treating the whole thing as the title (empty artist) if there's no '|'
 * at all - defensive, shouldn't happen for a real Spotify message. */
static void split_title_artist(const char *message, char *title, size_t title_size, char *artist, size_t artist_size)
{
    const char *sep = strchr(message, '|');
    if (!sep) {
        snprintf(title, title_size, "%s", message);
        artist[0] = '\0';
        return;
    }

    size_t title_len = (size_t)(sep - message);
    while (title_len > 0 && message[title_len - 1] == ' ') {
        title_len--;
    }
    if (title_len >= title_size) {
        title_len = title_size - 1;
    }
    memcpy(title, message, title_len);
    title[title_len] = '\0';

    const char *artist_start = sep + 1;
    while (*artist_start == ' ') {
        artist_start++;
    }
    snprintf(artist, artist_size, "%s", artist_start);
}

static void set_row_x_anim(void *var, int32_t v)
{
    lv_obj_set_x((lv_obj_t *)var, v);
}

/* Drives s_notification_row's x position - LVGL has no built-in way to
 * scroll a row of mixed-weight widgets as one unit the way
 * LV_LABEL_LONG_MODE_SCROLL_CIRCULAR does for a single label's own text,
 * so this hand-rolls the same idea: compare the row's natural content
 * width against the fixed-width clip viewport, center it statically if
 * it fits, or animate its x across it if it doesn't.
 *
 * Confirmed-stable fix (kept through a same-day revert that discarded an
 * unrelated, unstable feature - see git history/PROJECT.md):
 * 1. Skips the restart entirely when the message hasn't changed (see
 *    s_spotify_marquee_last_message below) - Spotify's poll republishes
 *    every 15s even when nothing changed, and this function used to
 *    unconditionally restart the scroll on every single call, reading
 *    live as "going very fast" (a repeating restart, not an actual rate
 *    change).
 * 2. speed=13, not 130 - matches s_notification_label (below) and
 *    spotify_screen.c's own s_now_playing_label exactly.
 * 3. Starts fully off-screen to the right (x = clip_w) and scrolls to
 *    fully off-screen left (x = -content_w) - a real "enters from the
 *    right, crosses the viewport, exits left" sweep, which is what
 *    "start on the right" turned out to mean live. No runway padding
 *    needed - the loop's repeat naturally restarts from x=clip_w, which
 *    is already blank/off-screen. */
static char s_spotify_marquee_last_message[NOTIFICATION_MESSAGE_MAX_LEN] = "";

static void update_spotify_marquee(const char *message)
{
    if (strcmp(message, s_spotify_marquee_last_message) == 0) {
        return;
    }
    snprintf(s_spotify_marquee_last_message, sizeof(s_spotify_marquee_last_message), "%s", message);

    char title[NOTIFICATION_MESSAGE_MAX_LEN];
    char artist[NOTIFICATION_MESSAGE_MAX_LEN];
    split_title_artist(message, title, sizeof(title), artist, sizeof(artist));

    lv_label_set_text(s_notification_title_label, title);
    lv_label_set_text(s_notification_artist_label, artist);
    /* An empty artist (title-only message - shouldn't happen for a real
     * Spotify message, but defensive) collapses the separator too,
     * rather than showing a bare trailing "|". */
    if (artist[0] == '\0') {
        lv_obj_add_flag(s_notification_sep_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_notification_artist_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(s_notification_sep_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_notification_artist_label, LV_OBJ_FLAG_HIDDEN);
    }

    /* Forces the pending flex-layout recompute (new text -> new content
     * width) to happen synchronously instead of on LVGL's own next async
     * pass - the exact same real bug this project already hit once with
     * logo sizing (try_logo_code(), see PROJECT.md) would otherwise apply
     * here too: reading the row's width right after changing its text
     * could see the stale pre-change size. */
    lv_obj_update_layout(s_notification_row);

    int32_t content_w = lv_obj_get_width(s_notification_row);
    int32_t clip_w = lv_obj_get_width(s_notification_clip);

    lv_anim_delete(s_notification_row, set_row_x_anim);

    if (content_w <= clip_w) {
        /* Fits - centered static, matching s_notification_label's own
         * look when its text fits. */
        lv_obj_set_x(s_notification_row, (clip_w - content_w) / 2);
        return;
    }

    int32_t start_value = clip_w;
    int32_t end_value = -content_w;
    uint32_t duration = lv_anim_speed_to_time(13, start_value, end_value);
    duration = LV_CLAMP(1500, duration, 20000);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_notification_row);
    lv_anim_set_exec_cb(&a, set_row_x_anim);
    lv_anim_set_values(&a, start_value, end_value);
    lv_anim_set_duration(&a, duration);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

/* Shows whichever active slot has the best (lowest-numbered) priority,
 * or hides the strip if nothing's active. Called after every set/clear/
 * expiry - the single place that actually touches the strip's LVGL
 * state, so display logic (color, text, hidden-while-aircraft) lives in
 * exactly one spot regardless of which slot changed. s_notification_label
 * itself is in LV_LABEL_LONG_MODE_SCROLL_CIRCULAR (see its creation
 * below), so a message too wide for the strip scrolls on its own - no
 * cycling logic needed here. Spotify is the one exception - its title/
 * artist marquee (update_spotify_marquee() above) needs two different
 * font weights LVGL can't mix in a single scrolling label. */
static void update_notification_icons(const notification_slot_t *best)
{
    if (best->icon == NOTIF_ICON_NONE || best->is_spotify) {
        lv_obj_add_flag(s_notification_icons, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_pad_left(s_notification_label, 0, 0);
        return;
    }
    /* NWS alerts (source "nws") reuse the F1 icon slot: one icon, no extra. */
    if (best->icon == NOTIF_ICON_NWS_WARNING) {
        lv_image_set_src(s_notification_icon_main, &nws_warning);
    } else if (best->icon == NOTIF_ICON_NWS_HURRICANE) {
        lv_image_set_src(s_notification_icon_main, &nws_hurricane);
    } else {
        lv_image_set_src(s_notification_icon_main, &f1_car);
    }
    if (best->icon == NOTIF_ICON_QUALI || best->icon == NOTIF_ICON_RACE) {
        lv_image_set_src(s_notification_icon_extra, best->icon == NOTIF_ICON_QUALI ? &f1_watch : &f1_flag);
        lv_obj_clear_flag(s_notification_icon_extra, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_notification_icon_extra, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_style_bg_color(s_notification_icons, best->color, 0);
    lv_obj_clear_flag(s_notification_icons, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(s_notification_icons);
    lv_obj_set_style_pad_left(s_notification_label, lv_obj_get_width(s_notification_icons) + 4, 0);
}

static void recompute_notification_display(void)
{
    notification_slot_t *best = NULL;
    for (int i = 0; i < NOTIFICATION_MAX_SOURCES; i++) {
        if (!s_notification_slots[i].active) {
            continue;
        }
        if (!best || s_notification_slots[i].priority < best->priority) {
            best = &s_notification_slots[i];
        }
    }

    if (!best) {
        lv_obj_add_flag(s_notification_strip, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_set_style_bg_color(s_notification_strip, best->color, 0);
    /* F1 live session: no background at all (explicit request - the cyan
     * bar was dropped for it), just the white row on the dark screen. */
    lv_obj_set_style_bg_opa(s_notification_strip, best->is_f1_live ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    update_notification_icons(best);
    if (best->is_f1_live) {
        lv_obj_add_flag(s_notification_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_notification_clip, LV_OBJ_FLAG_HIDDEN);
        lv_anim_delete(s_notification_row, set_row_x_anim);
        lv_obj_clear_flag(s_f1_session_group, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_f1_session_group, LV_OBJ_FLAG_HIDDEN);
    }
    if (best->is_f1_live) {
        /* handled above */
    } else if (best->is_spotify) {
        lv_obj_add_flag(s_notification_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_color(s_notification_title_label, best->text_color, 0);
        lv_obj_set_style_text_color(s_notification_sep_label, best->text_color, 0);
        lv_obj_set_style_text_color(s_notification_artist_label, best->text_color, 0);
        update_spotify_marquee(best->message);
        lv_obj_clear_flag(s_notification_clip, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_notification_clip, LV_OBJ_FLAG_HIDDEN);
        lv_anim_delete(s_notification_row, set_row_x_anim);
        lv_obj_set_style_text_color(s_notification_label, best->text_color, 0);
        lv_label_set_text(s_notification_label, best->message);
        lv_obj_clear_flag(s_notification_label, LV_OBJ_FLAG_HIDDEN);
    }
    if (!s_aircraft_active) {
        lv_obj_clear_flag(s_notification_strip, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Recomputes the unseen count from every slot's `unseen` flag and shows/
 * hides + relabels s_unseen_badge_group accordingly. Safe to call any
 * time the count OR the aircraft-active state might have changed - it's
 * just a few label/flag updates, no allocation. Hidden whenever
 * s_aircraft_active (explicit "only on this page, not on the plane
 * spotter" request), regardless of count. */
static void update_unseen_badge(void)
{
    int count = 0;
    for (int i = 0; i < NOTIFICATION_MAX_SOURCES; i++) {
        if (s_notification_slots[i].unseen) {
            count++;
        }
    }

    if (count == 0 || s_aircraft_active) {
        lv_obj_add_flag(s_unseen_badge_group, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_label_set_text_fmt(s_unseen_badge_count_label, "%d", count);
    lv_obj_clear_flag(s_unseen_badge_group, LV_OBJ_FLAG_HIDDEN);
}

/* See the shared declaration in screens.h. Takes the display lock
 * itself. */
void home_screen_set_notification(const char *source, const char *message, const char *severity, int ttl_min)
{
    home_screen_set_notification_ex(source, message, severity, ttl_min, NULL, false);
}

void home_screen_set_notification_ex(const char *source, const char *message, const char *severity, int ttl_min,
                                     const char *icon, bool play_sound)
{
    int priority = severity_priority(severity);
    if (priority < 0) {
        ESP_LOGW(TAG, "unknown notification severity \"%s\", ignoring", severity);
        return;
    }
    lv_color_t color = severity_color(severity);
    lv_color_t text_color = severity_text_color(severity);

    if (!ui_lock()) {
        return;
    }

    notification_slot_t *slot = find_or_alloc_slot(source);
    if (!slot) {
        ESP_LOGW(TAG, "no free notification slot for source \"%s\" (raise NOTIFICATION_MAX_SOURCES)", source);
        bsp_display_unlock();
        return;
    }

    /* A fresh occurrence is either a first-ever sighting for this source,
     * or the message text differing from what was last actually SHOWN
     * (last_notified_message) - NOT from slot->message, and NOT from
     * slot->active. Comparing against slot->active would mean an alert
     * that auto-hid after its 60s display window (see
     * NOTIFICATION_DISPLAY_MAX_TTL_SEC) looks "inactive" again on the next
     * unchanged republish and gets treated as fresh - exactly the "same
     * notification over and over" bug this replaced (explicit report:
     * Claude usage sitting flat at 80% kept reappearing every time its
     * ~2min poll republished, since each republish found the slot already
     * auto-expired and re-armed it). Only a real change reopens the
     * display window or beeps again; an ongoing-but-unchanged condition
     * stays suppressed until it changes or the source explicitly clears
     * it (see home_screen_clear_notification()). */
    bool fresh_occurrence = !slot->ever_notified || strcmp(slot->last_notified_message, message) != 0;
    if (fresh_occurrence && priority <= 1 /* red or orange */) {
        notification_sound_beep();
    }
    /* F1's own "start lights" sound, distinct from the alert beep and the
     * aircraft chime (explicit request). Same fresh_occurrence gate, so
     * each 15/10/5-minute message plays once; quiet hours are n8n's call. */
    if (fresh_occurrence && play_sound) {
        aircraft_alert_notify("f1");
    }
    /* Notification-history badge (see update_unseen_badge()/screens.h) -
     * only the interrupt tier (red/orange/green) counts as something
     * worth surfacing there; calendar/spotify are steady-state status,
     * not events. Uses the same fresh_occurrence gate as the beep above
     * so an unchanged republish (e.g. Claude usage sitting flat) doesn't
     * re-arm the badge either. */
    if (fresh_occurrence && priority <= 2 /* red, orange, or green */) {
        slot->unseen = true;
    }

    bool is_spotify = strcmp(severity, "spotify") == 0;
    /* Calendar ("blue") is exempt from the 60s display cap same as
     * Spotify - it's a persistent status ("what's the next event"), not a
     * one-off alert, and is expected to keep winning the strip over lower
     * priorities until the calendar workflow itself republishes something
     * different or clears it. Capping it caused a real regression the
     * same day this cap was added: it auto-expired after 60s, and since
     * its text hadn't changed it stayed suppressed (see fresh_occurrence
     * above) even after whatever had been covering it cleared - "the
     * calendar was supposed to appear after the other one was removed"
     * live report. */
    bool is_calendar = strcmp(severity, "blue") == 0;
    /* F1 alerts carry their own ttl (5 min - each one lasts until the next
     * countdown step replaces it) and are exempt from the 60s cap, which
     * would otherwise hide a "starts in 15 min" after a minute. */
    bool is_f1 = strcmp(severity, "f1") == 0;
    int ttl_sec = ttl_min > 0 ? ttl_min * 60 : 0;
    if (!is_spotify && !is_calendar && !is_f1 && (ttl_sec == 0 || ttl_sec > NOTIFICATION_DISPLAY_MAX_TTL_SEC)) {
        ttl_sec = NOTIFICATION_DISPLAY_MAX_TTL_SEC;
    }

    slot->priority = priority;
    slot->color = color;
    slot->text_color = text_color;
    slot->is_spotify = is_spotify;
    slot->is_f1_live = false;
    slot->icon = NOTIF_ICON_NONE;
    if (icon) {
        if (strcmp(icon, "practice") == 0) {
            slot->icon = NOTIF_ICON_PRACTICE;
        } else if (strcmp(icon, "quali") == 0) {
            slot->icon = NOTIF_ICON_QUALI;
        } else if (strcmp(icon, "race") == 0) {
            slot->icon = NOTIF_ICON_RACE;
        } else if (strcmp(icon, "warning") == 0) {
            slot->icon = NOTIF_ICON_NWS_WARNING;
        } else if (strcmp(icon, "hurricane") == 0) {
            slot->icon = NOTIF_ICON_NWS_HURRICANE;
        }
    }
    slot->ttl_sec = ttl_sec;
    if (fresh_occurrence) {
        slot->active = true;
        slot->age_sec = 0;
        snprintf(slot->last_notified_message, sizeof(slot->last_notified_message), "%s", message);
        slot->ever_notified = true;
    }
    snprintf(slot->source, sizeof(slot->source), "%s", source);
    snprintf(slot->message, sizeof(slot->message), "%s", message);

    recompute_notification_display();
    update_unseen_badge();
    bsp_display_unlock();
}

void home_screen_clear_notification(const char *source)
{
    if (!ui_lock()) {
        return;
    }

    notification_slot_t *slot = find_or_alloc_slot(source);
    if (slot) {
        /* Resets the dedup memory too, not just active - an explicit
         * clear means the condition actually resolved, so if this source
         * fires again later it's a genuinely new occurrence (fresh
         * beep + display window), not a suppressed repeat. */
        slot->ever_notified = false;
        slot->last_notified_message[0] = '\0';
        if (slot->active) {
            slot->active = false;
            recompute_notification_display();
        }
    }

    bsp_display_unlock();
}

/* See the shared declarations/notification_unseen_entry_t in screens.h.
 * Takes the display lock itself - safe to call from the Notification
 * screen's own tap/refresh handlers (the LVGL/UI task, not an MQTT
 * task), same lock every setter above already takes. */
int home_screen_get_unseen_notifications(notification_unseen_entry_t *out, int max)
{
    if (!ui_lock()) {
        return 0;
    }

    int count = 0;
    for (int i = 0; i < NOTIFICATION_MAX_SOURCES && count < max; i++) {
        notification_slot_t *slot = &s_notification_slots[i];
        if (!slot->unseen) {
            continue;
        }
        snprintf(out[count].source, sizeof(out[count].source), "%s", slot->source);
        snprintf(out[count].message, sizeof(out[count].message), "%s", slot->message);
        out[count].color = slot->color;
        count++;
    }

    bsp_display_unlock();
    return count;
}

void home_screen_dismiss_notification(const char *source)
{
    if (!ui_lock()) {
        return;
    }

    for (int i = 0; i < NOTIFICATION_MAX_SOURCES; i++) {
        if (s_notification_slots[i].unseen && strcmp(s_notification_slots[i].source, source) == 0) {
            s_notification_slots[i].unseen = false;
        }
    }
    update_unseen_badge();

    bsp_display_unlock();
}

void home_screen_dismiss_all_notifications(void)
{
    if (!ui_lock()) {
        return;
    }

    for (int i = 0; i < NOTIFICATION_MAX_SOURCES; i++) {
        s_notification_slots[i].unseen = false;
    }
    update_unseen_badge();

    bsp_display_unlock();
}

/* info->icon is one of "sun", "moon", "partly", "partly_night", "cloudy",
 * "fog", "rain", "snow", "thunder" (see weather_info_t's own comment in
 * screens.h) - matches one compiled wx_<name> descriptor each (see
 * tools/convert_ui_icons.py). Falls back to the sun icon for anything
 * else, rather than returning NULL and leaving the caller to decide -
 * every call site here always wants *some* valid image. */
const lv_image_dsc_t *wx_icon_lookup(const char *name)
{
    if (!name) {
        return &wx_sun;
    }
    if (strcmp(name, "sun") == 0) {
        return &wx_sun;
    }
    if (strcmp(name, "moon") == 0) {
        return &wx_moon;
    }
    if (strcmp(name, "partly") == 0) {
        return &wx_partly;
    }
    if (strcmp(name, "partly_night") == 0) {
        return &wx_partly_night;
    }
    if (strcmp(name, "cloudy") == 0) {
        return &wx_cloudy;
    }
    if (strcmp(name, "fog") == 0) {
        return &wx_fog;
    }
    if (strcmp(name, "rain") == 0) {
        return &wx_rain;
    }
    if (strcmp(name, "snow") == 0) {
        return &wx_snow;
    }
    if (strcmp(name, "thunder") == 0) {
        return &wx_thunder;
    }
    /* Detailed set, 2026-09-24 (n8n "icon_v2" / forecast icons). */
    if (strcmp(name, "mostly_cloudy") == 0) {
        return &wx_mostly_cloudy;
    }
    if (strcmp(name, "mostly_cloudy_night") == 0) {
        return &wx_mostly_cloudy_night;
    }
    if (strcmp(name, "sun_rain") == 0) {
        return &wx_sun_rain;
    }
    if (strcmp(name, "moon_rain") == 0) {
        return &wx_moon_rain;
    }
    if (strcmp(name, "rain_light") == 0) {
        return &wx_rain_light;
    }
    if (strcmp(name, "rain_heavy") == 0) {
        return &wx_rain_heavy;
    }
    return &wx_sun;
}

/* Called from the MQTT client's task - takes the display lock itself. */
void home_screen_set_weather(const weather_info_t *info)
{
    if (!ui_lock()) {
        return;
    }

    s_have_weather = true;

    /* Compiled-in, not a runtime SPIFFS decode (see
     * tools/convert_ui_icons.py) - same reasoning as update_sun_event_
     * label()'s own sun/arrow icons just above. wx_icon_lookup() falls
     * back to the sun icon for an unrecognized name rather than leaving
     * the widget showing whatever it last held, matching this widget's
     * pre-existing behavior (lv_image_set_src() with a bad path used to
     * just silently keep the old image). */
    lv_image_set_src(s_weather_icon, wx_icon_lookup(info->icon));

    /* Redesigned 2026-09-14, explicit request: condition icon beside the
     * temperature (was stacked above it), description text dropped
     * entirely from this row - the icon already conveys it, and the
     * reference layout this was matched to doesn't repeat it here.
     * Whole-degree temp (decimal removed again, same request that added it
     * a few hours earlier - the one-decimal precision lives on in the
     * skyops.weather_log column, just not shown on-screen), feels-like
     * moved onto the same row right after it (was its own row below) -
     * both parented to weather_main_row now instead of s_weather_detail
     * living directly under `info`. */
    lv_label_set_text_fmt(s_weather_main, "%.0f\xC2\xB0\x46", info->temp_f);

    lv_label_set_text_fmt(s_weather_detail, "Feels %d\xC2\xB0\x46", info->feels_like_f);

    /* Compact row: wind (static icon - no more rotating arrow, since the
     * short direction text already says where it's from - + bare speed +
     * short direction in a smaller font), humidity (drop + bare number, no
     * "%" label - a tiny "%" is baked into the icon itself instead, see
     * icon_humidity.png), next sun event (icon + time) - replaces the old
     * full-sentence "Wind from X at Ymph" / "Sunrise X  Sunset Y" rows
     * entirely. Bare numbers throughout is an explicit request ("no need
     * to add the knots"). */
    if (info->wind_mph >= 0) {
        lv_label_set_text_fmt(s_wind_label, "%d", info->wind_mph);
        if (info->wind_dir_deg >= 0) {
            lv_label_set_text(s_wind_dir_label, compass_abbrev(info->wind_dir_deg));
            lv_obj_clear_flag(s_wind_dir_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_wind_dir_label, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_clear_flag(s_wind_icon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_wind_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_wind_icon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_wind_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_wind_dir_label, LV_OBJ_FLAG_HIDDEN);
    }

    if (info->humidity_pct >= 0) {
        /* "%" moved back to text, at the end of the number - explicit
         * request 2026-09-14, undoing the same day's earlier "bake it into
         * the icon" version of this. icon_humidity.png is a plain drop
         * again. */
        lv_label_set_text_fmt(s_humidity_label, "%d%%", info->humidity_pct);
        lv_obj_clear_flag(s_humidity_icon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_humidity_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_humidity_icon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_humidity_label, LV_OBJ_FLAG_HIDDEN);
    }

    /* Cache the raw strings (see the static buffers' own comment for why
     * - the weather_info_t pointers don't outlive this call) and update
     * the label immediately, rather than waiting for the next 3s tick. */
    snprintf(s_sunrise_pretty, sizeof(s_sunrise_pretty), "%s", info->sunrise ? info->sunrise : "");
    snprintf(s_sunset_pretty, sizeof(s_sunset_pretty), "%s", info->sunset ? info->sunset : "");
    snprintf(s_sunrise_tomorrow_pretty, sizeof(s_sunrise_tomorrow_pretty), "%s",
             info->sunrise_tomorrow ? info->sunrise_tomorrow : "");
    snprintf(s_sunrise_24h, sizeof(s_sunrise_24h), "%s", info->sunrise_24h ? info->sunrise_24h : "");
    snprintf(s_sunset_24h, sizeof(s_sunset_24h), "%s", info->sunset_24h ? info->sunset_24h : "");
    s_have_sun_data = (s_sunrise_24h[0] != '\0' && s_sunset_24h[0] != '\0');
    update_sun_event_label();

    s_have_outlook = info->outlook_text && info->outlook_text[0] != '\0';
    if (s_have_outlook) {
        const char *c = info->outlook_color ? info->outlook_color : "";
        lv_palette_t pal = strcmp(c, "red") == 0      ? LV_PALETTE_RED
                           : strcmp(c, "orange") == 0 ? LV_PALETTE_ORANGE
                           : strcmp(c, "blue") == 0   ? LV_PALETTE_BLUE
                           : strcmp(c, "amber") == 0  ? LV_PALETTE_AMBER
                                                      : LV_PALETTE_GREEN;
        const char *ic = info->outlook_icon;
        if (ic && (strcmp(ic, "warning") == 0 || strcmp(ic, "hurricane") == 0)) {
            lv_image_set_src(s_outlook_icon, strcmp(ic, "hurricane") == 0 ? &nws_hurricane : &nws_warning);
            lv_obj_clear_flag(s_outlook_icon, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_outlook_dot, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_text_color(s_outlook_label, lv_palette_main(pal), 0);
        } else {
            lv_obj_set_style_bg_color(s_outlook_dot, lv_palette_main(pal), 0);
            lv_obj_clear_flag(s_outlook_dot, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_outlook_icon, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_text_color(s_outlook_label, lv_color_white(), 0);
        }
        lv_label_set_text(s_outlook_label, info->outlook_text);
    } else {
        lv_obj_add_flag(s_outlook_row, LV_OBJ_FLAG_HIDDEN);
    }

    if (!s_aircraft_active) {
        lv_obj_add_flag(s_aircraft_main, LV_OBJ_FLAG_HIDDEN);
        if (s_have_outlook) {
            lv_obj_clear_flag(s_outlook_row, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_clear_flag(s_wind_humidity_row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_weather_icon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_weather_main, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_weather_detail, LV_OBJ_FLAG_HIDDEN);
    }

    bsp_display_unlock();
}

lv_obj_t *home_screen_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    s_clock_label = lv_label_create(scr);
    lv_label_set_text(s_clock_label, "--:--");
    lv_obj_set_style_text_font(s_clock_label, &lv_font_montserrat_26, 0);
    lv_obj_align(s_clock_label, LV_ALIGN_TOP_LEFT, 12, 12);

    s_date_label = lv_label_create(scr);
    lv_label_set_text(s_date_label, "");
    lv_obj_set_style_text_color(s_date_label, UI_TEXT_SECONDARY, 0);
    lv_obj_align_to(s_date_label, s_clock_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 4);
    /* Read back where that actually landed (forcing the layout pass
     * synchronously, same reasoning as try_logo_code()'s own comment on
     * this) rather than hardcoding a guessed pixel offset, so the
     * unseen-badge group below can line up with this same row exactly -
     * explicit "lower them to the same level as the date and office
     * title" request 2026-09-17. */
    lv_obj_update_layout(s_date_label);
    int32_t date_row_y = lv_obj_get_y(s_date_label);

    /* Identifies s_conditions_label as the onboard sensor's own reading
     * (the room this box is sitting in) rather than the outdoor weather
     * shown lower on the screen - explicit request 2026-09-14, to tell
     * the two temperature readings apart at a glance. Was a small
     * building icon (office_icon.png) next to the reading; changed
     * 2026-09-15 to a plain "office" caption underneath it instead,
     * styled to match s_date_label exactly (same default font/grey) so
     * the two read as a matched pair, right-justified like everything
     * else in this column. A real flex column, not lv_obj_align_to() off
     * the reading itself - the align_to version landed underneath the
     * clock instead of next to the reading as intended (same class of
     * positioning fragility this codebase already avoids elsewhere with
     * flex rows, e.g. weather_main_row). */
    lv_obj_t *conditions_row = lv_obj_create(scr);
    lv_obj_set_size(conditions_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(conditions_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(conditions_row, 0, 0);
    lv_obj_set_style_pad_all(conditions_row, 0, 0);
    lv_obj_set_style_pad_row(conditions_row, 0, 0);
    lv_obj_set_flex_flow(conditions_row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(conditions_row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_clear_flag(conditions_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(conditions_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(conditions_row, LV_ALIGN_TOP_RIGHT, -12, 12);

    s_conditions_label = lv_label_create(conditions_row);
    lv_obj_set_style_text_color(s_conditions_label, lv_color_white(), 0);
    lv_label_set_text(s_conditions_label, "--\xC2\xB0\x46  --%");
    lv_obj_set_style_text_font(s_conditions_label, &lv_font_montserrat_26, 0);

    lv_obj_t *office_label = lv_label_create(conditions_row);
    lv_obj_set_style_text_color(office_label, UI_TEXT_SECONDARY, 0);
    lv_obj_set_style_text_align(office_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(office_label, "office");

    /* Notification-history envelope + unseen-count badge, explicit
     * request 2026-09-17 - see home_screen_dismiss_notification()/
     * screens.h for the full design. Centered on the screen horizontally,
     * lined up with the date/office row's actual y (date_row_y, read
     * back above - not a guessed offset) rather than the clock/temp
     * row itself - explicit "lower them to the same level as the date
     * and office title, make sure it's in the center of the page"
     * request 2026-09-17, after an earlier plain y=24 guess still didn't
     * match that row exactly. Hidden by default (shown once there's a
     * real unseen count, and only in this screen's weather/idle state -
     * see update_unseen_badge() and home_screen_set_aircraft()/
     * home_screen_clear_aircraft()). Plain built-in LV_SYMBOL_ENVELOPE
     * (no new image asset), same approach already confirmed working
     * on-device by spotify_screen.c's LV_SYMBOL_PREV/NEXT. */
    /* Same row, side by side - explicit "dot and envelope should be on
     * the same row" 2026-09-17 follow-up, reverting the column layout
     * the previous attempt used - and both ~25% smaller than that
     * attempt's sizes ("way too big" report): envelope font 26 -> 20,
     * badge pad_all 6 -> 4. */
    /* Centered row for the envelope + count (it briefly also held the F1
     * badge; kept as the envelope's container). */
    lv_obj_t *status_row = lv_obj_create(scr);
    lv_obj_set_size(status_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(status_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(status_row, 0, 0);
    lv_obj_set_style_pad_all(status_row, 0, 0);
    lv_obj_set_style_pad_column(status_row, 14, 0);
    lv_obj_set_flex_flow(status_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(status_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(status_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(status_row, LV_ALIGN_TOP_MID, 0, date_row_y);

    s_unseen_badge_group = lv_obj_create(status_row);
    lv_obj_set_size(s_unseen_badge_group, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_unseen_badge_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_unseen_badge_group, 0, 0);
    lv_obj_set_style_pad_all(s_unseen_badge_group, 0, 0);
    lv_obj_set_style_pad_column(s_unseen_badge_group, 4, 0);
    lv_obj_set_flex_flow(s_unseen_badge_group, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_unseen_badge_group, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(s_unseen_badge_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_unseen_badge_group, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_unseen_badge_group, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *envelope_label = lv_label_create(s_unseen_badge_group);
    lv_obj_set_style_text_color(envelope_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(envelope_label, &lv_font_montserrat_20, 0);
    lv_label_set_text(envelope_label, LV_SYMBOL_ENVELOPE);

    /* A label styled into a small filled circle (same trick
     * claude_usage_screen.c's own pill badges use - but a real fixed-size
     * container with the count label centered inside via flex, not the
     * label itself styled as the circle. Styling the label directly (the
     * previous attempt) sized the "circle" from the label's own content
     * box, and a single digit glyph is narrower than it is tall - the
     * radius ended up constrained by that narrower width, drawing as a
     * vertically-elongated pill instead of round (live-caught
     * 2026-09-17, "elongated on the top and bottom"). A fixed 20x20
     * square is round regardless of which digit it's showing, and flex
     * centering handles both axes properly (LVGL labels have no native
     * vertical-align property to lean on otherwise). */
    lv_obj_t *badge_circle = lv_obj_create(s_unseen_badge_group);
    lv_obj_set_size(badge_circle, 20, 20);
    lv_obj_set_style_bg_opa(badge_circle, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(badge_circle, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_set_style_border_width(badge_circle, 0, 0);
    lv_obj_set_style_radius(badge_circle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(badge_circle, 0, 0);
    lv_obj_set_flex_flow(badge_circle, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(badge_circle, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(badge_circle, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(badge_circle, LV_OBJ_FLAG_CLICKABLE);

    s_unseen_badge_count_label = lv_label_create(badge_circle);
    lv_obj_set_style_text_color(s_unseen_badge_count_label, lv_color_white(), 0);
    lv_label_set_text(s_unseen_badge_count_label, "1");

    /* Column of aircraft info that stacks and re-centers itself based on
     * actual content height, instead of fixed pixel offsets - a long
     * airline name wrapping to 2 lines used to push everything below it
     * into a cramped overlap since the old layout never accounted for it. */
    lv_obj_t *info = lv_obj_create(scr);
    lv_obj_set_size(info, 300, LV_SIZE_CONTENT);
    /* Centered (not top-anchored) so short content (e.g. no route line, no
     * logo) sits centered in the free space between the date row and the
     * page dots instead of floating high - the offset accounts for that
     * free band's center sitting a bit below the screen's true center.
     * Was +24, which on the tallest content (logo + wrapped airline name +
     * type + 2-line distance/route + look-direction) pushed the last line
     * down far enough to nearly touch the page dots; +12 matches the
     * band's actual measured center (date row ends ~y50, dots start ~y216
     * on the 320x240 panel) and leaves clearance either way.
     * +12 -> +2 - explicit request 2026-09-15: the weather layout now has
     * a 4th row (the condition icon, its own row), and even with pad_row
     * trimmed to a minimum the bottom row was still touching the news
     * ticker - shifting the whole column up a bit gives it room without
     * needing to shrink any of the rows themselves.
     * +2 -> +7 - explicit "everything is pushed to the top, lower it ~5px"
     * request 2026-09-17. */
    lv_obj_align(info, LV_ALIGN_CENTER, 0, 7);
    lv_obj_set_style_bg_opa(info, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(info, 0, 0);
    lv_obj_set_style_pad_all(info, 0, 0);
    /* 6 -> 3 -> 1 -> 0 -> 8 - explicit request 2026-09-15 (the first four
     * steps were "the 3 rows of data could be closer among them", back
     * when there were 4 stacked rows; now back to 2,
     * weather_main_row + s_wind_humidity_row, after reverting to the
     * pre-split layout later the same day). Opened back up, same day:
     * "add some more space between the two rows" - proactively, ahead of
     * a forecast icon change (cloudy conditions expected tomorrow) that's
     * taller than today's sun icon and would otherwise touch the row
     * below at 0 padding. */
    lv_obj_set_style_pad_row(info, 8, 0);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(info, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(info, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(info, LV_OBJ_FLAG_CLICKABLE);


    /* No blanket background chip behind the logo (removed 2026-09-11 for
     * testing, explicit request) - most bundled logos have enough color/
     * light content to read fine directly on the dark screen, and a white
     * chip behind all of them looked worse than the problem it solved.
     * 2026-09-13: confirmed via per-pixel luminance analysis of every
     * bundled PNG (opacity-weighted, against this screen's near-black
     * background) plus a visual pass on the borderline ones - a real
     * subset are genuinely dark navy/black ink that disappears without
     * help. Those specific codes get a white chip via
     * try_logo_code()/logo_needs_white_bg() below instead of a global one.
     * Content-sized (with a max-width cap so an extreme wordmark still
     * can't blow past the 300px-wide info column) keeps the widget sized
     * to whatever image is loaded. */
    s_airline_logo = lv_image_create(info);
    lv_obj_set_size(s_airline_logo, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(s_airline_logo, 180, 0);
    lv_obj_set_style_pad_all(s_airline_logo, 4, 0);
    lv_obj_set_style_radius(s_airline_logo, 4, 0);
    lv_obj_set_style_bg_opa(s_airline_logo, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(s_airline_logo, LV_OBJ_FLAG_HIDDEN);

    s_aircraft_main = lv_label_create(info);
    lv_obj_set_width(s_aircraft_main, 280);
    lv_obj_set_style_text_align(s_aircraft_main, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_aircraft_main, UI_TEXT_SECONDARY, 0);
    lv_label_set_text(s_aircraft_main, "No aircraft nearby");
    lv_obj_set_style_text_font(s_aircraft_main, &lv_font_montserrat_20, 0);

    s_aircraft_type = lv_label_create(info);
    lv_obj_set_width(s_aircraft_type, 300);
    lv_obj_set_style_text_align(s_aircraft_type, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_aircraft_type, lv_color_white(), 0);
    lv_label_set_text(s_aircraft_type, "");
    lv_obj_add_flag(s_aircraft_type, LV_OBJ_FLAG_HIDDEN);

    s_aircraft_detail = lv_label_create(info);
    lv_obj_set_width(s_aircraft_detail, 300);
    lv_obj_set_style_text_align(s_aircraft_detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_aircraft_detail, UI_TEXT_SECONDARY, 0);
    lv_label_set_text(s_aircraft_detail, "");
    lv_obj_add_flag(s_aircraft_detail, LV_OBJ_FLAG_HIDDEN);

    s_look_direction = lv_label_create(info);
    lv_obj_set_width(s_look_direction, 300);
    lv_obj_set_style_text_align(s_look_direction, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_look_direction, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_set_style_text_font(s_look_direction, &lv_font_montserrat_20, 0);
    lv_obj_add_flag(s_look_direction, LV_OBJ_FLAG_HIDDEN);

    /* Back to the pre-2026-09-15 2-row layout (icon+temp+feels on one
     * row, wind/humidity/sun compact on the other) - explicit request
     * 2026-09-15 evening, "go back to this [reference photo]" (the last
     * screenshot before that day's 4-row split redesign), "same font
     * size" (keep today's uniform 20px sizing below, not the old
     * montserrat_26 temp), "leave the ticket [ticker] untouched" (no
     * changes to s_news_ticker). This undoes the icon's-own-row +
     * wind/humidity-vs-sunrise split from earlier today; s_sunrise_row is
     * gone, its 3 widgets now live in s_wind_humidity_row alongside
     * wind/humidity, matching the reference photo's single compact row.
     *
     * Icon sizing itself is unchanged from the split version (native
     * 24x24 + scale 427 to reach ~40x40) - root-caused 2026-09-15 via
     * lv_snapshot_take() (a genuine independent re-render through LVGL's
     * real draw pipeline): a 40x40 NATIVE wx_*.png decoded to zero
     * non-transparent pixels on this build regardless of any object-state
     * variant tried, while shrinking the native asset to 24x24 (matching
     * every other icon on this screen) immediately fixed it - some
     * decode/allocation limit, not a layout bug. */
    s_outlook_row = lv_obj_create(info);
    lv_obj_set_size(s_outlook_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_outlook_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_outlook_row, 0, 0);
    lv_obj_set_style_pad_all(s_outlook_row, 0, 0);
    lv_obj_set_style_pad_column(s_outlook_row, 5, 0);
    lv_obj_set_flex_flow(s_outlook_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_outlook_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(s_outlook_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_outlook_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_outlook_row, LV_OBJ_FLAG_HIDDEN);
    s_outlook_dot = lv_obj_create(s_outlook_row);
    lv_obj_set_size(s_outlook_dot, 9, 9);
    lv_obj_set_style_radius(s_outlook_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_outlook_dot, 0, 0);
    lv_obj_clear_flag(s_outlook_dot, LV_OBJ_FLAG_CLICKABLE);
    s_outlook_icon = lv_image_create(s_outlook_row);
    lv_image_set_src(s_outlook_icon, &nws_warning);
    lv_obj_add_flag(s_outlook_icon, LV_OBJ_FLAG_HIDDEN);
    s_outlook_label = lv_label_create(s_outlook_row);
    lv_obj_set_style_text_font(s_outlook_label, &lv_font_montserrat_16_latin_ext, 0);
    lv_obj_set_style_text_color(s_outlook_label, lv_color_white(), 0);
    lv_label_set_text(s_outlook_label, "");

    lv_obj_t *weather_main_row = lv_obj_create(info);
    lv_obj_set_size(weather_main_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(weather_main_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(weather_main_row, 0, 0);
    lv_obj_set_style_pad_all(weather_main_row, 0, 0);
    lv_obj_set_style_pad_column(weather_main_row, 6, 0);
    lv_obj_set_flex_flow(weather_main_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(weather_main_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(weather_main_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(weather_main_row, LV_OBJ_FLAG_CLICKABLE);

    /* 40x40 -> 34x34 ("15% smaller", explicit request 2026-09-15:
     * 40*0.85=34). 256*34/24 = 362.67, rounded - scales the 24x24 native
     * asset up to a 34x34 apparent size, matching the box. */
    s_weather_icon = lv_image_create(weather_main_row);
    lv_obj_set_size(s_weather_icon, 34, 34);
    lv_image_set_scale(s_weather_icon, 363);
    lv_obj_add_flag(s_weather_icon, LV_OBJ_FLAG_HIDDEN);

    /* Same size as the sensor's own reading (s_conditions_label,
     * montserrat_26, top-right) - explicit request 2026-09-15, "the
     * current outside temperature the same size as the sensor
     * temperature". Was montserrat_20 (matched "feels" and every other
     * weather row instead, a few requests ago) - "feels" stays at 20,
     * only this label changes. Earlier still, briefly a real bold face
     * (compiled solely for this label) after an explicit request to bold
     * just this one, reverted the same day; the bold font file was
     * deleted since nothing else used it. */
    s_weather_main = lv_label_create(weather_main_row);
    lv_obj_set_style_text_color(s_weather_main, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_weather_main, &lv_font_montserrat_26, 0);
    lv_obj_add_flag(s_weather_main, LV_OBJ_FLAG_HIDDEN);

    /* Feels-like - explicit request 2026-09-14, "put right next to it the
     * feels... on the same row". White (was grey) - a later same-day
     * request. */
    s_weather_detail = lv_label_create(weather_main_row);
    lv_obj_set_style_text_color(s_weather_detail, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_weather_detail, &lv_font_montserrat_20, 0);
    lv_obj_add_flag(s_weather_detail, LV_OBJ_FLAG_HIDDEN);

    /* Compact row: wind, humidity, next sun event, all together - back to
     * this single row (was split into s_wind_humidity_row/s_sunrise_row
     * earlier 2026-09-15, undone same evening per the comment above).
     * Icons are fixed-size (not LV_SIZE_CONTENT) since they never change
     * dimensions - sidesteps the whole class of bug fixed elsewhere today
     * (LV_SIZE_CONTENT not resizing synchronously when a new image source
     * is assigned after creation) rather than relying on it. */
    s_wind_humidity_row = lv_obj_create(info);
    lv_obj_set_size(s_wind_humidity_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_wind_humidity_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_wind_humidity_row, 0, 0);
    lv_obj_set_style_pad_all(s_wind_humidity_row, 0, 0);
    lv_obj_set_style_pad_column(s_wind_humidity_row, 4, 0);
    lv_obj_set_flex_flow(s_wind_humidity_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_wind_humidity_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(s_wind_humidity_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_wind_humidity_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_wind_humidity_row, LV_OBJ_FLAG_HIDDEN);

    /* Static (non-rotating) wind glyph, not the old arrow - explicit
     * request 2026-09-14: the compass abbreviation text already says the
     * direction, so a rotating arrow was redundant. Box/scale history,
     * same day: 20x20 native -> 24x24 native ("20% larger", revealing the
     * source PNG's own margin, previously cropped by the smaller box) ->
     * 26x26 box + 277/256 scale ("10% more"), briefly 31x31 + 331/256
     * ("too large"), settled on 29x29 box + 309/256 scale (10% over the
     * 26x26 step: 26*1.1=28.6, scale = 256*29/24 to keep the same
     * visible-size/native-size ratio). Past 24x24 native, going bigger
     * needs actual scaling, not just a bigger box - same reasoning as
     * s_weather_icon above. */
    s_wind_icon = lv_image_create(s_wind_humidity_row);
    lv_obj_set_size(s_wind_icon, 29, 29);
    lv_image_set_scale(s_wind_icon, 309);
    lv_image_set_src(s_wind_icon, &icon_wind);

    s_wind_label = lv_label_create(s_wind_humidity_row);
    lv_obj_set_style_text_color(s_wind_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_wind_label, &lv_font_montserrat_20, 0);

    /* Font size history, same day: 12 -> 14 -> 16, briefly 20 ("too
     * large"), settled on 18 (10% over 16). Bumped to 20 2026-09-15,
     * explicit request to make every weather row the same font as "feels"
     * (s_weather_detail) - was the one remaining outlier. White (was
     * grey) - an earlier same-day request. */
    s_wind_dir_label = lv_label_create(s_wind_humidity_row);
    lv_obj_set_style_text_color(s_wind_dir_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_wind_dir_label, &lv_font_montserrat_20, 0);

    s_humidity_icon = lv_image_create(s_wind_humidity_row);
    lv_obj_set_size(s_humidity_icon, 29, 29);
    lv_image_set_scale(s_humidity_icon, 309);
    lv_image_set_src(s_humidity_icon, &icon_humidity);

    s_humidity_label = lv_label_create(s_wind_humidity_row);
    lv_obj_set_style_text_color(s_humidity_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_humidity_label, &lv_font_montserrat_20, 0);

    /* Always the sun icon regardless of sunrise/sunset - hardcoded per
     * 2026-09-15 request; only the time label and arrow direction change
     * in update_sun_event_label(). Same compact row as wind/humidity now,
     * not its own row. */
    s_sun_icon = lv_image_create(s_wind_humidity_row);
    lv_obj_set_size(s_sun_icon, 29, 29);
    lv_image_set_scale(s_sun_icon, 309);
    lv_image_set_src(s_sun_icon, &sun_icon);

    s_sun_time_label = lv_label_create(s_wind_humidity_row);
    lv_obj_set_style_text_color(s_sun_time_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_sun_time_label, &lv_font_montserrat_20, 0);

    /* Real arrow shape (arrow_up.png/arrow_down.png), not LV_SYMBOL_UP/
     * DOWN - those render as a plain chevron in the bundled icon font,
     * not an actual arrow - explicit request 2026-09-14. 16x16 native,
     * 18x18 + 288/256, now 21x21 box + 336/256 scale ("a little bit
     * bigger", another explicit request: 18*1.15=20.7, scale =
     * 256*21/16), same reasoning as the icons above once past native
     * size. */
    s_sun_arrow_icon = lv_image_create(s_wind_humidity_row);
    lv_obj_set_size(s_sun_arrow_icon, 21, 21);
    lv_image_set_scale(s_sun_arrow_icon, 336);
    lv_image_set_src(s_sun_arrow_icon, &arrow_up);

    /* Bottom notification strip - see the static declarations above for
     * the full design. Hidden by default; home_screen_set_notification()
     * colors it, sets the text, and shows it. */
    s_notification_strip = lv_obj_create(scr);
    lv_obj_set_width(s_notification_strip, lv_pct(92));
    lv_obj_set_height(s_notification_strip, LV_SIZE_CONTENT);
    lv_obj_clear_flag(s_notification_strip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(s_notification_strip, 0, 0);
    lv_obj_set_style_bg_opa(s_notification_strip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_notification_strip, 4, 0);
    lv_obj_set_style_pad_hor(s_notification_strip, 8, 0);
    lv_obj_set_style_pad_ver(s_notification_strip, 4, 0);
    lv_obj_align(s_notification_strip, LV_ALIGN_BOTTOM_MID, 0, -26);
    lv_obj_add_flag(s_notification_strip, LV_OBJ_FLAG_HIDDEN);

    s_notification_label = lv_label_create(s_notification_strip);
    lv_label_set_text(s_notification_label, "");
    lv_obj_set_style_text_color(s_notification_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_notification_label, &lv_font_montserrat_16_latin_ext, 0);
    /* Fixed width (not LV_SIZE_CONTENT) is required for SCROLL_CIRCULAR to
     * ever detect an overflow at all - it compares the rendered text size
     * against this object's own width, so a content-sized label could
     * never be "too wide for itself". Text stays centered (matches the
     * previous lv_obj_center() look) whenever it actually fits; LVGL
     * itself switches to left-aligned only once it starts scrolling (see
     * lv_label.c's own SCROLL_CIRCULAR draw path). */
    lv_obj_set_width(s_notification_label, lv_pct(100));
    lv_obj_set_style_text_align(s_notification_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_notification_label, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    /* LVGL's own default scroll speed (LV_LABEL_DEF_SCROLL_SPEED in
     * lv_label.c) is 40 (~400px/s) - too fast to comfortably read live,
     * explicit "it really should go slower" report 2026-09-16. ~130px/s
     * instead (speed=13), same lv_anim_speed_clamped() encoding LVGL
     * itself uses so it stays a rate (consistent pace regardless of
     * message length) rather than a fixed duration - min/max bumped up
     * to match (a short overflow shouldn't finish in under 1.5s, a very
     * long one shouldn't run past 20s). */
    lv_obj_set_style_anim_duration(s_notification_label, lv_anim_speed_clamped(13, 1500, 20000), 0);
    lv_obj_center(s_notification_label);

    /* F1 icon group - see the static declarations above. */
    s_notification_icons = lv_obj_create(s_notification_strip);
    lv_obj_set_size(s_notification_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_clear_flag(s_notification_icons, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(s_notification_icons, 0, 0);
    lv_obj_set_style_radius(s_notification_icons, 0, 0);
    lv_obj_set_style_pad_all(s_notification_icons, 0, 0);
    lv_obj_set_style_pad_column(s_notification_icons, 3, 0);
    lv_obj_set_style_bg_opa(s_notification_icons, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(s_notification_icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_notification_icons, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(s_notification_icons, LV_ALIGN_LEFT_MID, 0, 0);
    s_notification_icon_main = lv_image_create(s_notification_icons);
    lv_image_set_src(s_notification_icon_main, &f1_car);
    s_notification_icon_extra = lv_image_create(s_notification_icons);
    lv_image_set_src(s_notification_icon_extra, &f1_watch);
    lv_obj_add_flag(s_notification_icons, LV_OBJ_FLAG_HIDDEN);

    /* F1 live-session row - see the static declarations at the top. Lives
     * inside the strip, centered; montserrat 16 (20 read too big, 14 too
     * small), white text - the strip's background is turned off while this
     * row shows (recompute_notification_display()); live dot 12px, pulsing. */
    s_f1_session_group = lv_obj_create(s_notification_strip);
    lv_obj_set_size(s_f1_session_group, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(s_f1_session_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_f1_session_group, 0, 0);
    lv_obj_set_style_pad_all(s_f1_session_group, 0, 0);
    lv_obj_set_style_pad_column(s_f1_session_group, 6, 0);
    lv_obj_set_flex_flow(s_f1_session_group, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_f1_session_group, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(s_f1_session_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_f1_session_group, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *f1_car_img = lv_image_create(s_f1_session_group);
    lv_image_set_src(f1_car_img, &f1_car);
    s_f1_session_label = lv_label_create(s_f1_session_group);
    lv_obj_set_style_text_color(s_f1_session_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_f1_session_label, &lv_font_montserrat_16, 0);
    lv_label_set_text(s_f1_session_label, "");
    s_f1_live_dot = lv_obj_create(s_f1_session_group);
    lv_obj_set_size(s_f1_live_dot, 12, 12);
    lv_obj_set_style_radius(s_f1_live_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_f1_live_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_f1_live_dot, lv_palette_main(LV_PALETTE_GREEN), 0);
    lv_obj_set_style_border_width(s_f1_live_dot, 0, 0);
    lv_obj_clear_flag(s_f1_live_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_f1_live_dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *f1_sep = lv_label_create(s_f1_session_group);
    lv_obj_set_style_text_color(f1_sep, lv_color_white(), 0);
    lv_obj_set_style_text_font(f1_sep, &lv_font_montserrat_16, 0);
    lv_label_set_text(f1_sep, "|");
    /* Capped width with "..." so an unusually long GP name can't push the
     * row past the 300px column. */
    s_f1_session_gp_label = lv_label_create(s_f1_session_group);
    lv_obj_set_style_text_color(s_f1_session_gp_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_f1_session_gp_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_max_width(s_f1_session_gp_label, 190, 0);
    lv_label_set_long_mode(s_f1_session_gp_label, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(s_f1_session_gp_label, "");
    lv_obj_center(s_f1_session_group);
    lv_obj_add_flag(s_f1_session_group, LV_OBJ_FLAG_HIDDEN);

    /* Spotify title/artist marquee - see the static declarations' comment
     * above for the full design. Created hidden; recompute_notification_
     * display() shows whichever of this or s_notification_label matches
     * the active source, and drives update_spotify_marquee() below. */
    s_notification_clip = lv_obj_create(s_notification_strip);
    lv_obj_set_width(s_notification_clip, lv_pct(100));
    lv_obj_set_height(s_notification_clip, LV_SIZE_CONTENT);
    lv_obj_clear_flag(s_notification_clip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(s_notification_clip, 0, 0);
    lv_obj_set_style_bg_opa(s_notification_clip, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_notification_clip, 0, 0);
    lv_obj_add_flag(s_notification_clip, LV_OBJ_FLAG_HIDDEN);

    /* Sized to its own natural content width (can exceed s_notification_
     * clip's width) - update_spotify_marquee() reads that width to decide
     * whether/how far to animate x, the same "compare content size to the
     * viewport" idea LV_LABEL_LONG_MODE_SCROLL_CIRCULAR itself uses. */
    s_notification_row = lv_obj_create(s_notification_clip);
    lv_obj_set_size(s_notification_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_clear_flag(s_notification_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(s_notification_row, 0, 0);
    lv_obj_set_style_bg_opa(s_notification_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_notification_row, 0, 0);
    lv_obj_set_flex_flow(s_notification_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_notification_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_notification_row, 6, 0);
    /* No trailing pad set here (0 by default from pad_all above) - it's
     * only added in update_spotify_marquee() when the row actually needs
     * to scroll. Setting it unconditionally here made the "fits, should
     * be centered" case measure content_w 60px wider than the real
     * visible text, off-centering it - real bug, caught live 2026-09-16
     * ("if you leave like this it needs to be centered"). */
    lv_obj_set_pos(s_notification_row, 0, 0);

    s_notification_title_label = lv_label_create(s_notification_row);
    lv_obj_set_style_text_font(s_notification_title_label, &lv_font_montserrat_16_latin_ext_bold, 0);

    s_notification_sep_label = lv_label_create(s_notification_row);
    lv_label_set_text(s_notification_sep_label, "|");
    lv_obj_set_style_text_font(s_notification_sep_label, &lv_font_montserrat_16_latin_ext, 0);

    s_notification_artist_label = lv_label_create(s_notification_row);
    lv_obj_set_style_text_font(s_notification_artist_label, &lv_font_montserrat_16_latin_ext, 0);

    screens_add_page_dots(scr, SCREEN_HOME);

    lv_timer_create(home_timer_cb, 3000, NULL);

    return scr;
}
