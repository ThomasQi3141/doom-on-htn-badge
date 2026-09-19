#pragma once
#include <stdint.h>
#include <stddef.h>

// The panel has no MISO line, so the controller cannot be identified by reading
// an ID register. Both candidates were tried on hardware and ST7789 is the one
// that renders correct, upright, non-inverted colour bars. ILI9341 is kept only
// so the comparison can be repeated.
typedef enum {
    PANEL_ST7789 = 0,
    PANEL_ILI9341,
    PANEL_VARIANT_COUNT,
} panel_variant_t;

const char *display_variant_name(panel_variant_t v);

void display_init_bus(void);
void display_reset_and_init(panel_variant_t v);

// Sets the target rectangle, then streams `count` RGB565 pixels (big-endian on
// the wire, as both controllers expect).
void display_set_window(uint16_t x, uint16_t y, uint16_t w, uint16_t h);
void display_write_pixels(const uint16_t *px, size_t count);

void display_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);
void display_test_pattern(void);
