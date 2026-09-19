#pragma once
#include <stdint.h>
#include <stdbool.h>

// Doom renders at 320x200 into an 8bpp paletted buffer, exactly as the original
// does. The panel is 320x240 and wants RGB565, so conversion happens on the way
// out -- keeping only 64,000 bytes resident instead of a 128,000-byte RGB565
// framebuffer we could not comfortably afford.
#define DOOM_W 320
#define DOOM_H 200

// Vertical letterbox: 200 rows centred on a 240-row panel.
#define DOOM_Y_OFFSET ((240 - DOOM_H) / 2)

bool video_init(void);

// Takes Doom's PLAYPAL lump (768 bytes of R,G,B) and builds the RGB565 lookup.
void video_set_palette(const uint8_t *playpal);

uint8_t *video_framebuffer(void);

// Converts the 8bpp framebuffer to RGB565 and pushes it to the panel, overlapping
// conversion with DMA so the CPU is not idle during the transfer.
void video_present(void);

void video_clear(uint8_t index);

// FNV-1a over the whole 8bpp framebuffer, so a host-side reference decode of
// the same lump can be compared byte for byte.
uint32_t video_fb_hash(void);
