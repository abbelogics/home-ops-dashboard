#include <stdio.h>

#include "bsp/esp-box-3.h"
#include "screens.h"
#include "spotify_mqtt.h"
#include "ui_icons/ui_icons.h"

/* Custom-generated extended-Latin fonts (see home_screen.c's own
 * LV_FONT_DECLARE for the full rationale - real song/artist names need
 * accented characters, the bundled Montserrat fonts are ASCII-only).
 * Missed applying these here the first time this screen got its
 * track/artist text back - caught live 2026-09-16 ("the special
 * characters"), same "Versión" showing as a missing-glyph box that
 * prompted the original fix on the Home screen's notification strip. */
LV_FONT_DECLARE(lv_font_montserrat_14_latin_ext);
/* Larger companion, same extended range - explicit "make the title of
 * the song larger" follow-up, used for the now-playing line. Bumped to
 * 18px that day, then dialed back to 16px same as the home banner's own
 * notification text ("make the text ... 16") - both pull from the same
 * fonts/lv_font_montserrat_16_latin_ext.c. */
LV_FONT_DECLARE(lv_font_montserrat_16_latin_ext);
/* Bold - "Play"/"Pause"/"..." are pure ASCII, but LVGL's bundled
 * Montserrat has no bold weight at all, bundled or not - this project's
 * only bold font. Used for the play/pause button label; started at 14px
 * ("Pause text should be bold"), bumped to 18px same day ("make the
 * font of Pause larger"). */
LV_FONT_DECLARE(lv_font_montserrat_18_latin_ext_bold);

/* Spotify Connect transport controls, targeting the Bose Smart Soundbar
 * (see ../n8n/build_spotify_control_workflow.py's device-by-name
 * resolution). Track/artist re-added 2026-09-16 as ONE combined label
 * (not the original three separate track/artist/device labels) - that
 * fuller version pushed a marginal internal-SRAM boot race into an
 * actual intermittent task-watchdog hang, caught live the same day (see
 * PROJECT.md); the real fix turned out to be consolidating this
 * project's webhook-POST tasks (see webhook_post.h), but keeping the
 * widget count leaner here too costs nothing and is cheap insurance. A
 * device label came back too, same day, placed under the volume slider
 * per an explicit follow-up ("place the device where the music is
 * coming from underneath the volume slider"). */
static lv_obj_t *s_now_playing_label;
static lv_obj_t *s_play_pause_button;
static lv_obj_t *s_play_pause_label;
static lv_obj_t *s_volume_slider;
static lv_obj_t *s_volume_label;
static lv_obj_t *s_device_label;

static bool s_current_playing = false;
static bool s_pending = false; /* waiting on the webhook round-trip's MQTT confirmation */
static bool s_volume_dragging = false; /* user has a finger on the slider right now */

static void set_play_pause_style(lv_color_t bg, const char *text)
{
    lv_obj_set_style_bg_color(s_play_pause_button, bg, 0);
    lv_label_set_text(s_play_pause_label, text);
}

static void play_pause_click_cb(lv_event_t *event)
{
    (void)event;

    if (s_pending) {
        return; /* already waiting on a previous tap */
    }

    bool target_playing = !s_current_playing;
    s_pending = true;
    set_play_pause_style(lv_palette_main(LV_PALETTE_GREY), "...");
    spotify_mqtt_control(target_playing ? "play" : "pause");
}

static void previous_click_cb(lv_event_t *event)
{
    (void)event;
    spotify_mqtt_control("previous");
}

static void next_click_cb(lv_event_t *event)
{
    (void)event;
    spotify_mqtt_control("next");
}

static void update_volume_label(int32_t pct)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "Volume: %d%%", (int)pct);
    lv_label_set_text(s_volume_label, buf);
}

static void volume_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    int32_t value = lv_slider_get_value(s_volume_slider);

    if (code == LV_EVENT_PRESSED) {
        s_volume_dragging = true;
    } else if (code == LV_EVENT_VALUE_CHANGED) {
        /* Live label feedback while dragging, but don't hit the network on
         * every intermediate tick - only the final released value matters. */
        update_volume_label(value);
    } else if (code == LV_EVENT_RELEASED) {
        s_volume_dragging = false;
        spotify_mqtt_set_volume(value);
    }
}

/* Called from the MQTT client's own task - takes the display lock itself.
 * This is the actual confirmation (the next 15s poll's real state), not
 * just an optimistic button-tap guess - clears s_pending regardless of
 * whether this update matches what was just requested, same reasoning as
 * lights_screen_set_state(). volume_percent follows the same "no-op
 * while the user has a finger on it" guard lights_screen_set_brightness()
 * uses for its own slider, for the same reason: a mid-drag MQTT update
 * (the 15s poll, or the post-set re-poll) can't be allowed to yank the
 * slider out from under an in-progress gesture. */
