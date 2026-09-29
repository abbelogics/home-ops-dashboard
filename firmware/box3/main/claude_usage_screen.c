#include <stdio.h>

#include "bsp/esp-box-3.h"
#include "screens.h"
#include "ui_icons/ui_icons.h"

/* Claude Pro/Max usage (session/weekly %) - see claude_usage_mqtt.h for
 * where the data actually comes from (a real Anthropic API rate-limit
 * header, not claude.ai's private page - see that header's own comment
 * for the full story). Redesigned 2026-09-15, explicit request/reference
 * photo - a small physical device running the open-source Clawdmeter
 * project (github.com/HermannBjorgvin/Clawdmeter, the same project this
 * data-fetching technique itself is based on): big percentage + a
 * rounded "Current"/"Weekly" pill badge on one row, bar below, "Resets
 * in..." below that. Header is a small logo (S:/spiffs/claude_logo.png)
 * to the left of "Claude Usage" text, same day. Two earlier crops of
 * the real logo (a hand-drawn 3-spoke mark, then the bare white asterisk
 * mark alone) were unreadably faint at this project's forced ~24x24
 * native PNG size (see home_screen.c's weather-icon comment for why
 * that limit exists) - text-only title was used as a fallback for one
 * round, then this crop (the full rounded-square app icon, solid orange
 * background behind the white mark) replaced claude_logo.png and reads
 * fine at 24x24 because the orange/white contrast survives the downscale
 * where the mark-on-transparent crops didn't. Layout centers the whole
 * content block as one unit (s_content_col) rather than positioning each
 * row by a hand-computed pixel offset - the offset approach left content
 * touching the page dots at the bottom (explicit complaint, same day)
 * once the header's own height changed; centering is self-correcting
 * regardless of exact content height. Static layout (no motion), updated
 * whenever a new MQTT message arrives (every ~2 min from the poller on
 * `sky`). */
/* Bar colors match the Lights screen's ON/OFF button colors exactly
 * (lv_palette_main(), not a hardcoded hex guess) - explicit "keep
 * uniformity" request 2026-09-15: bar track uses the OFF button's grey,
 * bar fill uses the ON button's green. This replaces the earlier
 * severity-scaled fill (lime/orange/red by %) - a deliberate tradeoff
 * of that signal for cross-screen consistency, per the same request.
 * Badge pill color, however, reverted back to its original purple
 * (0x6E63B0) - "no change the pills for current and weekly to the
 * previous color", same day - the uniformity request applies to the
 * bars only, not the badges. */
#define BADGE_BG_COLOR    lv_color_hex(0x6E63B0)
#define BAR_FILL_COLOR    lv_palette_main(LV_PALETTE_GREEN)
/* Bar track (unfilled portion). Explicit border (see create_usage_block())
 * keeps the bar's full extent (where 100% actually is) visible regardless
 * of fill %. */
#define BAR_TRACK_COLOR   lv_palette_main(LV_PALETTE_GREY)
#define BAR_BORDER_COLOR  0xD6D0F7

static lv_obj_t *s_session_pct_label;
static lv_obj_t *s_session_bar;
static lv_obj_t *s_session_reset_label;
static lv_obj_t *s_week_pct_label;
static lv_obj_t *s_week_bar;
static lv_obj_t *s_week_reset_label;
static lv_obj_t *s_status_label;
static lv_obj_t *s_content_col;

/* "4h 53m" under a day, "5d 1h" at or beyond - minutes-from-now, not a
 * wall-clock time, so it stays correct regardless of exactly when the
 * device's own clock last synced relative to the poller's. */
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

void claude_usage_screen_set_usage(int session_pct, int session_reset_min, int week_pct, int week_reset_min)
{
    if (!ui_lock()) {
        return;
    }

    lv_obj_add_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_content_col, LV_OBJ_FLAG_HIDDEN);

    char buf[24];

    lv_label_set_text_fmt(s_session_pct_label, "%d%%", session_pct);
    lv_bar_set_value(s_session_bar, session_pct, LV_ANIM_OFF);
    format_reset(session_reset_min, buf, sizeof(buf));
    lv_label_set_text(s_session_reset_label, buf);

    lv_label_set_text_fmt(s_week_pct_label, "%d%%", week_pct);
    lv_bar_set_value(s_week_bar, week_pct, LV_ANIM_OFF);
    format_reset(week_reset_min, buf, sizeof(buf));
    lv_label_set_text(s_week_reset_label, buf);

    bsp_display_unlock();
}

