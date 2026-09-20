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
#include "esp_timer.h"

#include "badge_pins.h"
#include "display.h"
#include "buttons.h"
#include "video.h"
#include "badge_radio.h"
#include "badge_net.h"

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

    // The only moment a held button can be read before the engine starts
    // consuming events. A hosts, B joins, nothing held is single player
    // exactly as before.
    //
    // START is deliberately not an option: it is GPIO9, the ESP32-C3 boot
    // strap, so holding it at power-on lands in ROM download mode instead of
    // running the game at all.
    {
        uint16_t held = buttons_read();

        // Bench override. Holding a button needs a hand on the badge, which
        // rules out any test driven from the host over USB -- including the
        // one that matters most, two badges pairing with each other. Build
        // with -DBADGE_NET_FORCE_ROLE=1 to host or 2 to join.
#ifdef BADGE_NET_FORCE_ROLE
        held = 0;
        ESP_LOGW(TAG, "BADGE_NET_FORCE_ROLE=%d compiled in; ignoring buttons",
                 BADGE_NET_FORCE_ROLE);
        BadgeNet_RequestRole(BADGE_NET_FORCE_ROLE == 1 ? BADGE_NET_HOST
                                                       : BADGE_NET_CLIENT);
#endif

        if (held & (1u << BADGE_BTN_A))
        {
            ESP_LOGI(TAG, "A held at boot: hosting a co-op game");
            BadgeNet_RequestRole(BADGE_NET_HOST);
        }
        else if (held & (1u << BADGE_BTN_B))
        {
            ESP_LOGI(TAG, "B held at boot: joining a co-op game");
            BadgeNet_RequestRole(BADGE_NET_CLIENT);
        }
    }

    // The radio has to come up here, before D_DoomMain, because of how the
    // zone heap is sized: I_ZoneBase takes the largest contiguous block minus
    // a small reserve, so whatever WiFi has not claimed by then is gone for
    // good. Bringing it up first costs the zone whatever WiFi keeps, which is
    // the honest accounting anyway -- the alternative is Z_Init succeeding and
    // esp_wifi_start failing later with nothing left to allocate from.
    // Unconditional, and that is a deliberate cost. The lobby screen cannot be
    // drawn until the WAD is mapped and hu_font is cached, which is most of the
    // way through D_DoomMain -- long after Z_Init has sized the zone from the
    // largest contiguous block. Whatever WiFi has not claimed by then is gone,
    // so a radio started later cannot start at all.
    //
    // Starting it here costs single player the difference: the zone is 57,344
    // bytes with the radio up against 77,824 without. Measured, both run the
    // arena at 23.6 fps, and the smaller one still holds 9,780 bytes spare --
    // so the price is headroom we are not using, and it buys a co-op mode the
    // player can actually reach from a menu instead of only by holding a
    // button at power-on.
    if (badge_radio_init() != ESP_OK)
    {
        ESP_LOGE(TAG, "radio would not start; co-op will not be offered");
        BadgeNet_RequestRole(BADGE_NET_OFF);
    }
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

    // Radio smoke test, standing in for badge_net.c until it exists. One
    // 78-byte ANNOUNCE a second -- the same frame size co-op's TICSET will
    // use -- so the radio is exercised at a realistic size while Doom runs,
    // and any interference with frame time or the 74HC165 shows up here
    // rather than later. sent_ok rising proves the frame reached the PHY;
    // received rising means a second badge is answering.
    uint8_t probe[74];
    memset(probe, 0xa5, sizeof(probe));
    int64_t next_probe = 0;

    while (1)
    {
        doomgeneric_Tick();

        if (badge_radio_ready())
        {
            int64_t now = esp_timer_get_time();
            if (now >= next_probe)
            {
                next_probe = now + 1000000;
                badge_radio_send(BADGE_MSG_ANNOUNCE, probe, sizeof(probe));

                badge_radio_packet_t pkt;
                while (badge_radio_recv(&pkt, 0))
                {
                    ESP_LOGI(TAG, "rx %u bytes from "
                                  "%02x:%02x:%02x:%02x:%02x:%02x",
                             (unsigned)pkt.len, pkt.mac[0], pkt.mac[1],
                             pkt.mac[2], pkt.mac[3], pkt.mac[4], pkt.mac[5]);
                }

                ESP_LOGI(TAG, "radio: sent_ok %u, failures %u, rx %u, dropped %u",
                         (unsigned)badge_radio_sent_ok(),
                         (unsigned)badge_radio_send_failures(),
                         (unsigned)badge_radio_received(),
                         (unsigned)badge_radio_dropped());
            }
        }
    }
}
