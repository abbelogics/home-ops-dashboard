#include <string.h>

#include "bsp/esp-box-3.h"
#include "screens.h"

typedef struct {
    const char *key;
    lv_obj_t *dot;
    lv_obj_t *value;
} status_row_t;

#define STATUS_ROW_COUNT 4
static status_row_t s_rows[STATUS_ROW_COUNT];

/* Last octet only (e.g. "247" from "pilab.local") - explicit request
 * 2026-09-14, the full address wasn't needed at a glance and ate into the
 * room a temp reading needs on the same line. Falls back to the full
 * string if there's no '.' in it (shouldn't happen for a real IPv4
 * address, but cheaper than crashing on a malformed one). */
static const char *last_ip_octet(const char *ip)
{
    const char *last_dot = strrchr(ip, '.');
    return last_dot ? last_dot + 1 : ip;
}

static status_row_t *find_row(const char *key)
{
    for (int i = 0; i < STATUS_ROW_COUNT; i++) {
        if (s_rows[i].key && strcmp(s_rows[i].key, key) == 0) {
            return &s_rows[i];
        }
    }
    return NULL;
}

static void add_status_row(lv_obj_t *parent, const char *key, const char *name)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(90), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_ver(row, 8, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *dot = lv_obj_create(row);
    lv_obj_set_size(dot, 14, 14);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_set_style_bg_color(dot, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, name);
    lv_obj_set_flex_grow(label, 1);
    lv_obj_set_style_pad_left(label, 10, 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_18, 0);

    /* Same font+color as the row's own device-name label (the "system
     * type", e.g. "PiLab") - explicit request 2026-09-14, both bumped
     * 16->18 and value's color white now (matching name's default white,
     * was grey). An earlier attempt matched this to the screen's own
     * much-larger "Systems" title instead (montserrat_26), which broke
     * the row layout badly with a long "<ip>  <temp>" string - corrected
     * same day. LVGL still wraps to a second line by default if a value
     * doesn't fit this column's width, rather than clipping or
     * overflowing. */
    lv_obj_t *value = lv_label_create(row);
    lv_obj_set_style_text_font(value, &lv_font_montserrat_18, 0);
    lv_obj_set_width(value, 130);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(value, "--");
    lv_obj_set_style_text_color(value, lv_color_white(), 0);

    for (int i = 0; i < STATUS_ROW_COUNT; i++) {
        if (s_rows[i].key == NULL) {
            s_rows[i] = (status_row_t){.key = key, .dot = dot, .value = value};
            break;
        }
    }
}

void systems_screen_set_status(const char *key, const system_status_t *status)
{
    if (!bsp_display_lock(100)) {
        return;
    }

    status_row_t *row = find_row(key);
    if (row) {
        lv_obj_set_style_bg_color(row->dot,
                                   status->online ? lv_palette_main(LV_PALETTE_GREEN)
                                                   : lv_palette_main(LV_PALETTE_RED),
                                   0);

        if (!status->online) {
            lv_obj_set_style_text_color(row->value, lv_palette_main(LV_PALETTE_RED), 0);
            lv_label_set_text(row->value, "Offline");
        } else if (status->has_temp) {
            /* Temp only now, no IP at all - explicit request 2026-09-14,
             * replacing the same day's earlier "temp next to the IP"
             * version of this row ("that looks really bad"). White now
             * (was grey), matching the device-name label - a later
             * same-day request. */
            lv_obj_set_style_text_color(row->value, lv_color_white(), 0);
            lv_label_set_text_fmt(row->value, "%.1f\xC2\xB0\x43", status->temp_c);
        } else if (status->ip) {
            lv_obj_set_style_text_color(row->value, lv_color_white(), 0);
            lv_label_set_text(row->value, last_ip_octet(status->ip));
        } else {
            lv_obj_set_style_text_color(row->value, lv_color_white(), 0);
            lv_label_set_text(row->value, "Online");
        }
    }

    bsp_display_unlock();
}

lv_obj_t *systems_screen_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Systems");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_26, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    lv_obj_t *list = lv_obj_create(scr);
    lv_obj_set_size(list, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_align(list, LV_ALIGN_CENTER, 0, 10);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_CLICKABLE);

    add_status_row(list, "nas", "NAS");
    add_status_row(list, "pilab", "PiLab");
    add_status_row(list, "abbe", "Abbe");
    add_status_row(list, "sky", "Sky");

    screens_add_page_dots(scr, SCREEN_SYSTEMS);

    return scr;
}
