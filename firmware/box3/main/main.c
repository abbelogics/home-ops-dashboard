/*
 * Home Operations Dashboard - ESP32-S3-BOX-3 firmware
 *
 * Default screen is Home (time/temp/humidity/nearest aircraft). Tapping
 * the right half of the panel cycles forward, the left half back:
 * Home -> Systems -> Lights -> FlightAware -> Authenticator ->
 * Claude Usage -> Spotify -> Notifications -> Home (and the reverse
 * going back) - see screen_tap_cb() below, split-screen navigation
 * added 2026-09-17 so overshooting one screen doesn't mean cycling
 * through the entire rest of the loop to get back. The original
 * 2026-09-14 Claude Usage screen (and
 * FX) were removed 2026-09-15 - see screens.h. That removal alone turned
 * out NOT to fix that same day's real internal-RAM bottleneck (screens'
 * LVGL widgets/images live in external PSRAM, not internal SRAM -
 * confirmed via temporary per-init-step heap-checkpoint logging, which
 * showed ~0 bytes of internal RAM cost across all 4 remaining screens).
 * The actual fix was consolidating 4 separate MQTT clients into one
 * (homeops_mqtt_init() below) - see that header for the full story.
 * Claude Usage came back the same day with a genuinely different data
 * source (see claude_usage_mqtt.h) - the original never worked at all,
 * this one does. Aircraft state comes from the n8n Aircraft Poll
 * workflow via MQTT (retained topic homeops/aircraft/state) and updates
 * the Home screen directly - there's no separate aircraft screen.
 */

/* Direct BOX-3 package include (not the generic "bsp/esp-bsp.h") - see
 * CMakeLists.txt at the project root for why we don't go through the
 * shared multi-board "bsp" wrapper. */
#include "bsp/esp-box-3.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "homeops_mqtt.h"
#include "lights_voice.h"
#include "notification_sound.h"
#include "screens.h"
#include "sensor_accessory.h"
#include "webhook_post.h"
#include "wifi_time.h"

static const char *TAG = "box3_dashboard";

static lv_obj_t *s_screens[SCREEN_COUNT];
static int s_current_screen = SCREEN_HOME;

/* Split-screen tap navigation - explicit request 2026-09-17: a plain
 * single-direction tap cycle meant overshooting one screen had no way
 * back except cycling through the entire rest of the loop. Left half of
 * the panel goes back, right half goes forward - reads the actual touch
 * point off the active input device (same one this event already fired
 * from) rather than adding a second event type to listen for. Screens
 * with their own genuinely clickable children (Lights' button/slider,
 * Notification's rows/clear-all) are unaffected either way - those
 * deliberately don't set LV_OBJ_FLAG_EVENT_BUBBLE, so a tap on them
 * never reaches this handler (registered on the screen object itself)
 * at all, same as before this change. */
static void screen_tap_cb(lv_event_t *event)
{
    lv_indev_t *indev = lv_indev_get_act();
    lv_point_t point = {0};
    if (indev) {
        lv_indev_get_point(indev, &point);
    }
    bool go_back = point.x < (BSP_LCD_H_RES / 2);

    int next = go_back ? (s_current_screen - 1 + SCREEN_COUNT) % SCREEN_COUNT : (s_current_screen + 1) % SCREEN_COUNT;

    if (bsp_display_lock(0)) {
        lv_scr_load_anim(s_screens[next], go_back ? LV_SCR_LOAD_ANIM_MOVE_RIGHT : LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0,
                          false);
        bsp_display_unlock();
    }

    s_current_screen = next;
}

