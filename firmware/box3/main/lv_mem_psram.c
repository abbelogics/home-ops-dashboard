/* LVGL allocator -> PSRAM (2026-09-24).
 *
 * Replaces LVGL's built-in allocator, whose fixed pool
 * (CONFIG_LV_MEM_SIZE_KILOBYTES, a static .bss array) sat in the scarce
 * internal SRAM. Every widget on every screen lives in that pool, so
 * restoring the FX screen (a 10th screen, plus a 10th page dot on every
 * screen) exhausted the 64KB pool - LVGL's default assert handler is
 * while(1), so boot froze and MQTT never started. Growing the pool to 72KB
 * fixed that but took the 8KB from the same internal SRAM voice control
 * needs - "Lights On/Off" stopped working (both confirmed live). Putting
 * LVGL's heap in the 16MB PSRAM instead frees the whole pool back to
 * internal SRAM and removes the ceiling. The display DMA draw buffers are
 * separate (bsp display config, internal) and unaffected.
 *
 * Selected via CONFIG_LV_USE_CUSTOM_MALLOC=y (sdkconfig). */
#include "lvgl.h"

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM

#include <string.h>

#include "esp_heap_caps.h"

void lv_mem_init(void)
{
}

void lv_mem_deinit(void)
{
}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes)
{
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
    LV_UNUSED(pool);
}

void *lv_malloc_core(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void *lv_realloc_core(void *p, size_t new_size)
{
    return heap_caps_realloc(p, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void lv_free_core(void *p)
{
    heap_caps_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p)
{
    memset(mon_p, 0, sizeof(*mon_p));
    mon_p->total_size = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    mon_p->free_size = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    mon_p->free_biggest_size = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
}

lv_result_t lv_mem_test_core(void)
{
    return LV_RESULT_OK;
}

#endif
