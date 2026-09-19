#include "video.h"
#include "display.h"
#include "badge_pins.h"

#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "video";

// Rows converted per DMA transfer. Two of these ping-pong, so the CPU converts
// the next chunk while the SPI peripheral is still sending the previous one.
#define CHUNK_ROWS 16

static uint8_t  *s_fb;                     // DOOM_W * DOOM_H, 8bpp
static uint16_t *s_chunk[DISPLAY_SLOTS];   // DMA-capable, byte-swapped RGB565
static uint16_t  s_pal[256];

bool video_init(void)
{
    s_fb = heap_caps_malloc(DOOM_W * DOOM_H, MALLOC_CAP_8BIT);
    if (!s_fb) {
        ESP_LOGE(TAG, "could not allocate the %d-byte framebuffer", DOOM_W * DOOM_H);
        return false;
    }
    memset(s_fb, 0, DOOM_W * DOOM_H);

    for (int i = 0; i < DISPLAY_SLOTS; i++) {
        s_chunk[i] = heap_caps_malloc(DOOM_W * CHUNK_ROWS * sizeof(uint16_t),
                                      MALLOC_CAP_DMA);
        if (!s_chunk[i]) {
            ESP_LOGE(TAG, "could not allocate DMA chunk %d", i);
            return false;
        }
    }

    ESP_LOGI(TAG, "framebuffer %d bytes at %p, %d DMA chunks of %d bytes",
             DOOM_W * DOOM_H, s_fb, DISPLAY_SLOTS,
             DOOM_W * CHUNK_ROWS * (int)sizeof(uint16_t));
    return true;
}

void video_set_palette(const uint8_t *playpal)
{
    for (int i = 0; i < 256; i++) {
        uint8_t r = playpal[i * 3 + 0];
        uint8_t g = playpal[i * 3 + 1];
        uint8_t b = playpal[i * 3 + 2];
        uint16_t c = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
        s_pal[i] = (uint16_t)((c >> 8) | (c << 8));   // panel wants big-endian
    }
}

uint8_t *video_framebuffer(void) { return s_fb; }

void video_clear(uint8_t index) { memset(s_fb, index, DOOM_W * DOOM_H); }

// FNV-1a rather than a CRC: the host reference decoder has to compute the exact
// same value, and this has one unambiguous definition instead of several
// competing conventions about bit and byte order.
uint32_t video_fb_hash(void)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < DOOM_W * DOOM_H; i++) {
        h ^= s_fb[i];
        h *= 16777619u;
    }
    return h;
}

void video_present(void)
{
    display_set_window(0, DOOM_Y_OFFSET, DOOM_W, DOOM_H);

    int slot = 0;
    for (int y = 0; y < DOOM_H; y += CHUNK_ROWS) {
        int rows = (y + CHUNK_ROWS <= DOOM_H) ? CHUNK_ROWS : (DOOM_H - y);

        // Reusing this slot's buffer means waiting for its previous DMA first.
        display_wait_slot(slot);

        const uint8_t *src = s_fb + (size_t)y * DOOM_W;
        uint16_t *dst = s_chunk[slot];
        for (int i = 0; i < rows * DOOM_W; i++) dst[i] = s_pal[src[i]];

        display_queue(slot, dst, (size_t)rows * DOOM_W);
        slot ^= 1;
    }
    display_end_frame();
}

void video_present_sync(void)
{
    display_set_window(0, DOOM_Y_OFFSET, DOOM_W, DOOM_H);
    for (int y = 0; y < DOOM_H; y += CHUNK_ROWS) {
        int rows = (y + CHUNK_ROWS <= DOOM_H) ? CHUNK_ROWS : (DOOM_H - y);
        const uint8_t *src = s_fb + (size_t)y * DOOM_W;
        uint16_t *dst = s_chunk[0];
        for (int i = 0; i < rows * DOOM_W; i++) dst[i] = s_pal[src[i]];
        display_write_pixels(dst, (size_t)rows * DOOM_W);
    }
    display_end_frame();
}
