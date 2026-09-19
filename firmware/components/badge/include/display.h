#pragma once
#include <stdint.h>
#include <stddef.h>

// The panel has no MISO line, so the controller cannot be identified by reading
// an ID register. Both candidates were driven on hardware and ST7789 is the one
// that renders correct, upright, non-inverted colour bars. ILI9341 is kept only
// so the comparison can be repeated.
typedef enum {
    PANEL_ST7789 = 0,
    PANEL_ILI9341,
    PANEL_VARIANT_COUNT,
} panel_variant_t;

#define PANEL_CONFIRMED PANEL_ST7789

const char *display_variant_name(panel_variant_t v);

void display_init_bus(void);
void display_reset_and_init(panel_variant_t v);

// CS is driven by hand rather than by the SPI peripheral, so a frame can span
// many DMA transactions without the controller ever seeing CS go high mid-write.
// Every frame is: set_window ... (writes) ... end_frame.
void display_set_window(uint16_t x, uint16_t y, uint16_t w, uint16_t h);
void display_write_pixels(const uint16_t *px, size_t count);
void display_end_frame(void);

// Asynchronous path, used to overlap palette conversion with the SPI transfer.
// Buffers must stay valid and untouched until display_wait_slot() returns.
#define DISPLAY_SLOTS 2
void display_queue(int slot, const uint16_t *px, size_t count);
void display_wait_slot(int slot);

void display_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);
void display_test_pattern(void);