void screens_add_page_dots(lv_obj_t *screen, int active_index)
{
    lv_obj_t *row = lv_obj_create(screen);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_gap(row, 6, 0);
    lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    /* Let taps on the dot row fall through to the screen's own tap handler. */
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < SCREEN_COUNT; i++) {
        lv_obj_t *dot = lv_obj_create(row);
        lv_obj_set_size(dot, 8, 8);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(dot,
                                   (i == active_index) ? lv_palette_main(LV_PALETTE_BLUE)
                                                        : lv_palette_main(LV_PALETTE_GREY),
                                   0);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting home-ops dashboard firmware");

    bsp_i2c_init();

    /* 2026-09-15: lights_voice_init() moved here, first, ahead of
     * wifi_time_init() and all 4 MQTT client inits - this is the actual
     * fix for the same-day wake-word-unresponsive regression (see the
     * still-valid core-pinning comment on bsp_display_start_with_config
     * below for a real but incomplete first attempt at this). Root cause,
     * confirmed via a temporary explicit xTaskCreatePinnedToCore return-
     * value check in lights_voice.c: detect_task's creation (8KB stack)
     * was failing outright with only ~14.5KB internal RAM free at the
     * point it used to run - after WiFi init and 4 MQTT clients had
     * already claimed their share, on top of AFE/MultiNet's own large
     * internal working buffers. feed_task (smaller, 4KB stack) still
     * succeeded, which is why only detect_task silently vanished - engine
     * never running means nothing ever calls fetch() to drain AFE's
     * output ring buffer, which is the actual cause of every "rb_out
     * slow" warning seen, independent of core placement. Exactly the
     * same failure mode the comment below (on the aircraft_alert lazy-
     * init decision) already called out for a different eager init -
     * same lesson, just missed here until now. Running voice init first,
     * before anything else claims internal RAM, gives detect_task's
     * allocation first pick of a still-mostly-empty heap. */
    lights_voice_init();

    /* Reuses the 16kHz I2S bus lights_voice_init() just brought up - see
     * notification_sound.c's own comment. Must run after it, not
     * before. */
    notification_sound_init();

    /* Explicit config (not the plain bsp_display_start()) to pin LVGL's
     * own rendering task to core 0 - 2026-09-15, a real but incomplete
     * first attempt at fixing the wake-word regression above (the actual
     * cause turned out to be detect_task's creation failing outright, not
     * scheduling contention - see the lights_voice_init() comment). Kept
     * anyway since it's still a correct, low-risk improvement: it keeps
     * LVGL off core 1, which is otherwise dedicated to detect_task and
     * AFE's own internal worker (both priority 5). Rest of this cfg
     * copied from bsp_display_start()'s own default (esp-box-3.c) - only
     * task_affinity changes. See sdkconfig.defaults'
     * CONFIG_MQTT_TASK_CORE_SELECTION for the equivalent fix applied to
     * the 4 MQTT clients. */
    bsp_display_cfg_t display_cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_H_RES * CONFIG_BSP_LCD_DRAW_BUF_HEIGHT,
#if CONFIG_BSP_LCD_DRAW_BUF_DOUBLE
        .double_buffer = 1,
#else
        .double_buffer = 0,
#endif
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
        },
    };
    display_cfg.lvgl_port_cfg.task_affinity = 0;
    bsp_display_start_with_config(&display_cfg);
    /* 45 -> 35 (2026-09-14, explicit request, "reduce the brightness a
     * little bit") - full 100% was harsh for an always-on ambient display
     * to begin with. See DISPLAY_BRIGHTNESS_PCT's own comment (screens.h)
     * for why home_screen.c's wake-from-sleep path also needs this same
     * value, not just boot. */
    bsp_display_brightness_set(DISPLAY_BRIGHTNESS_PCT);

    /* Dark background: less glare/harsh white glow for an always-on ambient
     * display sitting in the room, easier to read at night. Briefly tried
     * light (2026-09-11) but reverted the same day - dark is the keeper. */
    lv_theme_default_init(lv_display_get_default(), lv_palette_main(LV_PALETTE_BLUE),
                           lv_palette_main(LV_PALETTE_GREY), true, &lv_font_montserrat_16);
    bsp_spiffs_mount(); /* airline logo PNGs */
    sensor_accessory_init();
    wifi_time_init();
    /* webhook_post_init() first (cheap - just its worker queue/task) so
     * its queue exists before either lights_mqtt.c's or spotify_mqtt.c's
     * own control calls could possibly enqueue onto it. One shared task
     * for both (see webhook_post.h) - each used to have its own, until a
     * second dedicated task turned out to be a real task-watchdog crash
     * at boot, caught live 2026-09-16. */
    webhook_post_init();
    /* Single MQTT client for aircraft/weather/systems/lights state topics
     * - see homeops_mqtt.h for why (2026-09-15: consolidated from 4
     * separate clients that were collectively eating ~38KB of internal
     * RAM for mostly-duplicate connection/task overhead). */
    homeops_mqtt_init();
    /* lights_voice_init() itself now runs first thing in this function,
     * above - see that call site's comment. Not called here anymore. */
    /* No aircraft_alert_init() call - that module's worker task/speaker
     * are both created lazily, on the first actual alert, not at boot
     * (see aircraft_alert.h). An earlier eager version of this call sat
     * right here and broke voice control by stealing internal-RAM
     * headroom AFE's ring buffers needed at creation time - worth a note
     * for whoever's tempted to add an eager init call for a new subsystem
     * in this exact spot again. */

    bsp_display_lock(0);

    s_screens[SCREEN_HOME] = home_screen_create();
    s_screens[SCREEN_WEATHER] = weather_screen_create();
    s_screens[SCREEN_SYSTEMS] = systems_screen_create();
    s_screens[SCREEN_LIGHTS] = lights_screen_create();
    s_screens[SCREEN_FLIGHTAWARE] = flightaware_screen_create();
    s_screens[SCREEN_TOTP] = totp_screen_create();
    s_screens[SCREEN_CLAUDE_USAGE] = claude_usage_screen_create();
    s_screens[SCREEN_CODEX_USAGE] = codex_usage_screen_create();
    s_screens[SCREEN_FX] = fx_screen_create();
    s_screens[SCREEN_SPOTIFY] = spotify_screen_create();
    s_screens[SCREEN_NOTIFICATIONS] = notification_screen_create();

    for (int i = 0; i < SCREEN_COUNT; i++) {
        lv_obj_add_event_cb(s_screens[i], screen_tap_cb, LV_EVENT_CLICKED, NULL);
    }

    lv_scr_load(s_screens[s_current_screen]);

    bsp_display_unlock();

    /* Permanent boot-health signal, not a temp diagnostic - internal RAM
     * on this device is genuinely scarce (2026-09-15's whole MQTT-
     * consolidation fix was about this exact number bottoming out at
     * ~2.5KB before, ~13KB after). One line at the end of boot, cheap to
     * keep, makes any future regression back toward that wall visible in
     * the serial log immediately instead of needing another investigation
     * like this one. */
    ESP_LOGI(TAG, "Boot complete - internal free heap=%u bytes", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}
