/*
 * ssd1306.c — see ssd1306.h.
 *
 * Panel geometry is fixed at 128x32 (DESIGN.md §2.4). The 128x64 variant needs a
 * different multiplex ratio and COM-pin config, so rather than pretend to support
 * both, the two values that differ are named constants below and the header says
 * 128x32. Guessing panel geometry at runtime is not worth the code.
 */

#include "ssd1306.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "font5x8.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ssd1306";

#define OLED_W     128
#define OLED_PAGES 4 /* 32 px / 8 */

#define OLED_MUX      0x1F /* 32 rows - 1 */
#define OLED_COMPINS  0x02 /* sequential, no remap: the 128x32 value */

/* Co-processor control bytes prefixing every I2C payload. */
#define CTRL_CMD  0x00
#define CTRL_DATA 0x40

struct ssd1306_dev_t {
    i2c_master_dev_handle_t dev;
    uint8_t                 fb[OLED_PAGES][OLED_W];
    uint8_t                 dirty; /* bit per page */
    bool                    on;
    uint8_t                 contrast;
};

static esp_err_t cmd(ssd1306_handle_t h, const uint8_t *bytes, size_t n)
{
    /* Small enough to always fit one stack buffer; the longest init command is 2
     * bytes and the longest sequence sent at once is the init block. */
    uint8_t buf[24];
    if (n + 1 > sizeof(buf)) {
        return ESP_ERR_INVALID_SIZE;
    }
    buf[0] = CTRL_CMD;
    memcpy(&buf[1], bytes, n);
    return i2c_master_transmit(h->dev, buf, n + 1, 100);
}

static esp_err_t cmd1(ssd1306_handle_t h, uint8_t c)
{
    return cmd(h, &c, 1);
}

static esp_err_t cmd2(ssd1306_handle_t h, uint8_t c, uint8_t arg)
{
    const uint8_t b[2] = {c, arg};
    return cmd(h, b, 2);
}

esp_err_t ssd1306_init(i2c_master_bus_handle_t bus, const ssd1306_config_t *cfg,
                       ssd1306_handle_t *out)
{
    ESP_RETURN_ON_FALSE(bus && cfg && out, ESP_ERR_INVALID_ARG, TAG, "args");

    /* Probe before allocating or configuring anything: "no display fitted" is a
     * normal outcome on this project (DESIGN.md §10), not an error path. */
    ESP_RETURN_ON_ERROR(i2c_master_probe(bus, cfg->i2c_addr, 100), TAG,
                        "no panel at 0x%02X", cfg->i2c_addr);

    ssd1306_handle_t h = calloc(1, sizeof(*h));
    ESP_RETURN_ON_FALSE(h, ESP_ERR_NO_MEM, TAG, "calloc");

    const i2c_device_config_t dcfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = cfg->i2c_addr,
        .scl_speed_hz    = cfg->scl_speed_hz,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dcfg, &h->dev);
    if (err != ESP_OK) {
        free(h);
        return err;
    }

    h->contrast = cfg->contrast;

    /*
     * Init sequence, SSD1306 datasheet §10.1 order. Display stays OFF until the
     * charge pump has settled, so the panel never shows a frame of garbage.
     */
    const uint8_t seq[] = {
        0xAE,                     /* display off */
        0xD5, 0x80,               /* clock: divide 1, Fosc 8 */
        0xA8, OLED_MUX,           /* multiplex ratio */
        0xD3, 0x00,               /* display offset */
        0x40,                     /* start line 0 */
        0x8D, 0x14,               /* charge pump on (must precede display-on) */
        0x20, 0x00,               /* horizontal addressing: page auto-advances */
        (uint8_t)(cfg->flip ? 0xA0 : 0xA1), /* segment remap */
        (uint8_t)(cfg->flip ? 0xC0 : 0xC8), /* COM scan direction */
        0xDA, OLED_COMPINS,       /* COM pin hardware config */
        0x81, cfg->contrast,      /* contrast */
        0xD9, 0xF1,               /* pre-charge */
        0xDB, 0x40,               /* VCOMH deselect */
        0xA4,                     /* resume from RAM (not all-on) */
        0xA6,                     /* non-inverted */
        0x2E,                     /* deactivate scroll */
    };
    /* Sent in chunks: cmd()'s buffer is intentionally small, and one transaction
     * per few bytes keeps the shared bus free between them (DESIGN.md §2.5). */
    for (size_t i = 0; i < sizeof(seq);) {
        const size_t n = (sizeof(seq) - i) > 8 ? 8 : (sizeof(seq) - i);
        err = cmd(h, &seq[i], n);
        if (err != ESP_OK) {
            goto fail;
        }
        i += n;
    }

    /* The panel needs ~100 ms after the charge pump comes up before it drives the
     * glass properly (DESIGN.md §2.5, ordering trap 2). Not holding the bus. */
    vTaskDelay(pdMS_TO_TICKS(100));

    ssd1306_clear(h);
    err = ssd1306_flush(h);
    if (err != ESP_OK) {
        goto fail;
    }

    err = cmd1(h, 0xAF); /* display on, with a blank framebuffer already pushed */
    if (err != ESP_OK) {
        goto fail;
    }
    h->on = true;

    ESP_LOGI(TAG, "128x32 at 0x%02X, contrast 0x%02X%s", cfg->i2c_addr,
             cfg->contrast, cfg->flip ? ", flipped" : "");
    *out = h;
    return ESP_OK;

