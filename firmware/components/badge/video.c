#include "video.h"
#include "display.h"
#include "badge_pins.h"

#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "video";

// Rows converted per DMA transfer. Two of these ping-pong, so the CPU converts
// the next chunk while the SPI peripheral is still sending the previous one.
// 8 rows per transfer. Halving this from 16 gives back 10,240 bytes and the
// SPI peripheral stays saturated either way -- the conversion is not the
// bottleneck, the 40 MHz bus is.
#define CHUNK_ROWS 8

// Static, not heap-allocated. At 64,000 bytes this is the single biggest
// allocation in the system, and taking it from the heap decides which of the
// three DRAM regions gets split -- so an unrelated .bss change could move it
// and halve the contiguous space left for Doom's zone. As a static array it is
// placed once at link time and the heap regions stay whole.
static uint8_t  s_fb[DOOM_W * DOOM_H];
static uint16_t *s_chunk[DISPLAY_SLOTS];   // DMA-capable, byte-swapped RGB565
static uint16_t  s_pal[256];

bool video_init(void)
{
    memset(s_fb, 0, sizeof(s_fb));

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
    ESP_LOGI(TAG, "output %dx%d, %s", DOOM_W, VIDEO_OUT_H,
             VIDEO_ASPECT_CORRECT ? "aspect-corrected 1.2x, fills the panel"
                                  : "letterboxed with black bands");
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

void video_set_palette_gamma(const uint8_t *playpal, const uint8_t *gamma)
{
    for (int i = 0; i < 256; i++) {
        uint8_t r = gamma[playpal[i * 3 + 0]];
        uint8_t g = gamma[playpal[i * 3 + 1]];
        uint8_t b = gamma[playpal[i * 3 + 2]];
        uint16_t c = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
        s_pal[i] = (uint16_t)((c >> 8) | (c << 8));
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

// Source row feeding a given output row. With aspect correction this is the
// exact 5/6 ratio; without it the mapping is one to one.
static inline int src_row_for(int out_y)
{
#if VIDEO_ASPECT_CORRECT
    return (out_y * DOOM_H) / VIDEO_OUT_H;   // *5/6, integer exact
#else
    return out_y;
#endif
}

void video_present(void)
{
    display_set_window(0, DOOM_Y_OFFSET, DOOM_W, VIDEO_OUT_H);

    int slot = 0;
    for (int y = 0; y < VIDEO_OUT_H; y += CHUNK_ROWS) {
        int rows = (y + CHUNK_ROWS <= VIDEO_OUT_H) ? CHUNK_ROWS : (VIDEO_OUT_H - y);

        // Reusing this slot's buffer means waiting for its previous DMA first.
        display_wait_slot(slot);

        uint16_t *dst = s_chunk[slot];
        for (int r = 0; r < rows; r++) {
            const uint8_t *src = s_fb + (size_t)src_row_for(y + r) * DOOM_W;
            uint16_t *out = dst + (size_t)r * DOOM_W;
            for (int x = 0; x < DOOM_W; x++) out[x] = s_pal[src[x]];
        }

        display_queue(slot, dst, (size_t)rows * DOOM_W);
        slot ^= 1;
    }
    display_end_frame();
}

void video_present_sync(void)
{
    display_set_window(0, DOOM_Y_OFFSET, DOOM_W, VIDEO_OUT_H);
    for (int y = 0; y < VIDEO_OUT_H; y += CHUNK_ROWS) {
        int rows = (y + CHUNK_ROWS <= VIDEO_OUT_H) ? CHUNK_ROWS : (VIDEO_OUT_H - y);
        uint16_t *dst = s_chunk[0];
        for (int r = 0; r < rows; r++) {
            const uint8_t *src = s_fb + (size_t)src_row_for(y + r) * DOOM_W;
            uint16_t *out = dst + (size_t)r * DOOM_W;
            for (int x = 0; x < DOOM_W; x++) out[x] = s_pal[src[x]];
        }
        display_write_pixels(dst, (size_t)rows * DOOM_W);
    }
    display_end_frame();
}
