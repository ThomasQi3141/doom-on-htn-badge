// The badge's platform layer for Doom.
//
// Supplies the three things the engine cannot get for itself here: the WAD
// (mapped from flash, never read into RAM), the screen (Doom's 8bpp buffer is
// the one the SPI driver scans out, with no copy in between), and input
// (eight buttons on a 74HC165 plus START on GPIO9).

#include <string.h>

#include "doomtype.h"
#include "doomkeys.h"
#include "d_event.h"
#include "w_file.h"
#include "z_zone.h"
#include "i_system.h"
#include "doomgeneric.h"

#include "esp_partition.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "buttons.h"
#include "video.h"

static const char *TAG = "doom.plat";

// ------------------------------------------------------------------ the WAD
//
// Doom's w_wad.c already knows how to use a memory-mapped WAD: when
// wad_file_t.mapped is non-NULL, W_CacheLumpNum hands back a pointer into the
// mapping and allocates nothing. On a normal port that is a small optimisation;
// here it is the difference between fitting and not, because lump caching is
// what dominates a zone heap.

static esp_partition_mmap_handle_t s_map;
static const uint8_t *s_wad_base;
static unsigned int s_wad_len;

extern wad_file_class_t badge_wad_file;

static wad_file_t *Badge_OpenFile(char *path)
{
    (void)path;   // there is one WAD and it is always the flash partition

    if (s_wad_base == NULL)
    {
        const esp_partition_t *part = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "wad");
        if (part == NULL)
        {
            ESP_LOGE(TAG, "no 'wad' partition");
            return NULL;
        }

        const void *ptr;
        if (esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA,
                               &ptr, &s_map) != ESP_OK)
        {
            ESP_LOGE(TAG, "could not map the wad partition");
            return NULL;
        }
        s_wad_base = ptr;

        if (memcmp(s_wad_base, "IWAD", 4) != 0 &&
            memcmp(s_wad_base, "PWAD", 4) != 0)
        {
            ESP_LOGE(TAG, "no WAD magic at the start of the partition");
            return NULL;
        }

        // The partition is larger than the WAD, so derive the real length from
        // the directory rather than reporting the whole partition.
        int numlumps, infotableofs;
        memcpy(&numlumps, s_wad_base + 4, 4);
        memcpy(&infotableofs, s_wad_base + 8, 4);
        s_wad_len = (unsigned int)(infotableofs + numlumps * 16);

        ESP_LOGI(TAG, "WAD mapped at %p, %u bytes, %d lumps",
                 s_wad_base, s_wad_len, numlumps);
    }

    wad_file_t *w = Z_Malloc(sizeof(wad_file_t), PU_STATIC, NULL);
    w->file_class = &badge_wad_file;
    w->mapped = (byte *)s_wad_base;
    w->length = s_wad_len;
    return w;
}

static void Badge_CloseFile(wad_file_t *file)
{
    Z_Free(file);          // the mapping outlives every handle
}

static size_t Badge_Read(wad_file_t *file, unsigned int offset,
                         void *buffer, size_t buffer_len)
{
    if (offset >= file->length) return 0;
    if (offset + buffer_len > file->length) buffer_len = file->length - offset;
    memcpy(buffer, s_wad_base + offset, buffer_len);
    return buffer_len;
}

wad_file_class_t badge_wad_file =
{
    Badge_OpenFile,
    Badge_CloseFile,
    Badge_Read,
};

// ------------------------------------------------------------------ screen
//
// DG_ScreenBuffer is pointed straight at the framebuffer the SPI driver scans
// out, so I_FinishUpdate has nothing to copy.

pixel_t *DG_ScreenBuffer = NULL;

void DG_Init(void)
{
    DG_ScreenBuffer = (pixel_t *)video_framebuffer();
    ESP_LOGI(TAG, "screen buffer at %p, shared with the SPI scanout",
             DG_ScreenBuffer);
}

extern int gametic;
int I_GetTime(void);
int Z_FreeMemory(void);
extern int badge_vp_overflow;
extern int badge_ds_overflow;

