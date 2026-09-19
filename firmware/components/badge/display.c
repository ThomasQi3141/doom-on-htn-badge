#include "display.h"
#include "badge_pins.h"

#include <string.h>
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "disp";

// Who drives chip select.
//
// 1 = the SPI peripheral, asserting CS per transaction. This is what the panel
//     was first confirmed working with, and it is what ESP-IDF's own esp_lcd
//     driver does: CS toggles between queued transactions and the controller
//     keeps its memory pointer across the gaps during RAMWR.
// 0 = a plain GPIO held low across an entire frame.
#ifndef DISPLAY_HW_CS
#define DISPLAY_HW_CS 1
#endif
static spi_device_handle_t s_spi;
static uint16_t *s_row;                       // one scratch row, DMA-capable
static spi_transaction_t s_trans[DISPLAY_SLOTS];
static bool s_slot_busy[DISPLAY_SLOTS];

const char *display_variant_name(panel_variant_t v)
{
    switch (v) {
    case PANEL_ST7789:  return "ST7789";
    case PANEL_ILI9341: return "ILI9341";
    default:            return "?";
    }
}

#if DISPLAY_HW_CS
static inline void cs_low(void)  { }
static inline void cs_high(void) { }
#else
static inline void cs_low(void)  { gpio_set_level(PIN_DISP_CS, 0); }
static inline void cs_high(void) { gpio_set_level(PIN_DISP_CS, 1); }
#endif

// Callers hold CS themselves; these only move DC and push bytes.
static void spi_tx(const uint8_t *data, size_t len, bool is_cmd)
{
    if (len == 0) return;
    gpio_set_level(PIN_DISP_DC, is_cmd ? 0 : 1);
    spi_transaction_t t = { .length = len * 8, .tx_buffer = data };
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_spi, &t));
}

static void cmd(uint8_t c) { spi_tx(&c, 1, true); }

static void cmd_data(uint8_t c, const uint8_t *d, size_t n)
{
    cmd(c);
    if (n) spi_tx(d, n, false);
}

void display_init_bus(void)
{
    uint64_t out_pins = (1ULL << PIN_DISP_DC) | (1ULL << PIN_DISP_RST);
#if !DISPLAY_HW_CS
    out_pins |= (1ULL << PIN_DISP_CS);
#endif
    gpio_config_t io = { .pin_bit_mask = out_pins, .mode = GPIO_MODE_OUTPUT };
    ESP_ERROR_CHECK(gpio_config(&io));
    cs_high();

    spi_bus_config_t bus = {
        .mosi_io_num     = PIN_DISP_MOSI,
        .miso_io_num     = -1,                 // panel has no MISO
        .sclk_io_num     = PIN_DISP_SCLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = DISP_W * 2 * 32,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev = {
        .clock_speed_hz = DISP_SPI_HZ,
        .mode           = 0,
#if DISPLAY_HW_CS
        .spics_io_num   = PIN_DISP_CS,
#else
        .spics_io_num   = -1,
#endif
        .queue_size     = DISPLAY_SLOTS + 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev, &s_spi));

    s_row = heap_caps_malloc(DISP_W * sizeof(uint16_t), MALLOC_CAP_DMA);
    assert(s_row);

    ESP_LOGI(TAG, "chip select driven by %s",
             DISPLAY_HW_CS ? "the SPI peripheral" : "GPIO, held across a frame");
    ESP_LOGI(TAG, "SPI2 up: sclk=%d mosi=%d cs=%d dc=%d rst=%d @ %d MHz",
             PIN_DISP_SCLK, PIN_DISP_MOSI, PIN_DISP_CS, PIN_DISP_DC,
             PIN_DISP_RST, DISP_SPI_HZ / 1000000);
}

