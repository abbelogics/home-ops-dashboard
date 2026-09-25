#include "bsp/esp-box-3.h"
#include "screens.h"
#include "totp.h"

/* Local 2FA code display (see totp.c/Kconfig.projbuild) - fully offline,
 * no MQTT, refreshes itself once/second via its own LVGL timer rather
 * than reacting to any external state (same pattern home_screen.c's own
 * clock uses). Added 2026-09-15 alongside the MQTT-consolidation fix, so
 * kept deliberately light on internal RAM: no new MQTT client, no image
 * assets, just two labels and a bar. */
static lv_obj_t *s_code_label;
static lv_obj_t *s_countdown_label;
static lv_obj_t *s_countdown_bar;
static lv_obj_t *s_status_label;

static void totp_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    char code[7];
    if (!totp_get_code(code, sizeof(code))) {
        lv_obj_add_flag(s_code_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_countdown_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_countdown_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_clear_flag(s_code_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_countdown_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_countdown_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);

    /* "123 456" not "123456" - a mid-code gap is how every authenticator
     * app displays a 6-digit TOTP code, purely for at-a-glance
     * readability (nothing to do with the algorithm itself). */
    lv_label_set_text_fmt(s_code_label, "%.3s %.3s", code, code + 3);

    int remaining = totp_seconds_remaining();
    lv_label_set_text_fmt(s_countdown_label, "%ds", remaining);
    lv_bar_set_value(s_countdown_bar, remaining, LV_ANIM_OFF);
}

lv_obj_t *totp_screen_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Authenticator");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_26, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    s_code_label = lv_label_create(scr);
    lv_label_set_text(s_code_label, "--- ---");
    lv_obj_set_style_text_font(s_code_label, &lv_font_montserrat_44, 0);
    lv_obj_align(s_code_label, LV_ALIGN_CENTER, 0, -20);
    lv_obj_add_flag(s_code_label, LV_OBJ_FLAG_HIDDEN);

    s_countdown_bar = lv_bar_create(scr);
    lv_obj_set_size(s_countdown_bar, 160, 8);
    lv_bar_set_range(s_countdown_bar, 0, 30);
    lv_bar_set_value(s_countdown_bar, 30, LV_ANIM_OFF);
    lv_obj_align(s_countdown_bar, LV_ALIGN_CENTER, 0, 30);
    lv_obj_add_flag(s_countdown_bar, LV_OBJ_FLAG_HIDDEN);

    s_countdown_label = lv_label_create(scr);
    lv_label_set_text(s_countdown_label, "30s");
    lv_obj_set_style_text_color(s_countdown_label, UI_TEXT_SECONDARY, 0);
    lv_obj_align(s_countdown_label, LV_ALIGN_CENTER, 0, 50);
    lv_obj_add_flag(s_countdown_label, LV_OBJ_FLAG_HIDDEN);

    /* Shown instead of the code/bar/countdown above whenever
     * totp_get_code() can't produce one yet (time not synced at boot) or
     * ever (CONFIG_TOTP_SECRET_BASE32 empty/invalid). */
    s_status_label = lv_label_create(scr);
    lv_label_set_text(s_status_label, "Waiting for time sync...");
    lv_obj_set_style_text_color(s_status_label, UI_TEXT_SECONDARY, 0);
    lv_obj_align(s_status_label, LV_ALIGN_CENTER, 0, 0);

    screens_add_page_dots(scr, SCREEN_TOTP);

    lv_timer_create(totp_timer_cb, 1000, NULL);

    return scr;
}