// Doom assumes every pixel of the 3D view is repainted each frame and so never
// clears. Anything it fails to paint -- a dropped wall segment, a missing
// visplane, a gap in the BSP -- leaves the previous frame showing through,
// which reads as sprites smearing as the view turns.
//
// Clearing the view ourselves costs one memset per frame and turns that
// failure mode from a smear into black, which is both far less distracting and
// diagnostic: if smearing survives this, the cause is not unpainted pixels.
extern int viewheight;
extern int viewwindowy;

void DG_DrawFrame(void)
{
    static int frames;
    static int64_t t0;

    video_present();

    // After presenting, not before: this frame has already been sent, and the
    // clear is what gives the *next* one a clean slate. Only the 3D view is
    // cleared -- the status bar is redrawn on change, not every frame.
    {
        uint8_t *fb = video_framebuffer();
        int y0   = (viewwindowy > 0) ? viewwindowy : 0;
        int rows = (viewheight  > 0) ? viewheight  : 168;
        if (y0 + rows > DOOM_H) rows = DOOM_H - y0;
        if (rows > 0) memset(fb + (size_t)y0 * DOOM_W, 0, (size_t)rows * DOOM_W);
    }

    // Is the loop alive, is game time advancing, and do the buttons read?
    // A static title screen with dead input can mean any of the three.
    // Set to 1 when tethered and debugging; off for a demo, where nothing is
    // reading the port and the write can stall.
#ifndef BADGE_FRAME_LOG
#define BADGE_FRAME_LOG 0
#endif
    if (BADGE_FRAME_LOG && ++frames % 60 == 0)
    {
        int64_t now = esp_timer_get_time();
        // Zone free is in here because a crash that only happens "after a
        // while" is usually memory filling up, and that is visible before the
        // failure rather than only at it.
        ESP_LOGI(TAG, "frame %d: %.1f fps, gametic %d, zone free %d, buttons 0x%03x",
                 frames, 60.0 / ((now - t0) / 1000000.0),
                 gametic, Z_FreeMemory(), buttons_read());
        if (badge_vp_overflow || badge_ds_overflow)
        {
            ESP_LOGW(TAG, "  renderer ran out: visplanes %d, drawsegs %d "
                          "-- unpainted pixels show the previous frame",
                     badge_vp_overflow, badge_ds_overflow);
            badge_vp_overflow = badge_ds_overflow = 0;
        }
        t0 = now;
    }
}

void DG_SetWindowTitle(const char *title) { (void)title; }

// ------------------------------------------------------------------ timing
uint32_t DG_GetTicksMs(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void DG_SleepMs(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

// ------------------------------------------------------------------ input
//
// The engine wants a stream of key up/down events, so edges are derived by
// diffing the shift register against the previous read.

static const unsigned char s_keymap[BTN_BIT_COUNT] = {
    [BADGE_BTN_UP]    = KEY_UPARROW,
    [BADGE_BTN_DOWN]  = KEY_DOWNARROW,
    [BADGE_BTN_LEFT]  = KEY_LEFTARROW,
    [BADGE_BTN_RIGHT] = KEY_RIGHTARROW,
    [BADGE_BTN_A]     = KEY_FIRE,
    [BADGE_BTN_B]     = KEY_USE,
    [BADGE_BTN_START] = KEY_ENTER,
    [BADGE_BTN_HOME]  = KEY_ESCAPE,
    [BADGE_BTN_AUX1]  = KEY_RSHIFT,   // the slide switch holds run on or off
};

static uint16_t s_prev_buttons;

int DG_GetKey(int *pressed, unsigned char *key)
{
    uint16_t now = buttons_read();
    uint16_t changed = now ^ s_prev_buttons;

    for (int b = 0; b < BTN_BIT_COUNT; b++)
    {
        if (!(changed & (1u << b))) continue;
        if (s_keymap[b] == 0) continue;

        // Report one edge per call and remember it, so the next call sees the
        // rest of the changes rather than losing them.
        s_prev_buttons ^= (uint16_t)(1u << b);
        *pressed = (now & (1u << b)) ? 1 : 0;
        *key = s_keymap[b];
        return 1;
    }

    s_prev_buttons = now;
    return 0;
}
