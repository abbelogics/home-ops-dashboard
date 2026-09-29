#include <stdio.h>

#include "bsp/esp-box-3.h"
#include "screens.h"

/* FlightAware AeroAPI usage - real numbers from FlightAware's own free
 * GET /account/usage endpoint (see n8n/rebuild_mia_fids_poll_workflow.py),
 * not this firmware's own estimate. Originally a row on the Systems
 * screen; moved to its own screen 2026-09-13, explicit request - the
 * combined "$X.XX (N)" text didn't fit that row's value column width. */
static lv_obj_t *s_cost_label;
static lv_obj_t *s_calls_label;

void flightaware_screen_set_usage(int calls, double cost)
{
    if (!ui_lock()) {
        return;
    }

    lv_label_set_text_fmt(s_cost_label, "$%.2f", cost);
    lv_label_set_text_fmt(s_calls_label, "%d calls this month", calls);

    bsp_display_unlock();
}

lv_obj_t *flightaware_screen_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "FlightAware");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_26, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    s_cost_label = lv_label_create(scr);
    lv_label_set_text(s_cost_label, "--");
    lv_obj_set_style_text_font(s_cost_label, &lv_font_montserrat_26, 0);
    lv_obj_align(s_cost_label, LV_ALIGN_CENTER, 0, -10);

    s_calls_label = lv_label_create(scr);
    lv_label_set_text(s_calls_label, "-- calls this month");
    lv_obj_set_style_text_color(s_calls_label, UI_TEXT_SECONDARY, 0);
    lv_obj_align(s_calls_label, LV_ALIGN_CENTER, 0, 24);

    screens_add_page_dots(scr, SCREEN_FLIGHTAWARE);

    return scr;
}
