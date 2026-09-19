// Bring-up firmware for the 2026 Hacker Badge.
//
// It exists to answer the two questions the schematic cannot:
//   1. Which controller is behind the FPC? There is no MISO, so we cannot ask.
//      Instead we cycle candidate init sequences and look at the screen.
//   2. Which physical button is which 74HC165 input? The schematic gives us
//      BTN_1..7 + SW_HPM; it does not say which one is A, UP or HOME.
//
// Console is USB Serial/JTAG -- UART0's pins drive the button shift register,
// so there is no serial console on this board.

#include <stdio.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_chip_info.h"

#include "badge_pins.h"
#include "display.h"
#include "buttons.h"

static const char *TAG = "bringup";

#define VARIANT_HOLD_MS 6000

static void report_memory(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    ESP_LOGI(TAG, "ESP32-C3 rev v%d.%d, %d core(s)",
             chip.revision / 100, chip.revision % 100, chip.cores);
    ESP_LOGI(TAG, "largest free DRAM block: %u bytes",
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    ESP_LOGI(TAG, "total free DRAM:         %u bytes",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));

    // The number that decides whether Doom is possible at all. A 320x200 8 bpp
    // framebuffer needs 64000 bytes and the port needs room to work around it.
    ESP_LOGI(TAG, "a 320x200 8bpp framebuffer would cost 64000 bytes of that");
}

// Timing the thing we will be bandwidth-bound by, at the real clock, over the
// real GPIO-matrix routing -- not a number computed from a datasheet.
static void benchmark_display(void)
{
    const int frames = 20;
    int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < frames; i++) {
        display_fill_rect(0, 0, DISP_W, 200, (i & 1) ? 0xFFFF : 0x0000);
    }
    int64_t us = (esp_timer_get_time() - t0) / frames;

    ESP_LOGI(TAG, "320x200 full-frame push: %" PRId64 " us  (%.1f fps ceiling)",
             us, 1000000.0 / (double)us);
}

// One self-paced loop that answers both open questions at once:
//   START toggles the panel init sequence and redraws the bars, so the pattern
//   stays up as long as it takes to judge it.
//   Every other button logs its schematic net name, which we map onto real
//   button names by pressing them in an agreed order.
static void probe_loop(void)
{
    panel_variant_t variant = PANEL_ST7789;
    uint16_t prev = buttons_read();

    ESP_LOGI(TAG, "=========================================================");
    ESP_LOGI(TAG, "Showing %s. Bars L->R should be:", display_variant_name(variant));
    ESP_LOGI(TAG, "  white yellow cyan green magenta red blue black");
    ESP_LOGI(TAG, "  red square bottom-LEFT, white square bottom-RIGHT");
    ESP_LOGI(TAG, "PRESS START to switch to the other panel variant.");
    ESP_LOGI(TAG, "Any other button just logs its net name.");
    ESP_LOGI(TAG, "=========================================================");

    while (1) {
        uint16_t now = buttons_read();
        uint16_t changed = now ^ prev;

        for (int b = 0; b < BTN_BIT_COUNT; b++) {
            if (!(changed & (1u << b))) continue;
            bool down = now & (1u << b);

            if (b == BTN_BIT_START) {
                if (down) {
                    variant = (variant == PANEL_ST7789) ? PANEL_ILI9341 : PANEL_ST7789;
                    ESP_LOGI(TAG, ">>> now showing %s <<<",
                             display_variant_name(variant));
                    display_reset_and_init(variant);
                    display_test_pattern();
                }
                continue;
            }

            ESP_LOGI(TAG, "%-20s %s   [mask 0x%03x]",
                     button_names[b], down ? "PRESSED " : "released", now);

            // Live feedback in the strip below the orientation markers.
            display_fill_rect(b * 34, 220, 32, 20,
                              down ? 0xE007 /* green, byte-swapped */ : 0x000F);
        }
        prev = now;
        vTaskDelay(pdMS_TO_TICKS(15));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== Hacker Badge bring-up ===");
    report_memory();

    buttons_init();
    display_init_bus();

    display_reset_and_init(PANEL_ST7789);
    display_test_pattern();
    benchmark_display();
    display_test_pattern();

    probe_loop();
}
