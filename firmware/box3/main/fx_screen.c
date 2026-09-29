#include <stdio.h>
#include <string.h>

#include "bsp/esp-box-3.h"
#include "freertos/FreeRTOS.h"
#include "screens.h"
#include "ui_icons/ui_icons.h"

/* FX screen - restored 2026-09-24 after being removed in the 2026-09-15
 * RAM-exhaustion fix (that removal turned out unnecessary: screens live in
 * PSRAM - see PROJECT.md). 1 USD in COP/EUR/GBP, each row: flag + code on
 * the left; currency sign + rate, then an up/down arrow with the change
 * vs the previous published rate on the right. Footer names both sources
 * and their dates, since they're two different official references:
 * COP = TRM, EUR/GBP = ECB (see fx_mqtt.h). Flags and the EUR/GBP signs
 * are compiled-in images (tools/icon_src/) - the original screen found
 * the euro glyph drawing as a tofu box on-device, so signs are images,
 * not font characters; COP's "$" is plain ASCII. Arrow direction is just
 * the number's direction (up = more units per dollar). */

typedef struct {
    lv_obj_t *value;
    lv_obj_t *change;
    int decimals;
} fx_row_t;

static fx_row_t s_rows[3];
static lv_obj_t *s_footer;
static char s_trm_date[12];
static char s_ecb_date[12];

/* "3264.39" -> "3,264.39" */
static void format_rate(char *buf, size_t len, double v, int decimals)
{
    char raw[32];
    snprintf(raw, sizeof(raw), "%.*f", decimals, v);
    char *dot = strchr(raw, '.');
    int int_len = dot ? (int)(dot - raw) : (int)strlen(raw);
    size_t o = 0;
    for (int i = 0; raw[i] && o + 1 < len; i++) {
        if (i < int_len && i > 0 && (int_len - i) % 3 == 0 && o + 2 < len) {
            buf[o++] = ',';
        }
        buf[o++] = raw[i];
    }
    buf[o] = '\0';
}

/* "2026-09-24" -> "24 Sep" */
static void format_date(char *buf, size_t len, const char *iso)
{
    static const char *const MONTHS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int y, m, d;
    if (sscanf(iso, "%d-%d-%d", &y, &m, &d) == 3 && m >= 1 && m <= 12) {
        snprintf(buf, len, "%d %s", d, MONTHS[m - 1]);
    } else {
        snprintf(buf, len, "--");
    }
}

/* Latest rates from MQTT, drawn by fx_apply_timer_cb in the LVGL task.
 * fx_screen_set_rate used to draw directly behind bsp_display_lock(100) and
 * silently dropped the update on a timeout - which is what happened to
 * EUR/GBP in the retained-message burst right after a reconnect
 * (2026-09-29: COP drew, EUR/GBP stayed "--"). Since n8n only republishes
 * on change, a dropped row stayed blank until the next ECB rate. Storing
 * here and drawing from an lv_timer (already holds the display lock)
 * can't drop anything. */
typedef struct {
    bool dirty;
    double rate;
    bool has_prev;
    double prev;
    char date[12];
} fx_pending_t;

static fx_pending_t s_pending[3];
static portMUX_TYPE s_pending_lock = portMUX_INITIALIZER_UNLOCKED;

void fx_screen_set_rate(fx_currency_t which, double rate, bool has_prev, double prev, const char *date)
{
    if (which < 0 || which > FX_GBP) {
        return;
    }
    taskENTER_CRITICAL(&s_pending_lock);
    fx_pending_t *p = &s_pending[which];
    p->rate = rate;
    p->has_prev = has_prev;
    p->prev = prev;
    strlcpy(p->date, date ? date : "", sizeof(p->date));
    p->dirty = true;
    taskEXIT_CRITICAL(&s_pending_lock);
}

static void draw_rate(fx_currency_t which, const fx_pending_t *p)
{
    fx_row_t *row = &s_rows[which];
    double rate = p->rate;

    char text[32];
    format_rate(text, sizeof(text), rate, row->decimals);
    lv_label_set_text(row->value, text);

    if (p->has_prev) {
        double diff = rate - p->prev;
        char d[24];
        format_rate(d, sizeof(d), diff < 0 ? -diff : diff, row->decimals);
        /* Compare at display precision so a sub-precision wobble reads as
         * unchanged instead of a misleading "+0.00". */
        char zero[24];
        format_rate(zero, sizeof(zero), 0, row->decimals);
        if (strcmp(d, zero) == 0) {
            lv_label_set_text(row->change, "=");
            lv_obj_set_style_text_color(row->change, UI_TEXT_SECONDARY, 0);
        } else {
            lv_label_set_text_fmt(row->change, "%s %s", diff > 0 ? LV_SYMBOL_UP : LV_SYMBOL_DOWN, d);
            lv_obj_set_style_text_color(row->change,
                                        lv_palette_main(diff > 0 ? LV_PALETTE_GREEN : LV_PALETTE_RED), 0);
        }
    } else {
        lv_label_set_text(row->change, "");
    }

    format_date(which == FX_COP ? s_trm_date : s_ecb_date, sizeof(s_trm_date), p->date);
    lv_label_set_text_fmt(s_footer, "TRM %s   |   ECB %s", s_trm_date[0] ? s_trm_date : "--",
                          s_ecb_date[0] ? s_ecb_date : "--");
}

