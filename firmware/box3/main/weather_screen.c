#include <stdio.h>
#include <string.h>

#include "bsp/esp-box-3.h"
#include "screens.h"
#include "ui_icons/ui_icons.h"
#include "weather_screen.h"

/* Weather page - added 2026-09-24, right after Home (explicit request: keep
 * Home as it was, put the extra detail on its own page). Same retained
 * homeops/weather/state payload the Home screen uses (weather_mqtt.c hands
 * it to both).
 *
 * 2026-09-25 redesign (user: "a lot of info... icons and info too small"):
 * the next-6-hours row was dropped and its space went to bigger everything
 * - user-picked from a to-scale mockup ("Bigger - tonight's real data").
 * Top to bottom on 320x240:
 *   y  8  now: 56px icon (24px asset scaled) + temp (montserrat 44), right
 *         column one fact per line (16px): condition / feels / high-low
 *   y 72  UV + gusts, centered
 *   y 94  radar line: dot + short text (MRMS radar at the office's own
 *         ~1 km cell, see pilab-scripts/wx_nowcast.py)
 *   y127  3 days, stacked: day (+ rain % in blue if >= 20%) / 40px icon /
 *         lo-hi (montserrat 20)
 * No title - the page is self-evident and the space is better used. Text
 * with "·" uses the latin_ext fonts (built-in Montserrat lacks it). */

LV_FONT_DECLARE(lv_font_montserrat_14_latin_ext);
LV_FONT_DECLARE(lv_font_montserrat_16_latin_ext);

#define DAYS 3
#define RAIN_MMHR 0.2   /* same floor as the Weather Poll */
#define POP_SHOW_PCT 20 /* hide low rain chances - less noise */

static lv_obj_t *s_now_icon;
static lv_obj_t *s_now_temp;
static lv_obj_t *s_now_desc;
static lv_obj_t *s_now_feels;
static lv_obj_t *s_now_hilo;
static lv_obj_t *s_uv_gust;
static lv_obj_t *s_radar_dot;
static lv_obj_t *s_radar_label;
static lv_obj_t *s_day_col[DAYS];
static lv_obj_t *s_day_icon[DAYS];
static lv_obj_t *s_day_name[DAYS];
static lv_obj_t *s_day_pop[DAYS];
static lv_obj_t *s_day_temp[DAYS];

static lv_obj_t *plain_box(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, "");
    return l;
}

