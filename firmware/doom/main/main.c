// Doom on the 2026 Hacker Badge -- I/O path proof.
//
// This does not play Doom yet. It proves the two things the real port stands on:
//   1. a WAD living in flash, read in place through the cache, with its lumps
//      located and its picture format decoded
//   2. a 320x200 8bpp framebuffer converted through Doom's own palette and
//      pushed to the panel fast enough to matter
//
// The framebuffer CRC is printed so the device's decode can be checked against
// a host-side reference decode of the same lump, bit for bit.

#include <stdio.h>
#include <inttypes.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "badge_pins.h"
#include "display.h"
#include "buttons.h"
#include "wad.h"
#include "video.h"

static const char *TAG = "doom";

static void report_memory(const char *when)
{
    ESP_LOGI(TAG, "[%s] free DRAM %u, largest block %u", when,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

static void benchmark_present(void)
{
    const int frames = 30;
    int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < frames; i++) video_present();
    int64_t us = (esp_timer_get_time() - t0) / frames;

    ESP_LOGI(TAG, "video_present: %" PRId64 " us/frame -> %.1f fps",
             us, 1000000.0 / (double)us);
}

static bool show_lump(const char *name)
{
    wad_lump_t l;
    if (!wad_find(name, &l)) {
        ESP_LOGW(TAG, "lump %s not present", name);
        return false;
    }

    const patch_t *p = (const patch_t *)l.data;
    ESP_LOGI(TAG, "%s: %u bytes, %dx%d, offset (%d,%d)",
             name, (unsigned)l.size, p->width, p->height,
             p->leftoffset, p->topoffset);

    video_clear(0);
    wad_draw_patch(p, video_framebuffer(), DOOM_W, DOOM_H, 0, 0);

    ESP_LOGI(TAG, "%s framebuffer FNV1a = 0x%08" PRIx32,
             name, video_fb_hash());
    video_present();
    return true;
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== Doom on the badge: I/O path ===");
    report_memory("boot");

    buttons_init();
    display_init_bus();
    display_reset_and_init(PANEL_CONFIRMED);
    display_fill_rect(0, 0, 320, 240, 0x0000);

    if (!video_init()) return;
    report_memory("after video_init");

    if (!wad_mount()) {
        ESP_LOGE(TAG, "no WAD -- flash one with tools/wad/flash-wad.sh");
        display_fill_rect(0, 0, 320, 240, 0x00F8);   // red: nothing to show
        return;
    }

    wad_lump_t pal;
    if (!wad_find("PLAYPAL", &pal)) {
        ESP_LOGE(TAG, "WAD has no PLAYPAL, cannot build a palette");
        return;
    }
    ESP_LOGI(TAG, "PLAYPAL: %u bytes (%u palettes of 256 colours)",
             (unsigned)pal.size, (unsigned)pal.size / 768);
    video_set_palette(pal.data);

    // Doom's own title screen, decoded from Doom's own picture format, drawn
    // through Doom's own palette. If this looks right, the path is real.
    if (!show_lump("TITLEPIC")) {
        show_lump("HELP1");
    }

    benchmark_present();
    report_memory("steady state");

    ESP_LOGI(TAG, "cycling PLAYPAL's 14 palettes -- press START to stop");
    int pal_index = 0;
    while (1) {
        uint16_t held = buttons_read();
        if (held & (1u << BADGE_BTN_START)) {
            ESP_LOGI(TAG, "stopped on palette %d", pal_index);
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        }
        // The extra palettes are the damage-red and item-pickup tints.
        pal_index = (pal_index + 1) % (pal.size / 768);
        video_set_palette(pal.data + pal_index * 768);
        video_present();
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}
