#include "buttons.h"
#include "badge_pins.h"

#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"

// Indexed by BTN_BIT_*, so the order here follows the shift register, not the
// order the buttons sit in on the badge.
const char *const button_names[BTN_BIT_COUNT] = {
    [BTN_BIT_1]     = "AUX1 (slide)",
    [BTN_BIT_2]     = "UP",
    [BTN_BIT_3]     = "RIGHT",
    [BTN_BIT_4]     = "LEFT",
    [BTN_BIT_5]     = "DOWN",
    [BTN_BIT_6]     = "HOME",
    [BTN_BIT_7]     = "B",
    [BTN_BIT_HPM]   = "A",
    [BTN_BIT_START] = "START",
};

void buttons_init(void)
{
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << PIN_SR_CLK) | (1ULL << PIN_SR_SHLD),
        .mode         = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&out));

    // The 165 outputs push-pull and START has an external 10k pull-up, so no
    // internal pulls are needed on either input.
    gpio_config_t in = {
        .pin_bit_mask = (1ULL << PIN_SR_QH) | (1ULL << PIN_BTN_START),
        .mode         = GPIO_MODE_INPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&in));

    gpio_set_level(PIN_SR_SHLD, 1);   // idle high = shift mode
    gpio_set_level(PIN_SR_CLK, 0);
}

// The shift-out below is bit-banged with 1 us between clock edges, and the
// WiFi task runs at priority 23 -- above everything Doom uses. A preemption
// between two edges stretches a pulse into something the 74HC165 may clock
// twice, which reads back as a phantom press. The whole sequence is ~35 us of
// interrupts-off, which is short enough not to disturb the radio.
static portMUX_TYPE s_sr_lock = portMUX_INITIALIZER_UNLOCKED;

uint16_t buttons_read(void)
{
    portENTER_CRITICAL(&s_sr_lock);

    // Pulse SH/LD low to latch the parallel inputs. Loading is asynchronous,
    // so QH already presents input H by the time we raise it again.
    gpio_set_level(PIN_SR_SHLD, 0);
    esp_rom_delay_us(1);
    gpio_set_level(PIN_SR_SHLD, 1);
    esp_rom_delay_us(1);

    // Shifting runs H -> G -> ... -> A, so we read the array back to front.
    uint16_t raw = 0;
    for (int i = BTN_BIT_HPM; i >= BTN_BIT_1; i--) {
        if (gpio_get_level(PIN_SR_QH)) raw |= (1u << i);
        gpio_set_level(PIN_SR_CLK, 1);
        esp_rom_delay_us(1);
        gpio_set_level(PIN_SR_CLK, 0);
        esp_rom_delay_us(1);
    }

    if (gpio_get_level(PIN_BTN_START)) raw |= (1u << BTN_BIT_START);

    portEXIT_CRITICAL(&s_sr_lock);

    // Active low: invert so a set bit means pressed.
    return (uint16_t)(~raw) & ((1u << BTN_BIT_COUNT) - 1);
}
