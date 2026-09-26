/*
 * touch.c — see touch.h.
 *
 * ESP32-2432S028 wiring: the XPT2046 has its own SPI pins, separate from the display's
 * -- CLK 25, MOSI 32, MISO 39, CS 33 (IRQ on 36, unused: pressure says the same thing
 * and needs no extra pin). It runs on SPI3 at 1 MHz; the chip's limit is ~2.5 MHz and
 * a resistive panel gains nothing from speed.
 *
 * Each read is a pressure check and then several X/Y conversions, keeping the median.
 * A resistive panel's first conversion after the press edge is routinely wild, and a
 * single sample is how a tap lands on the wrong button.
 */

#include "touch.h"

#include <stdlib.h>

#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "lcd.h"

static const char *TAG __attribute__((unused)) = "touch";

#define PIN_CLK  25
#define PIN_MOSI 32
#define PIN_MISO 39
#define PIN_CS   33

#define Z_THRESHOLD 400  /* pressure below this is no touch */
#define SAMPLES     5

static spi_device_handle_t s_dev;
static bool                s_was_down;
static int64_t             s_last_tap_us;

/* One 12-bit conversion. The reply to a command arrives in the two bytes after it. */
static int conv(uint8_t command)
{
    spi_transaction_t t = {
        .flags     = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA,
        .length    = 24,
        .tx_data   = {command, 0, 0},
    };
    spi_device_polling_transmit(s_dev, &t);
    return ((t.rx_data[1] << 8) | t.rx_data[2]) >> 3;
}

void touch_init(void)
{
    const spi_bus_config_t bus = {
        .sclk_io_num   = PIN_CLK,
        .mosi_io_num   = PIN_MOSI,
        .miso_io_num   = PIN_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_DISABLED));
    const spi_device_interface_config_t dev = {
        .clock_speed_hz = 1000 * 1000,
        .mode           = 0,
        .spics_io_num   = PIN_CS,
        .queue_size     = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI3_HOST, &dev, &s_dev));
}

static int cmp_int(const void *a, const void *b)
{
    return *(const int *)a - *(const int *)b;
}

static int median(int *v, int n)
{
    qsort(v, n, sizeof(int), cmp_int);
    return v[n / 2];
}

static int map(int raw, int lo, int hi, int out)
{
    int v = (raw - lo) * out / (hi - lo);
    if (v < 0) v = 0;
    if (v > out - 1) v = out - 1;
    return v;
}

/* Returns true while pressed, with the mapped position. */
static bool read(int *x, int *y)
{
    const int z1 = conv(0xB1);
    const int z2 = conv(0xC1);
    const int z  = z1 + 4095 - z2;
    if (z < Z_THRESHOLD) {
        return false;
    }

    int rx[SAMPLES], ry[SAMPLES];
    conv(0x91); /* discarded: the first conversion after a press is noisy */
    for (int i = 0; i < SAMPLES; i++) {
        rx[i] = conv(0x91);
        ry[i] = conv(0xD1);
    }
    conv(0x90); /* last command powers the ADC down between reads */

    int raw_x = median(rx, SAMPLES), raw_y = median(ry, SAMPLES);
#if CONFIG_REMOTE_TOUCH_SWAP_XY
    const int t = raw_x; raw_x = raw_y; raw_y = t;
#endif
    int sx = map(raw_x, CONFIG_REMOTE_TOUCH_X_MIN, CONFIG_REMOTE_TOUCH_X_MAX, LCD_W);
    int sy = map(raw_y, CONFIG_REMOTE_TOUCH_Y_MIN, CONFIG_REMOTE_TOUCH_Y_MAX, LCD_H);
#if CONFIG_REMOTE_ROTATE_180
    sx = LCD_W - 1 - sx;
    sy = LCD_H - 1 - sy;
#endif
#if CONFIG_REMOTE_TOUCH_LOG
    ESP_LOGI(TAG, "raw %d,%d z %d -> %d,%d", raw_x, raw_y, z, sx, sy);
#endif
    *x = sx;
    *y = sy;
    return true;
}

bool touch_tap(int *x, int *y)
{
    int tx, ty;
    const bool down = read(&tx, &ty);
    const bool edge = down && !s_was_down;
    s_was_down = down;

    /* A resistive panel bounces on release as well as on press. */
    const int64_t now = esp_timer_get_time();
    if (!edge || now - s_last_tap_us < 200 * 1000) {
        return false;
    }
    s_last_tap_us = now;
    *x = tx;
    *y = ty;
    return true;
}
