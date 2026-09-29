#include <stdio.h>

#include "bsp/esp-box-3.h"
#include "screens.h"

/* Codex (OpenAI) usage - same "big percentage + pill badge + bar +
 * reset line" layout as claude_usage_screen.c, deliberately mirrored
 * rather than redesigned (see codex_usage_mqtt.h for the data source).
 * Text-only title, no logo image - same fallback claude_usage_screen.c
 * itself used for one round before a working logo crop was found; not
 * worth chasing a new icon asset for this first pass. */
#define BADGE_BG_COLOR    lv_color_hex(0x6E63B0)
#define BAR_FILL_COLOR    lv_palette_main(LV_PALETTE_GREEN)
#define BAR_TRACK_COLOR   lv_palette_main(LV_PALETTE_GREY)
#define BAR_BORDER_COLOR  0xD6D0F7

static lv_obj_t *s_5h_pct_label;
static lv_obj_t *s_5h_bar;
static lv_obj_t *s_5h_reset_label;
static lv_obj_t *s_weekly_pct_label;
static lv_obj_t *s_weekly_bar;
static lv_obj_t *s_weekly_reset_label;
static lv_obj_t *s_status_label;
static lv_obj_t *s_content_col;

/* Identical to claude_usage_screen.c's format_reset() - minutes-from-now,
 * not a wall-clock time, same reasoning. */
static void format_reset(int minutes, char *buf, size_t buf_len)
{
    if (minutes <= 0) {
        snprintf(buf, buf_len, "Resets soon");
    } else if (minutes < 24 * 60) {
        snprintf(buf, buf_len, "Resets in %dh %dm", minutes / 60, minutes % 60);
    } else {
        snprintf(buf, buf_len, "Resets in %dd %dh", minutes / (24 * 60), (minutes % (24 * 60)) / 60);
    }
}

void codex_usage_screen_set_usage(int pct_5h, int reset_min_5h, int pct_weekly, int reset_min_weekly)
{
    if (!ui_lock()) {
        return;
    }

    lv_obj_add_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_content_col, LV_OBJ_FLAG_HIDDEN);

    char buf[24];

    lv_label_set_text_fmt(s_5h_pct_label, "%d%%", pct_5h);
    lv_bar_set_value(s_5h_bar, pct_5h, LV_ANIM_OFF);
    format_reset(reset_min_5h, buf, sizeof(buf));
    lv_label_set_text(s_5h_reset_label, buf);

    lv_label_set_text_fmt(s_weekly_pct_label, "%d%%", pct_weekly);
    lv_bar_set_value(s_weekly_bar, pct_weekly, LV_ANIM_OFF);
    format_reset(reset_min_weekly, buf, sizeof(buf));
    lv_label_set_text(s_weekly_reset_label, buf);

    bsp_display_unlock();
}

/* Identical structure to claude_usage_screen.c's create_usage_block(). */
static void create_usage_block(lv_obj_t *parent, const char *badge_text, lv_obj_t **pct_label, lv_obj_t **bar,
                                lv_obj_t **reset_label)
{
    lv_obj_t *block = lv_obj_create(parent);
    lv_obj_set_width(block, lv_pct(100));
    lv_obj_set_height(block, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(block, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(block, 0, 0);
    lv_obj_set_style_pad_row(block, 3, 0);
    lv_obj_set_style_bg_opa(block, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(block, 0, 0);
    lv_obj_clear_flag(block, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *top_row = lv_obj_create(block);
    lv_obj_set_width(top_row, lv_pct(100));
    lv_obj_set_height(top_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(top_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(top_row, 0, 0);
    lv_obj_set_style_bg_opa(top_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(top_row, 0, 0);
    lv_obj_clear_flag(top_row, LV_OBJ_FLAG_SCROLLABLE);

    *pct_label = lv_label_create(top_row);
    lv_label_set_text(*pct_label, "--");
    lv_obj_set_style_text_font(*pct_label, &lv_font_montserrat_26, 0);
    lv_obj_set_style_text_color(*pct_label, lv_color_white(), 0);

    lv_obj_t *badge = lv_obj_create(top_row);
    lv_obj_set_size(badge, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(badge, BADGE_BG_COLOR, 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(badge, 0, 0);
    lv_obj_set_style_pad_hor(badge, 12, 0);
    lv_obj_set_style_pad_ver(badge, 4, 0);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *badge_label = lv_label_create(badge);
    lv_label_set_text(badge_label, badge_text);
    lv_obj_set_style_text_color(badge_label, lv_color_white(), 0);

    *bar = lv_bar_create(block);
    lv_obj_set_size(*bar, lv_pct(100), 10);
    lv_bar_set_range(*bar, 0, 100);
    lv_bar_set_value(*bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(*bar, BAR_TRACK_COLOR, 0);
    lv_obj_set_style_bg_opa(*bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(*bar, 2, 0);
    lv_obj_set_style_border_color(*bar, lv_color_hex(BAR_BORDER_COLOR), 0);
    lv_obj_set_style_radius(*bar, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_radius(*bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(*bar, BAR_FILL_COLOR, LV_PART_INDICATOR);

    *reset_label = lv_label_create(block);
    lv_label_set_text(*reset_label, "");
    lv_obj_set_style_text_color(*reset_label, lv_color_white(), 0);
}

lv_obj_t *codex_usage_screen_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_pad_all(scr, 14, 0);

    s_content_col = lv_obj_create(scr);
    lv_obj_set_width(s_content_col, lv_pct(100));
    lv_obj_set_height(s_content_col, LV_SIZE_CONTENT);
    lv_obj_align(s_content_col, LV_ALIGN_TOP_MID, 0, -2);
    lv_obj_set_flex_flow(s_content_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_content_col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(s_content_col, 0, 0);
    lv_obj_set_style_pad_row(s_content_col, 8, 0);
    lv_obj_set_style_bg_opa(s_content_col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_content_col, 0, 0);
    lv_obj_clear_flag(s_content_col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_content_col, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *title = lv_label_create(s_content_col);
    /* On-screen title says "OpenAI" - explicit request 2026-09-22, "it is
     * actually OpenAI" - the underlying account/product this data comes
     * from is genuinely OpenAI's Codex/Agentic quota (see
     * codex_usage_mqtt.h), but "OpenAI" is the name that actually reads
     * as recognizable here. Internal file/topic/function names stay
     * "codex_usage" - purely internal plumbing, renaming those bought
     * nothing user-visible and only added risk across several wired-
     * together files for a cosmetic label. */
    lv_label_set_text(title, "OpenAI Usage");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_26, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_margin_top(title, 2, 0);
    lv_obj_set_style_margin_bottom(title, 14, 0);

    create_usage_block(s_content_col, "5h", &s_5h_pct_label, &s_5h_bar, &s_5h_reset_label);
    create_usage_block(s_content_col, "Weekly", &s_weekly_pct_label, &s_weekly_bar, &s_weekly_reset_label);

    s_status_label = lv_label_create(scr);
    lv_label_set_text(s_status_label, "Waiting for usage data...");
    lv_obj_set_style_text_color(s_status_label, UI_TEXT_SECONDARY, 0);
    lv_obj_align(s_status_label, LV_ALIGN_CENTER, 0, -22);

    screens_add_page_dots(scr, SCREEN_CODEX_USAGE);

    return scr;
}
