/*
 * lcd.c — see lcd.h.
 *
 * ESP32-2432S028 wiring (fixed on the board): SPI2 on SCLK 14, MOSI 13, MISO 12,
 * CS 15, DC 2, reset tied to EN, backlight on GPIO 21 (active high).
 *
 * The controller is driven with our own few commands rather than a panel driver:
 * ESP-IDF ships an ST7789 driver but not an ILI9341 one (that is a managed component),
 * and the two controllers agree on every command used here -- window, memory write,
 * pixel format, access order, inversion. What differs between the two board variants
 * is only the MADCTL value and inversion, which is what the Kconfig choice sets.
 */

#include "lcd.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "font5x8.h"

static const char *TAG = "lcd";

#define PIN_SCLK 14
#define PIN_MOSI 13
#define PIN_MISO 12
#define PIN_CS   15
#define PIN_DC   2

#if CONFIG_REMOTE_PANEL_ST7789
#define MADCTL_BASE 0x68 /* MX | MV | BGR: landscape on the 2USB board */
#define INVERT      1
#else
/* MX | MY | MV | BGR: landscape, USB at the left. 0x28 (MV | BGR, what TFT_eSPI calls
 * rotation 1) came out mirrored in both axes on real hardware. Touch is mapped for the
 * picture the right way up, so it does not follow this. */
#define MADCTL_BASE 0xE8
#define INVERT      0
#endif

#if CONFIG_REMOTE_ROTATE_180
#define MADCTL (MADCTL_BASE ^ 0xC0) /* flip both axes */
#else
#define MADCTL MADCTL_BASE
#endif

static esp_lcd_panel_io_handle_t s_io;
static SemaphoreHandle_t         s_done;
static uint16_t                 *s_tx;    /* DMA buffer, byte-swapped pixels */
static uint16_t                 *s_strip; /* caller-side strip buffer */

/* The panel reads RGB565 most significant byte first; the ESP32 stores it the other
 * way round. Swapped once, on the way into the DMA buffer. */
static inline uint16_t swap16(uint16_t c)
{
    return (uint16_t)((c >> 8) | (c << 8));
}

static bool on_color_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *e,
                          void *ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_done, &woken);
    return woken == pdTRUE;
}

static void cmd(uint8_t c, const uint8_t *data, size_t n)
{
    esp_lcd_panel_io_tx_param(s_io, c, data, n);
}

static void set_window(int x, int y, int w, int h)
{
    const int x1 = x + w - 1, y1 = y + h - 1;
    const uint8_t ca[] = {x >> 8, x & 0xFF, x1 >> 8, x1 & 0xFF};
    const uint8_t ra[] = {y >> 8, y & 0xFF, y1 >> 8, y1 & 0xFF};
    cmd(0x2A, ca, 4); /* CASET */
    cmd(0x2B, ra, 4); /* RASET */
}

/* Sends n pixels already in s_tx. The first chunk of a window carries RAMWR; the rest
 * continue it (-1: no command phase). Waits, because s_tx is about to be reused. */
static void send_px(size_t n, bool first)
{
    esp_lcd_panel_io_tx_color(s_io, first ? 0x2C : -1, s_tx, n * 2);
    xSemaphoreTake(s_done, portMAX_DELAY);
}

void lcd_init(void)
{
    const spi_bus_config_t bus = {
        .sclk_io_num     = PIN_SCLK,
        .mosi_io_num     = PIN_MOSI,
        .miso_io_num     = PIN_MISO,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = LCD_STRIP_PX * 2 + 16,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

    s_done = xSemaphoreCreateBinary();
    const esp_lcd_panel_io_spi_config_t io = {
        .cs_gpio_num         = PIN_CS,
        .dc_gpio_num         = PIN_DC,
        .spi_mode            = 0,
        .pclk_hz             = 40 * 1000 * 1000,
        .trans_queue_depth   = 4,
        .on_color_trans_done = on_color_done,
        .lcd_cmd_bits        = 8,
        .lcd_param_bits      = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io, &s_io));

    s_tx    = heap_caps_malloc(LCD_STRIP_PX * 2, MALLOC_CAP_DMA);
    s_strip = heap_caps_malloc(LCD_STRIP_PX * 2, MALLOC_CAP_8BIT);
    assert(s_tx && s_strip);

    cmd(0x01, NULL, 0); /* SWRESET */
    vTaskDelay(pdMS_TO_TICKS(150));
    cmd(0x11, NULL, 0); /* SLPOUT */
    vTaskDelay(pdMS_TO_TICKS(120));
    const uint8_t colmod = 0x55; /* 16 bits per pixel */
    cmd(0x3A, &colmod, 1);
    const uint8_t madctl = MADCTL;
    cmd(0x36, &madctl, 1);
    cmd(INVERT ? 0x21 : 0x20, NULL, 0); /* INVON / INVOFF */
    cmd(0x13, NULL, 0);                 /* NORON */

    lcd_fill(0, 0, LCD_W, LCD_H, C_BLACK); /* before the display turns on: no garbage */
    cmd(0x29, NULL, 0);                    /* DISPON */

    gpio_config_t bl = {
        .pin_bit_mask = 1ULL << CONFIG_REMOTE_BACKLIGHT_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&bl);
    gpio_set_level(CONFIG_REMOTE_BACKLIGHT_GPIO, 1);
    ESP_LOGI(TAG, "%s panel up, MADCTL 0x%02X", INVERT ? "ST7789" : "ILI9341", MADCTL);
}

