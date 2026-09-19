// Pin map for the 2026 Hacker Badge (ESP32-C3-MINI-1-N4).
// Every assignment here is traced from badge.kicad_sch -- see HARDWARE.md.
#pragma once

// Display: 2.0" 320x240 SPI panel (HS20HS072RX), 4-wire, write-only (no MISO).
// Backlight is hard-wired on; there is no enable or PWM line.
#define PIN_DISP_DC    0   // DISP_RS
#define PIN_DISP_SCLK  1   // DISP_SCL
#define PIN_DISP_CS    2   // DISP_CS
#define PIN_DISP_RST   4   // DISP_RST
#define PIN_DISP_MOSI  10  // DISP_SDA

// None of the above are SPI2 IO_MUX pins, so transfers route through the GPIO
// matrix. That caps the clock at 40 MHz -- do not raise this without scoping it.
#define DISP_SPI_HZ    (40 * 1000 * 1000)

#define DISP_W         320
#define DISP_H         240

// Buttons: eight on a single 74HC165, bit-banged. SER and CLK_INH are grounded,
// so this is one 8-bit stage, not a chain. All inputs have 10k pull-ups:
// a pressed button reads 0.
#define PIN_SR_QH      7   // 165 QH -> MCU in
#define PIN_SR_SHLD    20  // 165 SH/~LD   (UART0 RXD -- no serial console!)
#define PIN_SR_CLK     21  // 165 CLK      (UART0 TXD -- no serial console!)

// The ninth button is not on the shift register. GPIO9 is the ESP32-C3 boot
// strap, so holding START at power-on forces ROM download mode.
#define PIN_BTN_START  9

// 6x WS2812B-2020 chained, level-shifted to 5 V.
#define PIN_LED_DIN    3

// I2C: SC7A20 accelerometer + MFRC522 NFC reader share this bus (4k7 pull-ups).
#define PIN_I2C_SDA    5
#define PIN_I2C_SCL    6