void spotify_screen_set_state(bool is_playing, const char *track, const char *artist, const char *device,
                               int volume_percent)
{
    if (!ui_lock()) {
        return;
    }

    s_current_playing = is_playing;
    s_pending = false;
    if (is_playing) {
        set_play_pause_style(lv_palette_main(LV_PALETTE_GREEN), "Pause");
    } else {
        set_play_pause_style(lv_palette_main(LV_PALETTE_GREY), "Play");
    }

    if (track && track[0]) {
        if (artist && artist[0]) {
            char buf[96];
            snprintf(buf, sizeof(buf), "%s - %s", track, artist);
            lv_label_set_text(s_now_playing_label, buf);
        } else {
            lv_label_set_text(s_now_playing_label, track);
        }
    } else {
        lv_label_set_text(s_now_playing_label, "Nothing playing");
    }

    if (device && device[0]) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Playing on %s", device);
        lv_label_set_text(s_device_label, buf);
    } else {
        lv_label_set_text(s_device_label, "--");
    }

    if (!s_volume_dragging) {
        if (volume_percent >= 0) {
            lv_slider_set_value(s_volume_slider, volume_percent, LV_ANIM_ON);
            update_volume_label(volume_percent);
        } else {
            lv_label_set_text(s_volume_label, "Volume: --");
        }
    }

    bsp_display_unlock();
}