uint16_t *lcd_strip(void)
{
    return s_strip;
}

void lcd_fill(int x, int y, int w, int h, uint16_t color)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    const uint16_t c = swap16(color);
    size_t total = (size_t)w * h;
    const size_t chunk = total < LCD_STRIP_PX ? total : LCD_STRIP_PX;
    for (size_t i = 0; i < chunk; i++) {
        s_tx[i] = c;
    }
    set_window(x, y, w, h);
    bool first = true;
    while (total > 0) {
        const size_t n = total < chunk ? total : chunk;
        send_px(n, first);
        first = false;
        total -= n;
    }
}

void lcd_blit(int x, int y, int w, int h, const uint16_t *px)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    size_t total = (size_t)w * h;
    set_window(x, y, w, h);
    bool first = true;
    while (total > 0) {
        const size_t n = total < LCD_STRIP_PX ? total : LCD_STRIP_PX;
        for (size_t i = 0; i < n; i++) {
            s_tx[i] = swap16(px[i]);
        }
        send_px(n, first);
        first = false;
        px    += n;
        total -= n;
    }
}

int lcd_text_w(const char *s, int scale)
{
    return (int)strlen(s) * FONT_ADVANCE * scale;
}

/* One glyph cell at the largest scale used (8): 48 x 64 px. */
static uint16_t s_glyph[FONT_ADVANCE * 8 * 8 * 8];

void lcd_text(int x, int y, const char *s, int scale, uint16_t fg, uint16_t bg)
{
    if (scale < 1) scale = 1;
    if (scale > 8) scale = 8;
    const int gw = FONT_ADVANCE * scale, gh = 8 * scale;

    for (; *s; s++, x += gw) {
        if (x >= LCD_W) {
            break;
        }
        const unsigned char ch = (unsigned char)*s;
        const bool known = ch >= FONT_FIRST_CH && ch <= FONT_LAST_CH;
        const uint8_t *cols = known ? &font5x8[(ch - FONT_FIRST_CH) * FONT_WIDTH] : NULL;

        for (int py = 0; py < gh; py++) {
            const int row = py / scale;
            for (int px = 0; px < gw; px++) {
                const int col = px / scale;
                bool on;
                if (col >= FONT_WIDTH) {
                    on = false;          /* inter-character gap */
                } else if (!known) {
                    on = row < 7;        /* unknown: a visible box, not a blank */
                } else {
                    on = (cols[col] >> row) & 1;
                }
                s_glyph[py * gw + px] = on ? fg : bg;
            }
        }
        const int w = (x + gw > LCD_W) ? LCD_W - x : gw;
        if (w == gw) {
            lcd_blit(x, y, gw, gh, s_glyph);
        } else {
            /* Clipped at the right edge: send the visible columns row by row. */
            for (int py = 0; py < gh; py++) {
                lcd_blit(x, y + py, w, 1, &s_glyph[py * gw]);
            }
        }
    }
}

void lcd_text_field(int x, int y, int w, const char *s, int scale, uint16_t fg, uint16_t bg)
{
    lcd_text(x, y, s, scale, fg, bg);
    const int used = lcd_text_w(s, scale);
    if (used < w) {
        lcd_fill(x + used, y, w - used, 8 * scale, bg);
    }
}