fail:
    i2c_master_bus_rm_device(h->dev);
    free(h);
    return err;
}

esp_err_t ssd1306_display_on(ssd1306_handle_t h, bool on)
{
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "handle");
    const esp_err_t err = cmd1(h, on ? 0xAF : 0xAE);
    if (err == ESP_OK) {
        h->on = on;
    }
    return err;
}

esp_err_t ssd1306_set_contrast(ssd1306_handle_t h, uint8_t contrast)
{
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "handle");
    const esp_err_t err = cmd2(h, 0x81, contrast);
    if (err == ESP_OK) {
        h->contrast = contrast;
    }
    return err;
}

esp_err_t ssd1306_set_invert(ssd1306_handle_t h, bool invert)
{
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "handle");
    return cmd1(h, invert ? 0xA7 : 0xA6);
}

void ssd1306_clear(ssd1306_handle_t h)
{
    if (!h) {
        return;
    }
    memset(h->fb, 0, sizeof(h->fb));
    h->dirty = (1u << OLED_PAGES) - 1u;
}

void ssd1306_clear_row(ssd1306_handle_t h, uint8_t row)
{
    if (!h || row >= OLED_PAGES) {
        return;
    }
    memset(h->fb[row], 0, OLED_W);
    h->dirty |= (uint8_t)(1u << row);
}

void ssd1306_text(ssd1306_handle_t h, uint8_t row, uint8_t col, const char *s)
{
    if (!h || row >= OLED_PAGES || !s) {
        return;
    }

    uint8_t *page = h->fb[row];
    bool     changed = false;

    for (; *s && col < SSD1306_COLS; s++, col++) {
        const int x = col * FONT_ADVANCE;
        const unsigned char ch = (unsigned char)*s;

        uint8_t glyph[FONT_ADVANCE];
        if (ch < FONT_FIRST_CH || ch > FONT_LAST_CH) {
            /* Visible placeholder: a broken format string should look broken. */
            memset(glyph, 0x7E, FONT_WIDTH);
        } else {
            memcpy(glyph, &font5x8[(ch - FONT_FIRST_CH) * FONT_WIDTH], FONT_WIDTH);
        }
        glyph[FONT_WIDTH] = 0x00; /* inter-character gap */

        for (int i = 0; i < FONT_ADVANCE; i++) {
            if (x + i >= OLED_W) {
                break;
            }
            if (page[x + i] != glyph[i]) {
                page[x + i] = glyph[i];
                changed = true;
            }
        }
    }

    /* Only mark dirty on a real change: a static screen must cost nothing on the
     * bus (DESIGN.md §9.10, "screens render only on change"). */
    if (changed) {
        h->dirty |= (uint8_t)(1u << row);
    }
}

/*
 * Draws one glyph at double size. Each source column becomes two columns, and each
 * source byte becomes two bytes stacked across a page boundary: bit n of the glyph
 * lights rows 2n and 2n+1, which lands 8 source rows across 16 display rows.
 */