/* One metric block: "<pct>%" + a rounded pill badge on the same row,
 * a bar below, a white "Resets in..." line below that. */
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
    /* Track + a real border, not just a fill color, so the bar's full
     * extent (where 100% is) stays visible even when the fill is small -
     * see BAR_TRACK_COLOR/BAR_BORDER_COLOR's own comment above for why. */
    lv_obj_set_style_bg_color(*bar, BAR_TRACK_COLOR, 0);
    lv_obj_set_style_bg_opa(*bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(*bar, 2, 0);
    lv_obj_set_style_border_color(*bar, lv_color_hex(BAR_BORDER_COLOR), 0);
    lv_obj_set_style_radius(*bar, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_radius(*bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(*bar, BAR_FILL_COLOR, LV_PART_INDICATOR);

    /* White, not grey - explicit complaint 2026-09-15, "I can barely
     * read what it says" (grey-on-black was too low-contrast at this
     * font size). */
    *reset_label = lv_label_create(block);
    lv_label_set_text(*reset_label, "");
    lv_obj_set_style_text_color(*reset_label, lv_color_white(), 0);
}

lv_obj_t *claude_usage_screen_create(void)
{
    /* No flex_flow on the screen itself - screens_add_page_dots() below
     * positions its row with a plain lv_obj_align(BOTTOM_MID), which a
     * flex-managed parent would fight with/override. All the real
     * content instead lives in s_content_col, a separate flex-column
     * child sized to its own content and centered as a whole - this is
     * what actually fixes the bottom-crowding bug the previous fixed-
     * pixel-offset version had (explicit complaint 2026-09-15,
     * "everything seems justified to the bottom, touching the dots") -
     * centering the whole block is self-correcting regardless of its
     * exact height, unlike hand-computed top offsets. */
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_pad_all(scr, 14, 0);

    s_content_col = lv_obj_create(scr);
    lv_obj_set_width(s_content_col, lv_pct(100));
    lv_obj_set_height(s_content_col, LV_SIZE_CONTENT);
    /* Top-anchored, not centered - explicit request 2026-09-15 to match
     * the Authenticator (TOTP) screen's title level. totp_screen.c aligns
     * its title LV_ALIGN_TOP_MID 0,12 on an unpadded screen; scr here has
     * 14px padding already (see lv_obj_set_style_pad_all above), so -2
     * lands the header row at the same 12px-from-the-physical-top level. */
    lv_obj_align(s_content_col, LV_ALIGN_TOP_MID, 0, -2);
    lv_obj_set_flex_flow(s_content_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_content_col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(s_content_col, 0, 0);
    lv_obj_set_style_pad_row(s_content_col, 8, 0);
    lv_obj_set_style_bg_opa(s_content_col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_content_col, 0, 0);
    lv_obj_clear_flag(s_content_col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_content_col, LV_OBJ_FLAG_HIDDEN);

    /* Header row: logo to the left of "Claude Usage" text, both
     * vertically centered on each other. Logo stays at its native 24x24
     * (this project's PNG decoder silently fails above that - see the
     * top-of-file comment) and is scaled up slightly (256 -> 300, ~17%)
     * to roughly match the title text's rendered height. */
    lv_obj_t *header_row = lv_obj_create(s_content_col);
    lv_obj_set_size(header_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(header_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(header_row, 0, 0);
    lv_obj_set_style_pad_column(header_row, 6, 0);
    lv_obj_set_style_bg_opa(header_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header_row, 0, 0);
    lv_obj_clear_flag(header_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *logo = lv_image_create(header_row);
    lv_image_set_src(logo, &claude_logo);
    lv_image_set_scale(logo, 300);
    /* Extra gap below just this row (not the general 8px row gap used
     * elsewhere in s_content_col) - explicit "push the first percentage
     * down a little" request 2026-09-15, once the whole block moved up
     * and the session block ended up sitting too close under the
     * header. */
    lv_obj_set_style_margin_top(header_row, 2, 0);
    lv_obj_set_style_margin_bottom(header_row, 14, 0);

    lv_obj_t *title = lv_label_create(header_row);
    lv_label_set_text(title, "Claude Usage");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_26, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);

    create_usage_block(s_content_col, "Current", &s_session_pct_label, &s_session_bar, &s_session_reset_label);
    create_usage_block(s_content_col, "Weekly", &s_week_pct_label, &s_week_bar, &s_week_reset_label);

    /* Shown instead of s_content_col until the first MQTT message
     * arrives (poller on `sky` publishes every ~2 min - see
     * claude_usage_mqtt.h). */
    s_status_label = lv_label_create(scr);
    lv_label_set_text(s_status_label, "Waiting for usage data...");
    lv_obj_set_style_text_color(s_status_label, UI_TEXT_SECONDARY, 0);
    lv_obj_align(s_status_label, LV_ALIGN_CENTER, 0, -22);

    screens_add_page_dots(scr, SCREEN_CLAUDE_USAGE);

    return scr;
}
