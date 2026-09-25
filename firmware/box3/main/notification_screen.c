#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "screens.h"

/* Notification history - a plain list of currently-unseen green/red/
 * orange events (see home_screen.c's update_unseen_badge() and
 * NOTIFICATION_HISTORY_MAX in screens.h), each dismissible on its own
 * tap, plus a "Clear all" button. No MQTT client, no image assets, no
 * new task - refreshes itself once/second via its own LVGL timer, same
 * pattern totp_screen.c's clock already uses, reading state that
 * already lives in home_screen.c through the small API screens.h
 * exposes for it.
 *
 * Rows are pre-allocated (fixed NOTIFICATION_HISTORY_MAX of them,
 * hidden/shown as the actual unseen count changes) rather than created/
 * destroyed on the fly - same reasoning as home_screen.c's own
 * notification_slot_t pool: a handful of permanently-resident widgets is
 * cheaper and simpler than churning allocations, and this project's own
 * history already confirmed screens' LVGL widgets live in external
 * PSRAM, not the scarce internal SRAM, so this costs effectively
 * nothing on the resource that actually matters here.
 *
 * Real, hard-learned lesson from this file's first attempt (see
 * NOTIFICATION_HISTORY_MAX's own comment in screens.h): that PSRAM
 * exemption is specifically about LVGL's own widget objects (lv_obj_t,
 * allocated through LVGL's own PSRAM-backed pool) - it does NOT extend
 * to plain C statics this file declares itself, which the compiler puts
 * in ordinary internal-SRAM .bss same as everything else. The first
 * version kept a full char[24] source-name copy per row purely so
 * row_click_cb() could know which source to dismiss - a real, avoidable
 * cost on the one resource that actually matters here. Fixed by not
 * storing that at all: each row's fixed index (0..NOTIFICATION_HISTORY_
 * MAX-1) is encoded directly into its event registration as an integer
 * (via a pointer-sized cast, zero extra storage), and row_click_cb()
 * re-fetches the current list at click time and indexes into it live. */

static lv_obj_t *s_list;
static lv_obj_t *s_empty_label;
static lv_obj_t *s_clear_all_btn;
static lv_obj_t *s_rows[NOTIFICATION_HISTORY_MAX];
static lv_obj_t *s_row_dots[NOTIFICATION_HISTORY_MAX];
static lv_obj_t *s_row_labels[NOTIFICATION_HISTORY_MAX];

static void refresh_rows(void)
{
    notification_unseen_entry_t entries[NOTIFICATION_HISTORY_MAX];
    int count = home_screen_get_unseen_notifications(entries, NOTIFICATION_HISTORY_MAX);

    for (int i = 0; i < NOTIFICATION_HISTORY_MAX; i++) {
        if (i < count) {
            lv_obj_set_style_bg_color(s_row_dots[i], entries[i].color, 0);
            lv_label_set_text(s_row_labels[i], entries[i].message);
            lv_obj_clear_flag(s_rows[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_rows[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (count == 0) {
        lv_obj_clear_flag(s_empty_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_clear_all_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_empty_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_clear_all_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

static void refresh_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    refresh_rows();
}

static void row_click_cb(lv_event_t *event)
{
    int index = (int)(intptr_t)lv_event_get_user_data(event);

    notification_unseen_entry_t entries[NOTIFICATION_HISTORY_MAX];
    int count = home_screen_get_unseen_notifications(entries, NOTIFICATION_HISTORY_MAX);
    if (index < count) {
        home_screen_dismiss_notification(entries[index].source);
    }
    refresh_rows();
}

static void clear_all_click_cb(lv_event_t *event)
{
    (void)event;
    home_screen_dismiss_all_notifications();
    refresh_rows();
}

lv_obj_t *notification_screen_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Notifications");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_26, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    /* Scrollable column, sized to leave room for the title above and the
     * page dots below - up to NOTIFICATION_HISTORY_MAX rows plus the
     * clear-all button can exceed the 240px panel height on its own. */
    s_list = lv_obj_create(scr);
    lv_obj_set_size(s_list, 300, 150);
    lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 52);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_set_style_pad_row(s_list, 4, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(s_list, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < NOTIFICATION_HISTORY_MAX; i++) {
        lv_obj_t *row = lv_obj_create(s_list);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x1A1A1A), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_set_style_pad_hor(row, 10, 0);
        lv_obj_set_style_pad_ver(row, 8, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
        /* Deliberately no LV_OBJ_FLAG_EVENT_BUBBLE - a tap here must only
         * dismiss this row, never also bubble up as a screen-advance tap
         * (screen_tap_cb in main.c is registered on the screen object
         * itself; bubbling is opt-in per child, not automatic, so this
         * is safe by default - same reasoning already relied on by
         * lights_screen.c's button/slider). */
        lv_obj_add_event_cb(row, row_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_rows[i] = row;

        lv_obj_t *dot = lv_obj_create(row);
        lv_obj_set_size(dot, 10, 10);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        s_row_dots[i] = dot;

        lv_obj_t *label = lv_label_create(row);
        lv_obj_set_style_text_color(label, lv_color_white(), 0);
        lv_obj_set_width(label, 240);
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
        s_row_labels[i] = label;
    }

    s_empty_label = lv_label_create(scr);
    lv_label_set_text(s_empty_label, "No notifications");
    lv_obj_set_style_text_color(s_empty_label, UI_TEXT_SECONDARY, 0);
    lv_obj_align(s_empty_label, LV_ALIGN_TOP_MID, 0, 90);

    s_clear_all_btn = lv_obj_create(scr);
    lv_obj_set_size(s_clear_all_btn, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(s_clear_all_btn, 6, 0);
    /* Red, not grey - explicit request 2026-09-17 (matches the red
     * unseen-count badge this button relates to). */
    lv_obj_set_style_bg_color(s_clear_all_btn, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_set_style_bg_opa(s_clear_all_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_clear_all_btn, 0, 0);
    lv_obj_set_style_pad_hor(s_clear_all_btn, 16, 0);
    lv_obj_set_style_pad_ver(s_clear_all_btn, 8, 0);
    lv_obj_clear_flag(s_clear_all_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_clear_all_btn, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_obj_add_event_cb(s_clear_all_btn, clear_all_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *clear_all_label = lv_label_create(s_clear_all_btn);
    lv_label_set_text(clear_all_label, "Clear all");
    lv_obj_set_style_text_color(clear_all_label, lv_color_black(), 0);

    screens_add_page_dots(scr, SCREEN_NOTIFICATIONS);

    refresh_rows();
    lv_timer_create(refresh_timer_cb, 1000, NULL);

    return scr;
}
