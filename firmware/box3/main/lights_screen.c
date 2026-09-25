#include <stdio.h>

#include "bsp/esp-box-3.h"
#include "lights_mqtt.h"
#include "screens.h"

/* Office lamp control (see ../n8n/rebuild_lights_control_workflow.py) -
 * real button added 2026-09-11, explicit request. Was read-only ("Voice
 * control only") until then; that text is gone now, this screen actually
 * drives the light. */
static lv_obj_t *s_button;
static lv_obj_t *s_button_label;
static lv_obj_t *s_slider;
static lv_obj_t *s_brightness_label;
static lv_obj_t *s_voice_status;
static bool s_current_on = false;
static bool s_pending = false; /* waiting on the webhook round-trip's MQTT confirmation */
static bool s_slider_dragging = false; /* user has a finger on the slider right now */

static void set_button_style(lv_color_t bg, const char *text)
{
    lv_obj_set_style_bg_color(s_button, bg, 0);
    lv_label_set_text(s_button_label, text);
}

static void button_click_cb(lv_event_t *event)
{
    (void)event;

    if (s_pending) {
        return; /* already waiting on a previous tap */
    }

    bool target = !s_current_on;
    s_pending = true;
    set_button_style(lv_palette_main(LV_PALETTE_GREY), "Setting...");
    lights_mqtt_set(target);
}

/* Called from the MQTT client's own task - takes the display lock itself.
 * This is the actual confirmation (the bridge's real state, re-polled by
 * the n8n workflow after every set command), not just an optimistic
 * button-tap guess - clears s_pending regardless of whether this update
 * matches what was just requested. */
void lights_screen_set_state(bool on)
{
    if (!bsp_display_lock(100)) {
        return;
    }

    s_current_on = on;
    s_pending = false;
    if (on) {
        set_button_style(lv_palette_main(LV_PALETTE_GREEN), "ON - tap to turn off");
    } else {
        set_button_style(lv_palette_main(LV_PALETTE_GREY), "OFF - tap to turn on");
    }

    bsp_display_unlock();
}

static void update_brightness_label(int bri)
{
    /* Hue's 1-254 range shown as a friendlier 1-100% - display only,
     * lights_mqtt.c still speaks Hue's native units end to end. */
    int pct = (bri * 100) / 254;
    if (pct < 1) {
        pct = 1;
    }
    char buf[24];
    snprintf(buf, sizeof(buf), "Brightness: %d%%", pct);
    lv_label_set_text(s_brightness_label, buf);
}

static void slider_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    int32_t value = lv_slider_get_value(s_slider);

    if (code == LV_EVENT_PRESSED) {
        s_slider_dragging = true;
    } else if (code == LV_EVENT_VALUE_CHANGED) {
        /* Live label feedback while dragging, but don't hit the network
         * on every intermediate tick - only the final released value
         * matters to Hue. */
        update_brightness_label(value);
    } else if (code == LV_EVENT_RELEASED) {
        s_slider_dragging = false;
        lights_mqtt_set_brightness(value);
    }
}

/* Called from the MQTT client's own task - takes the display lock itself.
 * A no-op while the user has a finger on the slider (s_slider_dragging),
 * so the 30s state poll (or the post-set re-poll after a previous
 * request) can't yank the slider out from under an in-progress drag. */
void lights_screen_set_brightness(int bri)
{
    if (!bsp_display_lock(100)) {
        return;
    }

    if (!s_slider_dragging) {
        lv_slider_set_value(s_slider, bri, LV_ANIM_ON);
        update_brightness_label(bri);
    }

    bsp_display_unlock();
}

/* Called from the voice recognition task (lights_voice.c) - takes the
 * display lock itself. */
void lights_screen_set_voice_status(const char *status)
{
    if (!bsp_display_lock(100)) {
        return;
    }

    lv_label_set_text(s_voice_status, status ? status : "");

    bsp_display_unlock();
}

