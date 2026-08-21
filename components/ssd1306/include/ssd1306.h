/*
 * ssd1306.h — 128x32 OLED, text-only, on the shared I2C bus.
 *
 * Scope is deliberately narrow: a 4-line text framebuffer with dirty-page
 * tracking, which is all Appendix D's screens ever need. No graphics primitives,
 * no scrolling, no large-digit font yet (DESIGN.md Appendix D wants one for SoC at
 * M5b; there is no SoC to show at M1).
 *
 * Two constraints come from DESIGN.md §2.5, because this panel shares its bus with
 * the gauge sensors:
 *
 *   1. Flushing pushes ONE PAGE PER TRANSACTION and only the dirty ones, so the
 *      sampler never waits behind a full 512-byte frame. A full redraw is four
 *      short transfers, not one long one.
 *   2. Nothing here holds the bus across a delay. The 100 ms charge-pump settle in
 *      ssd1306_init() happens before the device is added to the bus, not while
 *      holding it.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 128 px / 6 px per glyph = 21 columns; 32 px / 8 px per row = 4 rows. */
#define SSD1306_COLS 21
#define SSD1306_ROWS 4

/** Double-size text: 12 px per glyph and 16 px tall, so 10 columns and 2 rows.
 *  Two of these fill the panel exactly, which is what the volts/amps readout uses. */
#define SSD1306_COLS_2X 10
#define SSD1306_ROWS_2X 2

typedef struct ssd1306_dev_t *ssd1306_handle_t;

typedef struct {
    uint8_t  i2c_addr;      /**< 0x3C typical; 0x3D if SA0 is strapped high */
    uint32_t scl_speed_hz;
    bool     flip;          /**< 180° rotation for enclosure fit (§8.3 tag 0x0074) */
    uint8_t  contrast;      /**< 0x40 default — dim is cheaper AND more readable (§9.10) */
} ssd1306_config_t;

#define SSD1306_CONFIG_DEFAULT()      \
    {                                 \
        .i2c_addr     = 0x3C,         \
        .scl_speed_hz = 400000,       \
        .flip         = false,        \
        .contrast     = 0x40,         \
    }

/** Probes the panel, runs the init sequence and leaves the display ON and cleared.
 *  Returns ESP_ERR_NOT_FOUND if nothing answers at the address, which the caller is
 *  expected to treat as "no display fitted" rather than as a fatal error (§10). */
esp_err_t ssd1306_init(i2c_master_bus_handle_t bus, const ssd1306_config_t *cfg,
                       ssd1306_handle_t *out);

esp_err_t ssd1306_display_on(ssd1306_handle_t h, bool on);
esp_err_t ssd1306_set_contrast(ssd1306_handle_t h, uint8_t contrast);

/** Hardware inversion (0xA6/0xA7). Does not touch the framebuffer, so it is free
 *  and is what the burn-in blink of §9.11 uses. */
esp_err_t ssd1306_set_invert(ssd1306_handle_t h, bool invert);

/** Framebuffer edits. Nothing reaches the panel until ssd1306_flush(). */
void ssd1306_clear(ssd1306_handle_t h);
void ssd1306_clear_row(ssd1306_handle_t h, uint8_t row);

/** Writes @p s at (row, col) in the 6x8 font. Clipped at the right edge; characters
 *  outside 0x20..0x7E render as a filled box so a formatting bug is visible rather
 *  than invisible. */
void ssd1306_text(ssd1306_handle_t h, uint8_t row, uint8_t col, const char *s);

/** Clears @p row and prints into it. Truncated at SSD1306_COLS. */
void ssd1306_printf(ssd1306_handle_t h, uint8_t row, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/** Double-size text at a column, without clearing the rest of the two pages -- so a
 *  large value and small text can share the same rows. @p col is in 12-px units. */
void ssd1306_text_2x(ssd1306_handle_t h, uint8_t row, uint8_t col, const char *s);

/** Same, at double size. @p row is 0 or 1 and covers two 8-px pages. The font is
 *  scaled rather than a second table: at 2x a pixel-doubled 5x7 is indistinguishable
 *  from a hand-drawn 10x14 at reading distance, and costs no flash. */
void ssd1306_printf_2x(ssd1306_handle_t h, uint8_t row, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/** Pushes dirty pages, one transaction each. Cheap when nothing changed. */
esp_err_t ssd1306_flush(ssd1306_handle_t h);

#ifdef __cplusplus
}
#endif
