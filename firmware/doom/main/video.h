#pragma once
#include <stdint.h>
#include <stdbool.h>

// Doom renders at 320x200 into an 8bpp paletted buffer, exactly as the original
// does. The panel is 320x240 and wants RGB565, so conversion happens on the way
// out -- keeping only 64,000 bytes resident instead of a 128,000-byte RGB565
// framebuffer we could not comfortably afford.
#define DOOM_W 320
#define DOOM_H 200

#define PANEL_H 240

// Doom's 320x200 was drawn for 4:3 monitors with non-square pixels, so it is
// meant to be stretched vertically by 1.2x. 200 * 1.2 is exactly 240, the
// panel's height -- so aspect correction and filling the screen are the same
// operation, and the ratio is exactly 6 destination rows per 5 source rows.
//
// 1 = scale to the full 240 rows (correct aspect, no borders, 20% more SPI)
// 0 = letterbox 200 rows with black bands
#ifndef VIDEO_ASPECT_CORRECT
#define VIDEO_ASPECT_CORRECT 1
#endif

#if VIDEO_ASPECT_CORRECT
#define VIDEO_OUT_H   PANEL_H
#define DOOM_Y_OFFSET 0
#else
#define VIDEO_OUT_H   DOOM_H
#define DOOM_Y_OFFSET ((PANEL_H - DOOM_H) / 2)
#endif

bool video_init(void);

// Takes Doom's PLAYPAL lump (768 bytes of R,G,B) and builds the RGB565 lookup.
void video_set_palette(const uint8_t *playpal);

uint8_t *video_framebuffer(void);

// Converts the 8bpp framebuffer to RGB565 and pushes it to the panel, overlapping
// conversion with DMA so the CPU is not idle during the transfer.
void video_present(void);

// Same conversion, but pushed with blocking polling transfers instead of queued
// DMA. Slower, and used to tell a broken async path apart from a broken bus.
void video_present_sync(void);

void video_clear(uint8_t index);

// FNV-1a over the whole 8bpp framebuffer, so a host-side reference decode of
// the same lump can be compared byte for byte.
uint32_t video_fb_hash(void);