static void fx_apply_timer_cb(lv_timer_t *t)
{
    (void)t;
    for (int i = FX_COP; i <= FX_GBP; i++) {
        fx_pending_t p;
        taskENTER_CRITICAL(&s_pending_lock);
        p = s_pending[i];
        s_pending[i].dirty = false;
        taskEXIT_CRITICAL(&s_pending_lock);
        if (p.dirty) {
            draw_rate((fx_currency_t)i, &p);
        }
    }
}

static lv_obj_t *plain_row(lv_obj_t *parent, int gap)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_size(o, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_pad_column(o, gap, 0);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(o, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static void add_fx_row(lv_obj_t *parent, fx_currency_t which, const lv_image_dsc_t *flag, const char *code,
                       const lv_image_dsc_t *sign_img, const char *sign_text, int decimals)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_ver(row, 5, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *left = plain_row(row, 8);
    lv_obj_t *flag_img = lv_image_create(left);
    lv_image_set_src(flag_img, flag);
    lv_obj_t *code_label = lv_label_create(left);
    lv_label_set_text(code_label, code);
    lv_obj_set_style_text_color(code_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(code_label, &lv_font_montserrat_20, 0);

    /* Fixed-width columns (sign | value | change) so the three rows line
     * up like a price list instead of each right side being sized to its
     * own content (explicit "align the values" request): sign centered in
     * a 22px cell, value right-aligned in a 100px cell. */
    lv_obj_t *right = plain_row(row, 3);
    lv_obj_t *sign_cell = lv_obj_create(right);
    lv_obj_set_size(sign_cell, 22, 24);
    lv_obj_set_style_bg_opa(sign_cell, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sign_cell, 0, 0);
    lv_obj_set_style_pad_all(sign_cell, 0, 0);
    lv_obj_clear_flag(sign_cell, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(sign_cell, LV_OBJ_FLAG_CLICKABLE);
    if (sign_img) {
        lv_obj_t *sign = lv_image_create(sign_cell);
        lv_image_set_src(sign, sign_img);
        lv_obj_center(sign);
    } else {
        lv_obj_t *sign = lv_label_create(sign_cell);
        lv_label_set_text(sign, sign_text);
        lv_obj_set_style_text_color(sign, lv_color_white(), 0);
        lv_obj_set_style_text_font(sign, &lv_font_montserrat_22, 0);
        lv_obj_center(sign);
    }
    s_rows[which].value = lv_label_create(right);
    lv_obj_set_width(s_rows[which].value, 100);
    lv_obj_set_style_text_align(s_rows[which].value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(s_rows[which].value, "--");
    lv_obj_set_style_text_color(s_rows[which].value, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_rows[which].value, &lv_font_montserrat_22, 0);

    /* Fixed width so the three rates line up regardless of the change
     * text's length. Default font (montserrat 14) carries LV_SYMBOL_UP/
     * DOWN. */
    s_rows[which].change = lv_label_create(right);
    lv_obj_set_width(s_rows[which].change, 74);
    lv_obj_set_style_margin_left(s_rows[which].change, 6, 0);
    lv_obj_set_style_text_font(s_rows[which].change, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_rows[which].change, "");
    s_rows[which].decimals = decimals;
}

lv_obj_t *fx_screen_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "FX");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_26, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    lv_obj_t *subtitle = lv_label_create(scr);
    lv_label_set_text(subtitle, "1 USD");
    lv_obj_set_style_text_color(subtitle, UI_TEXT_SECONDARY, 0);
    lv_obj_align_to(subtitle, title, LV_ALIGN_OUT_BOTTOM_MID, 0, 2);

    lv_obj_t *col = lv_obj_create(scr);
    lv_obj_set_size(col, 300, LV_SIZE_CONTENT);
    lv_obj_align_to(col, subtitle, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_style_pad_all(col, 0, 0);
    lv_obj_set_style_pad_row(col, 2, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_CLICKABLE);

    add_fx_row(col, FX_COP, &flag_cop, "COP", NULL, "$", 2);
    add_fx_row(col, FX_EUR, &flag_eur, "EUR", &icon_euro, NULL, 5);
    add_fx_row(col, FX_GBP, &flag_gbp, "GBP", &icon_gbp, NULL, 5);

    s_footer = lv_label_create(scr);
    lv_label_set_text(s_footer, "Waiting for rates...");
    lv_obj_set_style_text_color(s_footer, UI_TEXT_SECONDARY, 0);
    lv_obj_align(s_footer, LV_ALIGN_BOTTOM_MID, 0, -34);

    screens_add_page_dots(scr, SCREEN_FX);
    lv_timer_create(fx_apply_timer_cb, 500, NULL);

    return scr;
}