lv_obj_t *spotify_screen_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Vertical band is y=8 to y=196 (screens_add_page_dots() in main.c
     * reserves BOTTOM_MID,-8 for the page-dot row on every screen). Upper
     * cluster (title/now-playing/buttons) is top-anchored; lower cluster
     * (volume/device) is bottom-anchored off the page-dot row, same
     * "-28" safe offset lights_screen.c's own voice_hint uses - more
     * robust than accumulating top-down offsets for content whose total
     * height isn't fixed in advance. */
    lv_obj_t *header_row = lv_obj_create(scr);
    lv_obj_set_size(header_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(header_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(header_row, 0, 0);
    lv_obj_set_style_pad_column(header_row, 6, 0);
    lv_obj_set_style_bg_opa(header_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header_row, 0, 0);
    lv_obj_clear_flag(header_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(header_row, LV_ALIGN_TOP_MID, 0, 8);

    /* Same 24x24-native-then-scale pattern as the Claude Usage screen's
     * own header logo (see claude_usage_screen.c) - this project's PNG
     * decoder silently fails above ~40x40, so every logo asset stays
     * native-small and reaches its on-screen size via lv_image_set_scale()
     * instead of a larger source file. Explicit "same thing you did on
     * the claude usage page" request 2026-09-16. */
    lv_obj_t *logo = lv_image_create(header_row);
    lv_image_set_src(logo, &spotify_logo);
    lv_image_set_scale(logo, 300);

    lv_obj_t *title = lv_label_create(header_row);
    lv_label_set_text(title, "Spotify");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_26, 0);

    /* Was LV_LABEL_LONG_MODE_DOTS (fixed height, single-line "..."
     * truncation) - explicit "make it like a ticker" follow-up once a
     * long track+artist line just got cut off rather than being fully
     * readable. Same SCROLL_CIRCULAR mechanism as the Home screen's own
     * notification-strip marquee (see home_screen.c) - width-only
     * constraint (no height needed; this mode measures/scrolls a single
     * line by design, it doesn't wrap), same ~130px/s pace. A short line
     * that already fits just sits there static, same as before. */
    s_now_playing_label = lv_label_create(scr);
    lv_label_set_text(s_now_playing_label, "--");
    lv_obj_set_style_text_font(s_now_playing_label, &lv_font_montserrat_16_latin_ext, 0);
    lv_obj_set_width(s_now_playing_label, lv_pct(90));
    lv_obj_set_style_text_align(s_now_playing_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_now_playing_label, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    /* 13 -> 8, explicit "going too fast" request 2026-09-16 - this speed
     * constant is independent of the home banner marquee's own "13"
     * (separate hand-rolled lv_anim, see update_spotify_marquee() in
     * home_screen.c), even though they happened to share the same
     * number. */
    lv_obj_set_style_anim_duration(s_now_playing_label, lv_anim_speed_clamped(8, 1500, 20000), 0);
    lv_obj_align(s_now_playing_label, LV_ALIGN_TOP_MID, 0, 46);

    /* Transport row: Previous | Play/Pause | Next, all clickable so a tap
     * here targets the button directly rather than falling through to
     * the screen's own tap handler (which cycles to the next screen).
     * Real bug caught live 2026-09-16 ("forward and backward are
     * touching play/pause"): prev at x=-80 (width 60, right edge -50)
     * and the play/pause button (width 100, left edge -50) shared the
     * exact same edge - zero gap, not just visually tight. Widened
     * spacing (x=+-95, narrower 55px side buttons, 90px center button)
     * leaves a real ~20px gap on each side. y=82 - went to 104 briefly to
     * clear the now-playing label's old wrap bug, then pulled back up
     * once that got fixed properly (fixed height + DOTS truncation, see
     * that label's own comment) - 104 was "too much" per a live report.
     * All three solid green with black icons/text
     * - explicit "make all the buttons green like the pause/play, but
     * with black lettering" follow-up (play/pause's own grey/green
     * still reflects paused/playing state, just black text either way,
     * bold per a further follow-up). */
    lv_obj_t *prev_button = lv_obj_create(scr);
    lv_obj_set_size(prev_button, 55, 42);
    lv_obj_align(prev_button, LV_ALIGN_TOP_MID, -95, 82);
    lv_obj_set_style_radius(prev_button, 12, 0);
    lv_obj_set_style_border_width(prev_button, 0, 0);
    lv_obj_set_style_bg_color(prev_button, lv_palette_main(LV_PALETTE_GREEN), 0);
    lv_obj_clear_flag(prev_button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(prev_button, previous_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *prev_label = lv_label_create(prev_button);
    lv_label_set_text(prev_label, LV_SYMBOL_PREV);
    lv_obj_set_style_text_color(prev_label, lv_color_black(), 0);
    lv_obj_center(prev_label);

    s_play_pause_button = lv_obj_create(scr);
    lv_obj_set_size(s_play_pause_button, 90, 42);
    lv_obj_align(s_play_pause_button, LV_ALIGN_TOP_MID, 0, 82);
    lv_obj_set_style_radius(s_play_pause_button, 12, 0);
    lv_obj_set_style_border_width(s_play_pause_button, 0, 0);
    lv_obj_set_style_bg_opa(s_play_pause_button, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_play_pause_button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_play_pause_button, play_pause_click_cb, LV_EVENT_CLICKED, NULL);
    s_play_pause_label = lv_label_create(s_play_pause_button);
    lv_obj_set_style_text_color(s_play_pause_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(s_play_pause_label, &lv_font_montserrat_18_latin_ext_bold, 0);
    lv_obj_center(s_play_pause_label);
    set_play_pause_style(lv_palette_main(LV_PALETTE_GREY), "--");

    lv_obj_t *next_button = lv_obj_create(scr);
    lv_obj_set_size(next_button, 55, 42);
    lv_obj_align(next_button, LV_ALIGN_TOP_MID, 95, 82);
    lv_obj_set_style_radius(next_button, 12, 0);
    lv_obj_set_style_border_width(next_button, 0, 0);
    lv_obj_set_style_bg_color(next_button, lv_palette_main(LV_PALETTE_GREEN), 0);
    lv_obj_clear_flag(next_button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(next_button, next_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *next_label = lv_label_create(next_button);
    lv_label_set_text(next_label, LV_SYMBOL_NEXT);
    lv_obj_set_style_text_color(next_label, lv_color_black(), 0);
    lv_obj_center(next_label);

    /* Volume label + slider + device label, bottom-anchored as one
     * cluster (see the top-of-function comment) - synced to Spotify's
     * own real current volume/device (see spotify_screen_set_state())
     * instead of static placeholders, explicit "we need to see the
     * percentage of the volume" / "place the device ... underneath the
     * volume slider" follow-ups. Device label is white, not the earlier
     * grey - explicit follow-up, easier to read. Both bumped to the
     * larger 18px fonts too, same "make ... larger" request as the
     * now-playing label above - kept the same bottom offsets (-68/-46/
     * -28) since they already had enough clearance for the size bump. */
    s_volume_label = lv_label_create(scr);
    lv_label_set_text(s_volume_label, "Volume: --");
    lv_obj_set_style_text_font(s_volume_label, &lv_font_montserrat_18, 0);
    lv_obj_align(s_volume_label, LV_ALIGN_BOTTOM_MID, 0, -85);

    s_volume_slider = lv_slider_create(scr);
    lv_obj_set_size(s_volume_slider, 220, 8);
    lv_obj_align(s_volume_slider, LV_ALIGN_BOTTOM_MID, 0, -63);
    lv_obj_set_style_pad_all(s_volume_slider, 2, LV_PART_KNOB);
    lv_slider_set_range(s_volume_slider, 0, 100);
    lv_slider_set_value(s_volume_slider, 50, LV_ANIM_OFF);
    lv_obj_add_event_cb(s_volume_slider, volume_event_cb, LV_EVENT_ALL, NULL);

    s_device_label = lv_label_create(scr);
    lv_label_set_text(s_device_label, "--");
    /* Back down to the smaller 14px extended font - "Playing on Bose
     * Smart Soundbar" is a long phrase, and at 18px (this screen's other
     * two labels' size) it didn't fit well - explicit "make it smaller"
     * follow-up. */
    lv_obj_set_style_text_font(s_device_label, &lv_font_montserrat_14_latin_ext, 0);
    lv_obj_set_style_text_color(s_device_label, lv_color_white(), 0);
    lv_obj_align(s_device_label, LV_ALIGN_BOTTOM_MID, 0, -28);

    screens_add_page_dots(scr, SCREEN_SPOTIFY);

    return scr;
}
