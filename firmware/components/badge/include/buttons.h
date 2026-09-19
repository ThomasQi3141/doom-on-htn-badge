#pragma once
#include <stdint.h>

// Bit positions in the value returned by buttons_read().
// Bits 0..7 are the 74HC165 parallel inputs A..H, in schematic order.
// Bit 8 is START, which hangs off GPIO9 instead of the shift register.
enum {
    BTN_BIT_1     = 0,  // 165 A  -- SW11, the MSK12C02 slide switch
    BTN_BIT_2     = 1,  // 165 B  -- SW2
    BTN_BIT_3     = 2,  // 165 C  -- SW3
    BTN_BIT_4     = 3,  // 165 D  -- SW4
    BTN_BIT_5     = 4,  // 165 E  -- SW8
    BTN_BIT_6     = 5,  // 165 F  -- SW7
    BTN_BIT_7     = 6,  // 165 G  -- SW5
    BTN_BIT_HPM   = 7,  // 165 H  -- SW6 (net is named SW_HPM)
    BTN_BIT_START = 8,  // GPIO9
    BTN_BIT_COUNT = 9,
};

// Physical identities, established by pressing each button on real hardware in
// a known order -- the schematic gives net names only, not which switch is which.
typedef enum {
    BADGE_BTN_UP    = BTN_BIT_2,
    BADGE_BTN_DOWN  = BTN_BIT_5,
    BADGE_BTN_LEFT  = BTN_BIT_4,
    BADGE_BTN_RIGHT = BTN_BIT_3,
    BADGE_BTN_A     = BTN_BIT_HPM,
    BADGE_BTN_B     = BTN_BIT_7,
    BADGE_BTN_HOME  = BTN_BIT_6,
    BADGE_BTN_AUX1  = BTN_BIT_1,      // slide switch, not momentary
    BADGE_BTN_START = BTN_BIT_START,
} badge_button_t;

extern const char *const button_names[BTN_BIT_COUNT];

void buttons_init(void);

// Returns a bitmask where a set bit means PRESSED. The hardware is active-low
// (10k pull-ups, switches to ground); this function inverts for you.
uint16_t buttons_read(void);