void display_reset_and_init(panel_variant_t v)
{
    gpio_set_level(PIN_DISP_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_DISP_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(150));

    cs_low();
    cmd(0x01);                      // SWRESET
    vTaskDelay(pdMS_TO_TICKS(150));
    cmd(0x11);                      // SLPOUT
    vTaskDelay(pdMS_TO_TICKS(120));

    cmd_data(0x3A, (const uint8_t[]){0x55}, 1);   // COLMOD, 16 bpp

    // MADCTL: landscape 320x240. MV swaps axes; MX/MY set scan direction.
    if (v == PANEL_ILI9341) {
        cmd_data(0x36, (const uint8_t[]){0x28}, 1);  // MV | BGR
    } else {
        cmd_data(0x36, (const uint8_t[]){0x60}, 1);  // MV | MX
    }

    // The discriminator between the two: ST7789 is normally-black and needs
    // inversion on, ILI9341 needs it off. ST7789 is the confirmed part.
    cmd(v == PANEL_ST7789 ? 0x21 : 0x20);

    cmd(0x13);                      // NORON
    vTaskDelay(pdMS_TO_TICKS(10));
    cmd(0x29);                      // DISPON
    vTaskDelay(pdMS_TO_TICKS(120));
    cs_high();

    ESP_LOGI(TAG, "init sequence sent as %s", display_variant_name(v));
}

void display_set_window(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    uint16_t x1 = x + w - 1, y1 = y + h - 1;
    uint8_t buf[4];

    cs_low();
    buf[0] = x >> 8; buf[1] = x & 0xFF; buf[2] = x1 >> 8; buf[3] = x1 & 0xFF;
    cmd_data(0x2A, buf, 4);                     // CASET

    buf[0] = y >> 8; buf[1] = y & 0xFF; buf[2] = y1 >> 8; buf[3] = y1 & 0xFF;
    cmd_data(0x2B, buf, 4);                     // RASET

    cmd(0x2C);                                  // RAMWR
    gpio_set_level(PIN_DISP_DC, 1);             // everything after this is data
}

void display_write_pixels(const uint16_t *px, size_t count)
{
    spi_tx((const uint8_t *)px, count * 2, false);
}

void display_queue(int slot, const uint16_t *px, size_t count)
{
    display_wait_slot(slot);
    spi_transaction_t *t = &s_trans[slot];
    memset(t, 0, sizeof(*t));
    t->length    = count * 16;
    t->tx_buffer = px;
    ESP_ERROR_CHECK(spi_device_queue_trans(s_spi, t, portMAX_DELAY));
    s_slot_busy[slot] = true;
}

void display_wait_slot(int slot)
{
    if (!s_slot_busy[slot]) return;
    spi_transaction_t *done;
    ESP_ERROR_CHECK(spi_device_get_trans_result(s_spi, &done, portMAX_DELAY));
    s_slot_busy[slot] = false;
}

void display_end_frame(void)
{
    for (int i = 0; i < DISPLAY_SLOTS; i++) display_wait_slot(i);
    cs_high();
}

void display_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    if (w == 0 || h == 0) return;
    for (uint16_t i = 0; i < w; i++) s_row[i] = color;
    display_set_window(x, y, w, h);
    for (uint16_t r = 0; r < h; r++) display_write_pixels(s_row, w);
    display_end_frame();
}

// Colours go out big-endian, so these constants are byte-swapped RGB565.
#define SWAP16(c) ((uint16_t)(((c) >> 8) | ((c) << 8)))

void display_test_pattern(void)
{
    static const uint16_t bars[8] = {
        SWAP16(0xFFFF), SWAP16(0xFFE0), SWAP16(0x07FF), SWAP16(0x07E0),
        SWAP16(0xF81F), SWAP16(0xF800), SWAP16(0x001F), SWAP16(0x0000),
    };
    const uint16_t bw = DISP_W / 8;
    for (int i = 0; i < 8; i++) display_fill_rect(i * bw, 0, bw, 200, bars[i]);

    display_fill_rect(0, 200, DISP_W, 20, SWAP16(0x0000));
    display_fill_rect(0, 200, 24, 20, SWAP16(0xF800));
    display_fill_rect(DISP_W - 24, 200, 24, 20, SWAP16(0xFFFF));
    display_fill_rect(0, 220, DISP_W, 20, SWAP16(0x000F));
}