static lv_obj_t *divider(lv_obj_t *scr, int y)
{
    lv_obj_t *d = lv_obj_create(scr);
    lv_obj_set_size(d, 296, 1);
    lv_obj_set_style_bg_color(d, lv_color_hex(0x2d2d2d), 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(d, 0, 0);
    lv_obj_set_style_radius(d, 0, 0);
    lv_obj_clear_flag(d, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(d, LV_ALIGN_TOP_MID, 0, y);
    return d;
}

static const char *get_str(const cJSON *o, const char *k)
{
    cJSON *i = cJSON_GetObjectItem(o, k);
    return cJSON_IsString(i) ? i->valuestring : NULL;
}

static bool get_num(const cJSON *o, const char *k, double *out)
{
    cJSON *i = cJSON_GetObjectItem(o, k);
    if (!cJSON_IsNumber(i)) {
        return false;
    }
    *out = i->valuedouble;
    return true;
}

lv_obj_t *weather_screen_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    /* Secondary fields (feels, high-low, UV/gusts, day names) use the
     * shared UI_TEXT_SECONDARY (#BDBDBD) - explicit request 2026-09-25,
     * after trying them white (too flat) and grey (too dark). */
    lv_color_t grey = UI_TEXT_SECONDARY;

    /* Now block - same size as Home's weather row (explicit request
     * 2026-09-25): 34px icon (24px asset, scale 363) + montserrat 26. */
    s_now_icon = lv_image_create(scr);
    lv_obj_set_size(s_now_icon, 34, 34);
    lv_image_set_scale(s_now_icon, 363);
    lv_image_set_src(s_now_icon, &wx_sun);
    lv_obj_align(s_now_icon, LV_ALIGN_TOP_LEFT, 12, 13);

    s_now_temp = label(scr, &lv_font_montserrat_26, lv_color_white());
    lv_label_set_text(s_now_temp, "--");
    lv_obj_align(s_now_temp, LV_ALIGN_TOP_LEFT, 52, 8);

    s_now_desc = label(scr, &lv_font_montserrat_16_latin_ext, lv_color_white());
    lv_label_set_text(s_now_desc, "Waiting...");
    /* Layout "D" (user-picked 2026-09-25): Feels sits under the temp on
     * the left; the right column is only condition + H·L - two lines each
     * side, and the widest case (100°F / Feels 112° vs "Mostly Cloudy" /
     * "H 102° · L 81°") still leaves ~68px between them. */
    lv_obj_align(s_now_desc, LV_ALIGN_TOP_RIGHT, -12, 12);
    s_now_feels = label(scr, &lv_font_montserrat_16_latin_ext, grey);
    lv_obj_align(s_now_feels, LV_ALIGN_TOP_LEFT, 53, 38);
    s_now_hilo = label(scr, &lv_font_montserrat_16_latin_ext, grey);
    lv_obj_align(s_now_hilo, LV_ALIGN_TOP_RIGHT, -12, 32);

    s_uv_gust = label(scr, &lv_font_montserrat_16_latin_ext, grey);
    lv_obj_align(s_uv_gust, LV_ALIGN_TOP_MID, 0, 72);

    /* Radar line - dot + text, centered as one group. */
    lv_obj_t *radar = plain_box(scr);
    lv_obj_set_size(radar, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(radar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(radar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(radar, 4, 0);
    lv_obj_align(radar, LV_ALIGN_TOP_MID, 0, 94);
    s_radar_dot = lv_obj_create(radar);
    lv_obj_set_size(s_radar_dot, 9, 9);
    lv_obj_set_style_radius(s_radar_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_radar_dot, 0, 0);
    lv_obj_set_style_bg_color(s_radar_dot, grey, 0);
    lv_obj_clear_flag(s_radar_dot, LV_OBJ_FLAG_CLICKABLE);
    s_radar_label = label(radar, &lv_font_montserrat_16_latin_ext, lv_color_white());

    divider(scr, 120);

    /* 3 days - fixed 100px columns, stacked [day + pop] / icon / lo-hi. */
    lv_obj_t *days = plain_box(scr);
    lv_obj_set_size(days, 300, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(days, LV_FLEX_FLOW_ROW);
    lv_obj_align(days, LV_ALIGN_TOP_MID, 0, 127);
    for (int i = 0; i < DAYS; i++) {
        s_day_col[i] = plain_box(days);
        lv_obj_set_size(s_day_col[i], 100, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(s_day_col[i], LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(s_day_col[i], LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(s_day_col[i], 3, 0);
        lv_obj_add_flag(s_day_col[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_t *top = plain_box(s_day_col[i]);
        lv_obj_set_size(top, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(top, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(top, 6, 0);
        s_day_name[i] = label(top, &lv_font_montserrat_16_latin_ext, grey);
        s_day_pop[i] = label(top, &lv_font_montserrat_14_latin_ext, lv_palette_main(LV_PALETTE_BLUE));
        lv_obj_add_flag(s_day_pop[i], LV_OBJ_FLAG_HIDDEN);
        s_day_icon[i] = lv_image_create(s_day_col[i]);
        lv_obj_set_size(s_day_icon[i], 40, 40);
        lv_image_set_scale(s_day_icon[i], 427); /* 256 * 40/24 */
        lv_image_set_src(s_day_icon[i], &wx_sun);
        s_day_temp[i] = label(s_day_col[i], &lv_font_montserrat_20, lv_color_white());
    }

    screens_add_page_dots(scr, SCREEN_WEATHER);
    return scr;
}

static void set_radar_line(const cJSON *radar)
{
    lv_color_t dot = lv_palette_main(LV_PALETTE_GREY);
    char text[96];
    const char *src = radar ? get_str(radar, "src") : NULL;
    double rate = 0, near_mi = 0, ltg = 0;
    bool have_near = radar && get_num(radar, "near_mi", &near_mi);
    bool have_ltg = radar && get_num(radar, "ltg", &ltg);

    if (!src || strcmp(src, "mrms") != 0 || !get_num(radar, "rate", &rate)) {
        snprintf(text, sizeof(text), "Radar offline \xC2\xB7 model estimate");
    } else if (rate >= RAIN_MMHR) {
        /* At 16px the line fits ~290px: lightning, when likely, replaces
         * the rate ("Heavy rain · Lightning 60%"). */
        const char *what = rate < 2.5 ? "Light rain" : rate <= 7.6 ? "Rain" : "Heavy rain";
        if (have_ltg && ltg >= 30) {
            snprintf(text, sizeof(text), "%s \xC2\xB7 Lightning %.0f%%", what, ltg);
            dot = lv_palette_main(LV_PALETTE_AMBER);
        } else {
            snprintf(text, sizeof(text), "%s here \xC2\xB7 %.1f mm/h", what, rate);
            dot = lv_palette_main(LV_PALETTE_BLUE);
        }
    } else if (have_near) {
        snprintf(text, sizeof(text), "Dry here \xC2\xB7 rain %.1f mi away", near_mi);
        dot = near_mi < 3 ? lv_palette_main(LV_PALETTE_AMBER) : lv_palette_main(LV_PALETTE_GREEN);
    } else {
        snprintf(text, sizeof(text), "Dry here \xC2\xB7 no rain within 10 mi");
        dot = lv_palette_main(LV_PALETTE_GREEN);
    }
    lv_obj_set_style_bg_color(s_radar_dot, dot, 0);
    lv_label_set_text(s_radar_label, text);
}

void weather_screen_update(const cJSON *root)
{
    if (!root || !ui_lock()) {
        return;
    }

    const char *icon = get_str(root, "icon_v2");
    if (!icon) {
        icon = get_str(root, "icon");
    }
    lv_image_set_src(s_now_icon, wx_icon_lookup(icon));

    double v;
    if (get_num(root, "temp_f", &v)) {
        lv_label_set_text_fmt(s_now_temp, "%.0f\xC2\xB0\x46", v);
    }
    const char *desc = get_str(root, "description");
    lv_label_set_text(s_now_desc, desc ? desc : "");

    const cJSON *today = cJSON_GetObjectItem(root, "today");
    double feels = 0, hi = 0, lo = 0, uv = 0, gust = 0;
    if (get_num(root, "feels_like_f", &feels)) {
        lv_label_set_text_fmt(s_now_feels, "Feels %.0f\xC2\xB0", feels);
    } else {
        lv_label_set_text(s_now_feels, "");
    }
    if (today && get_num(today, "hi", &hi) && get_num(today, "lo", &lo)) {
        lv_label_set_text_fmt(s_now_hilo, "H %.0f\xC2\xB0 \xC2\xB7 L %.0f\xC2\xB0", hi, lo);
    } else {
        lv_label_set_text(s_now_hilo, "");
    }
    bool have_uv = today && get_num(today, "uv", &uv);
    bool have_gust = get_num(root, "gust_mph", &gust);
    if (have_uv && have_gust) {
        lv_label_set_text_fmt(s_uv_gust, "UV %.0f \xC2\xB7 Gusts %.0f mph", uv, gust);
    } else if (have_gust) {
        lv_label_set_text_fmt(s_uv_gust, "Gusts %.0f mph", gust);
    } else {
        lv_label_set_text(s_uv_gust, "");
    }

    set_radar_line(cJSON_GetObjectItem(root, "radar"));

    const cJSON *fc = cJSON_GetObjectItem(root, "forecast");
    for (int i = 0; i < DAYS; i++) {
        const cJSON *d = cJSON_IsArray(fc) ? cJSON_GetArrayItem(fc, i) : NULL;
        const char *day = d ? get_str(d, "day") : NULL;
        double dlo = 0, dhi = 0, pop = 0;
        if (!day || !get_num(d, "lo", &dlo) || !get_num(d, "hi", &dhi)) {
            lv_obj_add_flag(s_day_col[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_image_set_src(s_day_icon[i], wx_icon_lookup(get_str(d, "icon")));
        lv_label_set_text(s_day_name[i], day);
        if (get_num(d, "pop", &pop) && pop >= POP_SHOW_PCT) {
            lv_label_set_text_fmt(s_day_pop[i], "%.0f%%", pop);
            lv_obj_clear_flag(s_day_pop[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_day_pop[i], LV_OBJ_FLAG_HIDDEN);
        }
        lv_label_set_text_fmt(s_day_temp[i], "%.0f-%.0f\xC2\xB0", dlo, dhi);
        lv_obj_clear_flag(s_day_col[i], LV_OBJ_FLAG_HIDDEN);
    }

    bsp_display_unlock();
}