static void draw_glyph_2x(ssd1306_handle_t h, uint8_t page, int x, unsigned char ch)
{
    uint8_t glyph[FONT_ADVANCE];
    if (ch < FONT_FIRST_CH || ch > FONT_LAST_CH) {
        memset(glyph, 0x7E, FONT_WIDTH);
    } else {
        memcpy(glyph, &font5x8[(ch - FONT_FIRST_CH) * FONT_WIDTH], FONT_WIDTH);
    }
    glyph[FONT_WIDTH] = 0x00;

    for (int i = 0; i < FONT_ADVANCE; i++) {
        uint16_t e = 0;
        for (int bit = 0; bit < 8; bit++) {
            if (glyph[i] & (1u << bit)) {
                e |= (uint16_t)3u << (2 * bit); /* one row becomes two */
            }
        }
        const uint8_t lo = (uint8_t)(e & 0xFF);
        const uint8_t hi = (uint8_t)(e >> 8);

        for (int dup = 0; dup < 2; dup++) { /* one column becomes two */
            const int dx = x + i * 2 + dup;
            if (dx < 0 || dx >= OLED_W) {
                continue;
            }
            if (h->fb[page][dx] != lo) {
                h->fb[page][dx] = lo;
                h->dirty |= (uint8_t)(1u << page);
            }
            if (page + 1 < OLED_PAGES && h->fb[page + 1][dx] != hi) {
                h->fb[page + 1][dx] = hi;
                h->dirty |= (uint8_t)(1u << (page + 1));
            }
        }
    }
}

void ssd1306_text_2x(ssd1306_handle_t h, uint8_t row, uint8_t col, const char *s)
{
    if (!h || row >= SSD1306_ROWS_2X || !s) {
        return;
    }
    const uint8_t page = (uint8_t)(row * 2);
    for (; *s && col < SSD1306_COLS_2X; s++, col++) {
        draw_glyph_2x(h, page, col * (FONT_ADVANCE * 2), (unsigned char)*s);
    }
}

void ssd1306_printf_2x(ssd1306_handle_t h, uint8_t row, const char *fmt, ...)
{
    if (!h || row >= SSD1306_ROWS_2X) {
        return;
    }
    char buf[SSD1306_COLS_2X + 1];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    size_t n = strlen(buf);
    while (n < SSD1306_COLS_2X) {
        buf[n++] = ' ';
    }
    buf[SSD1306_COLS_2X] = '\0';

    const uint8_t page = (uint8_t)(row * 2);
    for (int c = 0; c < SSD1306_COLS_2X; c++) {
        draw_glyph_2x(h, page, c * (FONT_ADVANCE * 2), (unsigned char)buf[c]);
    }
}

void ssd1306_printf(ssd1306_handle_t h, uint8_t row, const char *fmt, ...)
{
    if (!h || row >= OLED_PAGES) {
        return;
    }
    char buf[SSD1306_COLS + 1];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    /* Pad to full width so the tail of a previous, longer line cannot survive
     * underneath -- clearing the row first would blank it on the bus even when the
     * new text is identical, which defeats the dirty tracking. */
    size_t n = strlen(buf);
    while (n < SSD1306_COLS) {
        buf[n++] = ' ';
    }
    buf[SSD1306_COLS] = '\0';

    ssd1306_text(h, row, 0, buf);
}

esp_err_t ssd1306_flush(ssd1306_handle_t h)
{
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "handle");
    if (!h->dirty) {
        return ESP_OK;
    }

    for (uint8_t p = 0; p < OLED_PAGES; p++) {
        if (!(h->dirty & (1u << p))) {
            continue;
        }

        const uint8_t win[] = {
            0x21, 0, OLED_W - 1, /* column range */
            0x22, p, p,          /* page range: this page only */
        };
        ESP_RETURN_ON_ERROR(cmd(h, win, sizeof(win)), TAG, "window p%u", p);

        uint8_t buf[1 + OLED_W];
        buf[0] = CTRL_DATA;
        memcpy(&buf[1], h->fb[p], OLED_W);
        ESP_RETURN_ON_ERROR(i2c_master_transmit(h->dev, buf, sizeof(buf), 100), TAG,
                            "data p%u", p);

        h->dirty &= (uint8_t)~(1u << p);

        /* Yield between pages. One page is ~3 ms at 400 kHz; four back-to-back
         * would be 13 ms of bus occupancy, and the sampler ticks every 100 ms with
         * a 2.6 ms conversion to collect. Letting it in between pages is the whole
         * reason flush is page-at-a-time (DESIGN.md §2.5). */
        taskYIELD();
    }
    return ESP_OK;
}
