// Doom on the 2026 Hacker Badge.
//
// The platform layer lives in the doom component (badge_platform.c): the WAD is
// mapped from flash and never read into RAM, Doom's 8bpp buffer is the same one
// the SPI driver scans out, and the 74HC165 feeds DG_GetKey.

#include <stdio.h>
#include <inttypes.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"

#include "badge_pins.h"
#include "display.h"
#include "buttons.h"
#include "video.h"
#include "badge_radio.h"

static const char *TAG = "doom";

// Provided by the engine.
extern int myargc;
extern char **myargv;
void D_DoomMain(void);
void I_ReportLastCrash(void);
void DG_Init(void);
void doomgeneric_Tick(void);

static char *s_argv[] = { "doom", NULL };

static void report_memory(const char *when)
{
    ESP_LOGI(TAG, "[%s] free DRAM %u, largest block %u", when,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

void app_main(void)
{
    // Unbuffered: a hang or a hard reset does not flush stdout the way abort()
    // does, so a buffered I_Error message dies with the process and the crash
    // looks like it had no cause at all. This has cost two debugging rounds.
    setvbuf(stdout, NULL, _IONBF, 0);

    ESP_LOGI(TAG, "=== Doom on the badge ===");

    // Why did we start? A panic and a clean boot look identical otherwise,
    // and an I_Error and a hard fault need completely different hunting.
    // Only interesting after a panic. Printing a stale message on every
    // ordinary boot made it look like the badge had just crashed when it had not.
    if (esp_reset_reason() == ESP_RST_PANIC)
    {
        ESP_LOGW(TAG, "recovered from a panic");
        I_ReportLastCrash();
    }
    report_memory("boot");

    buttons_init();

    // The radio has to come up here, before D_DoomMain, because of how the
    // zone heap is sized: I_ZoneBase takes the largest contiguous block minus
    // a small reserve, so whatever WiFi has not claimed by then is gone for
    // good. Bringing it up first costs the zone whatever WiFi keeps, which is
    // the honest accounting anyway -- the alternative is Z_Init succeeding and
    // esp_wifi_start failing later with nothing left to allocate from.
    if (badge_radio_init() != ESP_OK)
        ESP_LOGE(TAG, "radio would not start; co-op will not be offered");
    report_memory("after radio_init");

    display_init_bus();
    display_reset_and_init(PANEL_CONFIRMED);
    display_fill_rect(0, 0, 320, 240, 0x0000);

    if (!video_init()) {
        ESP_LOGE(TAG, "no framebuffer, stopping");
        return;
    }
    report_memory("after video_init");

    // Upstream's doomgeneric_Create also allocates DG_ScreenBuffer and reads a
    // response file off disk. We have no disk, and DG_Init points the screen
    // buffer at the framebuffer that already exists.
    myargc = 1;
    myargv = s_argv;

    DG_Init();

    ESP_LOGI(TAG, "handing over to D_DoomMain");

    // D_DoomMain does *not* keep running the game. doomgeneric's D_DoomLoop
    // finishes setup, calls doomgeneric_Tick() exactly once and returns --
    // upstream's platform backends drive the loop themselves. Returning here
    // is what left the title screen frozen with the main task deleted.
    D_DoomMain();

    ESP_LOGI(TAG, "entering the game loop");
    while (1)
    {
        doomgeneric_Tick();
    }
}