lv_obj_t *lights_screen_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Laid out with explicit TOP_MID offsets (not CENTER, like before the
     * brightness slider was added) - on a 320x240 display, every widget
     * needs a predictable, non-overlapping slot; CENTER-relative offsets
     * in both directions got hard to reason about once there were six
     * stacked widgets instead of four. Deliberately even ~16-18px gaps
     * between each (not the original ad-hoc offsets, which left the
     * button/brightness-label/slider/voice-status nearly touching each
     * other while title/lamp-name had much more room - looked "justified
     * to the top" instead of using the display's full height). Available
     * band is y=8 (top) to y=196 (screens_add_page_dots() in main.c
     * reserves BOTTOM_MID,-8 for the page-dot row on every screen, and
     * this screen's own voice_hint sits right above that at -28, i.e.
     * top edge ~196) - six widgets have to fit in that ~188px. */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Lights");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_26, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t *lamp_name = lv_label_create(scr);
    lv_label_set_text(lamp_name, "Office Lamp");
    lv_obj_align(lamp_name, LV_ALIGN_TOP_MID, 0, 44);

    /* The actual button - clickable, so a tap here targets this object
     * directly rather than falling through to the screen's own tap
     * handler (which cycles to the next screen). Taps outside the button
     * still cycle screens as usual. */
    s_button = lv_obj_create(scr);
    lv_obj_set_size(s_button, 200, 42);
    lv_obj_align(s_button, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_set_style_radius(s_button, 12, 0);
    lv_obj_set_style_border_width(s_button, 0, 0);
    lv_obj_set_style_bg_opa(s_button, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_button, button_click_cb, LV_EVENT_CLICKED, NULL);

    s_button_label = lv_label_create(s_button);
    lv_obj_set_style_text_color(s_button_label, lv_color_white(), 0);
    lv_obj_center(s_button_label);

    set_button_style(lv_palette_main(LV_PALETTE_GREY), "--");

    /* Voice feedback (see lights_voice.c) - "Listening..." after the wake
     * word, then the recognized command briefly, or blank the rest of the
     * time. Grouped right under the button (both are about the on/off
     * state), with the brightness controls as their own group below. */
    s_voice_status = lv_label_create(scr);
    lv_label_set_text(s_voice_status, "");
    lv_obj_set_style_text_color(s_voice_status, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_align(s_voice_status, LV_ALIGN_TOP_MID, 0, 120);

    /* Brightness label + slider are one paired unit (the label directly
     * describes the control right below it), so these two sit close
     * together - unlike the generous gaps between the button/voice-status/
     * brightness groups above. The slider previously sat far enough down
     * to visually collide with voice_hint at the bottom of the screen -
     * its rendered knob extends past the 20px box set on the object
     * itself, so the real footprint is bigger than the layout math alone
     * suggested. Pulled the whole group up and shrunk the slider's own
     * height a bit to leave real clearance above voice_hint, not just a
     * couple of pixels. */
    s_brightness_label = lv_label_create(scr);
    lv_label_set_text(s_brightness_label, "Brightness: --");
    lv_obj_set_style_text_font(s_brightness_label, &lv_font_montserrat_14, 0);
    lv_obj_align(s_brightness_label, LV_ALIGN_TOP_MID, 0, 142);

    s_slider = lv_slider_create(scr);
    lv_obj_set_size(s_slider, 220, 8);
    lv_obj_align(s_slider, LV_ALIGN_TOP_MID, 0, 172);
    /* Default theme knob is padded out well past the track's own height
     * (a bigger touch target) - trimmed here since it was visually heavy
     * next to an 8px-tall track. Still leaves the knob a bit larger than
     * the track itself, which is fine/expected for a touchscreen. */
    lv_obj_set_style_pad_all(s_slider, 2, LV_PART_KNOB);
    lv_slider_set_range(s_slider, 1, 254);
    lv_slider_set_value(s_slider, 254, LV_ANIM_OFF);
    lv_obj_add_event_cb(s_slider, slider_event_cb, LV_EVENT_ALL, NULL);

    lv_obj_t *voice_hint = lv_label_create(scr);
    lv_label_set_text(voice_hint, "Say \"Hi, ESP\" then \"Lights On/Off\"");
    lv_obj_set_style_text_color(voice_hint, UI_TEXT_SECONDARY, 0);
    lv_obj_set_style_text_font(voice_hint, &lv_font_montserrat_14, 0);
    /* -28, not -8: screens_add_page_dots() (main.c) puts the page-dot row
     * at BOTTOM_MID,-8 on every screen - this needs clearance above it. */
    lv_obj_align(voice_hint, LV_ALIGN_BOTTOM_MID, 0, -28);

    screens_add_page_dots(scr, SCREEN_LIGHTS);

    return scr;
}
