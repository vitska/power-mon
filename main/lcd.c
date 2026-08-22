/*
 * lcd.c — the internal 128x32 OLED: the panel driver and the screens drawn on it.
 *
 * Both layers live here because there is exactly one panel on this board and the
 * driver had exactly one caller. Keeping them in one translation unit makes the
 * whole framebuffer/flush machinery file-private, so lcd.h carries only what the
 * rest of the firmware asks for -- turn it on, pin a screen, set contrast, show a
 * passkey.
 *
 * PANEL DRIVER (was ssd1306.c). A 4-line text framebuffer with dirty-page tracking,
 * which is all Appendix D's screens ever need. Two constraints come from DESIGN.md
 * §2.5, because this panel shares its bus with the gauge sensors:
 *
 *   1. Flushing pushes ONE PAGE PER TRANSACTION and only the dirty ones, so the
 *      sampler never waits behind a full 512-byte frame.
 *   2. Nothing here holds the bus across a delay. The 100 ms charge-pump settle in
 *      panel init happens before the device is added to the bus, not while holding it.
 *
 * SCREENS (was display_debug.c). Debug mode as specified in §9.11: the panel is held
 * ON, the screens carry raw pre-scaled values, and the renderer reads the same
 * snapshot the console prints from. That last point is the important one -- a debug
 * view that recomputes its own numbers is a debug view that lies about the bug being
 * chased.
 *
 * Power: unapologetically a bench mode. ~6 mA for the panel plus a core that never
 * light-sleeps, against a T2 floor of ~300 uA (§9.8).
 */

#include "history_values.h"
#include "lcd.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bme280.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fixed_fmt.h"
#include "font5x8.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fuelgauge.h"
#include "sdkconfig.h"
#include "values.h"

/* --- panel: private interface ------------------------------------------------- */

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
static esp_err_t ssd1306_init(i2c_master_bus_handle_t bus, const ssd1306_config_t *cfg,
                       ssd1306_handle_t *out);

static esp_err_t ssd1306_display_on(ssd1306_handle_t h, bool on);
static esp_err_t ssd1306_set_contrast(ssd1306_handle_t h, uint8_t contrast);

/** Hardware inversion (0xA6/0xA7). Does not touch the framebuffer, so it is free
 *  and is what the burn-in blink of §9.11 uses. */
static esp_err_t ssd1306_set_invert(ssd1306_handle_t h, bool invert);

/** Framebuffer edits. Nothing reaches the panel until ssd1306_flush(). */
static void ssd1306_clear(ssd1306_handle_t h);

/** Writes @p s at (row, col) in the 6x8 font. Clipped at the right edge; characters
 *  outside 0x20..0x7E render as a filled box so a formatting bug is visible rather
 *  than invisible. */
static void ssd1306_text(ssd1306_handle_t h, uint8_t row, uint8_t col, const char *s);

/** Clears @p row and prints into it. Truncated at SSD1306_COLS. */
static void ssd1306_printf(ssd1306_handle_t h, uint8_t row, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/** Double-size text at a column, without clearing the rest of the two pages -- so a
 *  large value and small text can share the same rows. @p col is in 12-px units. */
static void ssd1306_text_2x(ssd1306_handle_t h, uint8_t row, uint8_t col, const char *s);

/** Same, at double size. @p row is 0 or 1 and covers two 8-px pages. The font is
 *  scaled rather than a second table: at 2x a pixel-doubled 5x7 is indistinguishable
 *  from a hand-drawn 10x14 at reading distance, and costs no flash. */
static void ssd1306_printf_2x(ssd1306_handle_t h, uint8_t row, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/** Pushes dirty pages, one transaction each. Cheap when nothing changed. */
static esp_err_t ssd1306_flush(ssd1306_handle_t h);

/* --- panel: implementation ---------------------------------------------------- */

static const char *PANEL_TAG = "lcd.panel";

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

static esp_err_t ssd1306_init(i2c_master_bus_handle_t bus, const ssd1306_config_t *cfg,
                       ssd1306_handle_t *out)
{
    ESP_RETURN_ON_FALSE(bus && cfg && out, ESP_ERR_INVALID_ARG, PANEL_TAG, "args");

    /* Probe before allocating or configuring anything: "no display fitted" is a
     * normal outcome on this project (DESIGN.md §10), not an error path. */
    ESP_RETURN_ON_ERROR(i2c_master_probe(bus, cfg->i2c_addr, 100), PANEL_TAG,
                        "no panel at 0x%02X", cfg->i2c_addr);

    ssd1306_handle_t h = calloc(1, sizeof(*h));
    ESP_RETURN_ON_FALSE(h, ESP_ERR_NO_MEM, PANEL_TAG, "calloc");

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

    ESP_LOGI(PANEL_TAG, "128x32 at 0x%02X, contrast 0x%02X%s", cfg->i2c_addr,
             cfg->contrast, cfg->flip ? ", flipped" : "");
    *out = h;
    return ESP_OK;

fail:
    i2c_master_bus_rm_device(h->dev);
    free(h);
    return err;
}

static esp_err_t ssd1306_display_on(ssd1306_handle_t h, bool on)
{
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, PANEL_TAG, "handle");
    const esp_err_t err = cmd1(h, on ? 0xAF : 0xAE);
    if (err == ESP_OK) {
        h->on = on;
    }
    return err;
}

static esp_err_t ssd1306_set_contrast(ssd1306_handle_t h, uint8_t contrast)
{
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, PANEL_TAG, "handle");
    const esp_err_t err = cmd2(h, 0x81, contrast);
    if (err == ESP_OK) {
        h->contrast = contrast;
    }
    return err;
}

static esp_err_t ssd1306_set_invert(ssd1306_handle_t h, bool invert)
{
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, PANEL_TAG, "handle");
    return cmd1(h, invert ? 0xA7 : 0xA6);
}

static void ssd1306_clear(ssd1306_handle_t h)
{
    if (!h) {
        return;
    }
    memset(h->fb, 0, sizeof(h->fb));
    h->dirty = (1u << OLED_PAGES) - 1u;
}


static void ssd1306_text(ssd1306_handle_t h, uint8_t row, uint8_t col, const char *s)
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

static void ssd1306_text_2x(ssd1306_handle_t h, uint8_t row, uint8_t col, const char *s)
{
    if (!h || row >= SSD1306_ROWS_2X || !s) {
        return;
    }
    const uint8_t page = (uint8_t)(row * 2);
    for (; *s && col < SSD1306_COLS_2X; s++, col++) {
        draw_glyph_2x(h, page, col * (FONT_ADVANCE * 2), (unsigned char)*s);
    }
}

static void ssd1306_printf_2x(ssd1306_handle_t h, uint8_t row, const char *fmt, ...)
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

static void ssd1306_printf(ssd1306_handle_t h, uint8_t row, const char *fmt, ...)
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

static esp_err_t ssd1306_flush(ssd1306_handle_t h)
{
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, PANEL_TAG, "handle");
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
        ESP_RETURN_ON_ERROR(cmd(h, win, sizeof(win)), PANEL_TAG, "window p%u", p);

        uint8_t buf[1 + OLED_W];
        buf[0] = CTRL_DATA;
        memcpy(&buf[1], h->fb[p], OLED_W);
        ESP_RETURN_ON_ERROR(i2c_master_transmit(h->dev, buf, sizeof(buf), 100), PANEL_TAG,
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

/* --- screens ------------------------------------------------------------------ */

static const char *TAG = "lcd";

/* Burn-in mitigation (DESIGN.md §9.11): a 128x32 OLED holding static text for hours
 * retains it. Invert the whole panel briefly, periodically. Doubles as a visible
 * "the render loop is still running" indicator, which is why it is not subtle. */
#define BLINK_PERIOD_US ((int64_t)5 * 60 * 1000000)
#define BLINK_HOLD_MS   1000

struct {
    ssd1306_handle_t oled;
    app_ctx_t       *ctx;
    volatile bool    enabled;
    volatile int8_t  pinned_screen; /* -1 = auto-cycle */
    volatile uint8_t n_screens;
    volatile uint32_t passkey;      /* non-zero while pairing is in progress */
/*
 * Screen 0 is pinned by default rather than cycling. The panel's job at the bench is
 * to show volts and amps while a meter is being compared against it, and a readout
 * that rotates to error counters every five seconds is not that. 'disp screen auto'
 * turns cycling back on.
 */
    /* Last contrast written, so `config` can report it. The panel has no
     * readable contrast register -- this is the only place the value exists. */
    uint8_t contrast;
} static s_disp = {.pinned_screen = 0, .n_screens = 3, .contrast = 0x40};

/* --- screens ------------------------------------------------------------------ */

/*
 * Screen 0 — volts and amps, double height. Two 16-px lines fill the panel exactly.
 *
 * Right-aligned so the decimal points and the unit letters stack, which is what makes
 * a changing value readable at a glance: the digits move, the frame does not. This is
 * Appendix D's "readable at arm's length" rule applied to the two numbers M1 actually
 * cares about -- there is no SoC to give the large digits to yet.
 *
 *   +---------------------+
 *   |    12.439V          |
 *   |     1.9588A         |
 *   +---------------------+
 */
static void draw_big(const power_sample_t *s, bool valid)
{
    char b[24];

    if (!valid) {
        ssd1306_printf_2x(s_disp.oled, 0, "  -- --");
        ssd1306_printf_2x(s_disp.oled, 1, " no data");
        return;
    }

    fg_status_t fg;
    fg_get(&fg);

    /*
     * SoC gets the large digits, which is Appendix D's rule -- it is the one number
     * someone should be able to read across a garage. Volts and amps sit beside it in
     * the small font, and the bottom two rows carry the amp-hours and the gauge state.
     *
     * A '?' after the percentage means the figure came from voltage alone, with no
     * trustworthy count behind it yet. Silently showing a guess as a measurement is
     * the one thing a gauge must not do.
     */
    /* 12 bytes, not 8: the compiler cannot see that soc_permille is bounded to 1000,
     * and a truncation warning here is worth a few bytes of stack rather than a
     * pragma. */
    char soc[12];
    snprintf(soc, sizeof(soc), "%3lu%%%s",
             (unsigned long)((fg.soc_permille + 5) / 10),
             fg.voltage_only ? "?" : "");
    ssd1306_text_2x(s_disp.oled, 0, 0, soc);

    ssd1306_text(s_disp.oled, 0, 11, FMT_V(b, s->v_pack_uv));
    ssd1306_text(s_disp.oled, 0, 18, "V");
    ssd1306_text(s_disp.oled, 1, 11, FMT_A(b, s->i_ua));
    ssd1306_text(s_disp.oled, 1, 19, "A");

    /* Amp-hours remaining out of the learned capacity: the number that answers "how
     * much is left" in the unit the battery is actually sold in. */
    const int64_t rem_uah = fg.charge_uas / 3600;
    char bcap[24];
    ssd1306_printf(s_disp.oled, 2, "%s/%sAh",
                   fixed_fmt(b, sizeof(b), rem_uah, 1000000, 1),
                   fixed_fmt(bcap, sizeof(bcap), fg.full_capacity_uah, 1000000, 0));
    ssd1306_printf(s_disp.oled, 3, "%-9s %sW", fg_state_str(fg.state),
                   fixed_fmt(b, sizeof(b), s->p_uw, 1000000, 1));
}

/*
 * Screen 1 — live electrical. The numbers the bench meter is being compared
 * against, at the precision the M1 exit criterion needs (DESIGN.md §11).
 *
 *   +---------------------+
 *   | 13.420V  -2.1500A  *|
 *   | P  -28.850W  pga/4  |
 *   | shunt   -21400uV SAT|
 *   | n12345   up 1:23:45 |
 *   +---------------------+
 */
static void draw_live(const power_sample_t *s, bool valid, char spin)
{
    char b1[24], b2[24];

    if (!valid) {
        ssd1306_printf(s_disp.oled, 0, "-- no sample --   %c", spin);
        ssd1306_printf(s_disp.oled, 1, "sensors not ready");
        ssd1306_printf(s_disp.oled, 2, "see 'scan' on the");
        ssd1306_printf(s_disp.oled, 3, "console");
        return;
    }

    ssd1306_printf(s_disp.oled, 0, "%sV %8sA %c",
                   FMT_V(b1, s->v_pack_uv), FMT_A(b2, s->i_ua), spin);
    ssd1306_printf(s_disp.oled, 1, "P %9sW pga%s",
                   FMT_W(b1, s->p_uw), ina219_pga_str(s->pga));
    /* Temperature shares this row with the shunt drop: both are diagnostics, and a
     * 21-column line has room for exactly these two. */
    if (values()->env_valid) {
        ssd1306_printf(s_disp.oled, 2, "sh%7suV %sC",
                       FMT_MV(b1, s->v_shunt_uv),
                       fixed_fmt(b2, sizeof(b2), values()->env.temp_centi_c, 100, 1));
    } else {
        ssd1306_printf(s_disp.oled, 2, "shunt %7suV%s",
                       FMT_MV(b1, s->v_shunt_uv), s->saturated ? " SAT" : "");
    }

    const uint32_t up_s = (uint32_t)(esp_timer_get_time() / 1000000);
    ssd1306_printf(s_disp.oled, 3, "n%-7lu up%lu:%02lu:%02lu",
                   (unsigned long)values()->n_samples,
                   (unsigned long)(up_s / 3600), (unsigned long)((up_s / 60) % 60),
                   (unsigned long)(up_s % 60));
}

/*
 * Screen 2 — health. What is wrong, if anything: which sensors answered, whether
 * roles resolved, the error counters and the noise figure. This is the screen that
 * finds wiring faults (Appendix D, D1's job).
 *
 *   +---------------------+
 *   | A40 ok  B41 ok  N  *|
 *   | bus0 nf12 rng0 unr0 |
 *   | mn-0.0020 sd0.0001  |
 *   | off  -14uA  10.0mR  |
 *   +---------------------+
 */
static void draw_health(char spin)
{
    const app_ctx_t *ctx = s_disp.ctx;
    char             b1[24], b2[24];

    const bool have_pos = ctx->sensors && sensors_have_pos(ctx->sensors);
    const bool have_neg = ctx->sensors && sensors_have_neg(ctx->sensors);
    const bool resolved = ctx->sensors &&
                          sensors_get_role_state(ctx->sensors) == SENSORS_ROLE_RESOLVED;

    /* Mode letter, plus '?' when roles have not resolved -- the state in which the
     * gauge deliberately refuses to integrate (§2.10.6). */
    const char *mode = ctx->sensors ? sensors_mode_str(sensors_get_mode(ctx->sensors))
                                    : "none";

    ssd1306_printf(s_disp.oled, 0, "A%02X %s B%02X %s %c%c",
                   CONFIG_BATMON_ADDR_POS_POLE, have_pos ? "ok" : "--",
                   CONFIG_BATMON_ADDR_NEG_POLE, have_neg ? "ok" : "--",
                   resolved ? mode[0] : '?', spin);
    ssd1306_printf(s_disp.oled, 1, "bus%lu nf%lu rg%lu ur%lu",
                   (unsigned long)values()->err_bus,
                   (unsigned long)values()->err_not_finished,
                   (unsigned long)values()->err_range_discard,
                   (unsigned long)values()->err_unresolved);
    ssd1306_printf(s_disp.oled, 2, "mn%s sd%s",
                   FMT_A(b1, stats_mean_ua(history_window())),
                   FMT_A(b2, stats_stddev_ua(history_window())));
    ssd1306_printf(s_disp.oled, 3, "off%7suA %lumR",
                   FMT_MA(b1, values()->last.idle_offset_ua),
                   (unsigned long)(CONFIG_BATMON_SHUNT_UOHM / 1000));
}

/*
 * Pairing screen. This is the reason the panel earns its place in the security
 * design (DESIGN.md §8.5): passkey-display pairing needs somewhere to display the
 * passkey, and a device with a screen can be authenticated rather than falling back
 * to Just Works. It takes over the display because a six-digit number the user is
 * waiting to type must not be rotated away after five seconds.
 */
static void draw_passkey(uint32_t pk)
{
    ssd1306_printf(s_disp.oled, 0, "   BLE PAIRING");
    ssd1306_printf(s_disp.oled, 1, "  passkey %06lu", (unsigned long)pk);
    ssd1306_printf(s_disp.oled, 2, " enter on the phone");
    ssd1306_printf(s_disp.oled, 3, " ");
}

/* --- task --------------------------------------------------------------------- */

static void display_task(void *arg)
{
    (void)arg;

    static const char spinner[] = {'|', '/', '-', '\\'};
    uint32_t          tick = 0;
    int64_t           next_blink_us = esp_timer_get_time() + BLINK_PERIOD_US;
    bool              was_enabled = true;
    int8_t            shown = -1;

    for (;;) {
        if (!s_disp.enabled) {
            if (was_enabled) {
                ssd1306_display_on(s_disp.oled, false);
                was_enabled = false;
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        if (!was_enabled) {
            ssd1306_display_on(s_disp.oled, true);
            was_enabled = true;
        }

        /* Pairing outranks everything, including a pinned screen. */
        const uint32_t pk = s_disp.passkey;
        if (pk != 0) {
            if (shown != 99) {
                ssd1306_clear(s_disp.oled);
                shown = 99;
            }
            draw_passkey(pk);
            if (ssd1306_flush(s_disp.oled) != ESP_OK) {
                ESP_LOGW(TAG, "passkey flush failed");
            }
            vTaskDelay(pdMS_TO_TICKS(CONFIG_BATMON_DISPLAY_REFRESH_MS));
            continue;
        }

        /* Which screen. Auto-cycle unless pinned from the console. */
        int8_t screen = s_disp.pinned_screen;
        if (screen < 0) {
            const uint32_t per =
                CONFIG_BATMON_DISPLAY_SCREEN_MS / CONFIG_BATMON_DISPLAY_REFRESH_MS;
            screen = (int8_t)((per ? (tick / per) : 0) % s_disp.n_screens);
        }
        /* A screen change must repaint every row, including rows whose text happens
         * to be identical -- otherwise the dirty tracking correctly decides there is
         * nothing to do and the old screen stays up. */
        if (screen != shown) {
            ssd1306_clear(s_disp.oled);
            shown = screen;
        }

        const char spin = spinner[tick & 3];
        switch (screen) {
        case 0:  draw_big(&values()->last, values()->last_valid);      break;
        case 1:  draw_live(&values()->last, values()->last_valid, spin); break;
        default: draw_health(spin);                                        break;
        }

        const esp_err_t err = ssd1306_flush(s_disp.oled);
        if (err != ESP_OK) {
            /* The panel shares the gauge's bus (§2.5). A display fault must never
             * escalate: log it, keep going, and let the sampler carry on. */
            ESP_LOGW(TAG, "flush failed: %s", esp_err_to_name(err));
        }

        const int64_t now = esp_timer_get_time();
        if (now >= next_blink_us) {
            ssd1306_set_invert(s_disp.oled, true);
            vTaskDelay(pdMS_TO_TICKS(BLINK_HOLD_MS));
            ssd1306_set_invert(s_disp.oled, false);
            next_blink_us = now + BLINK_PERIOD_US;
        }

        tick++;
        vTaskDelay(pdMS_TO_TICKS(CONFIG_BATMON_DISPLAY_REFRESH_MS));
    }
}

/* --- public ------------------------------------------------------------------- */

esp_err_t lcd_start(app_ctx_t *ctx)
{
    ssd1306_config_t cfg = SSD1306_CONFIG_DEFAULT();
    cfg.i2c_addr         = CONFIG_BATMON_DISPLAY_ADDR;
    cfg.scl_speed_hz     = CONFIG_BATMON_I2CA_FREQ_HZ;
    cfg.contrast         = CONFIG_BATMON_DISPLAY_CONTRAST;
#ifdef CONFIG_BATMON_DISPLAY_FLIP
    cfg.flip = true;
#endif

    const esp_err_t err = ssd1306_init(ctx->bus_a, &cfg, &s_disp.oled);
    if (err != ESP_OK) {
        /* DESIGN.md §10: "OLED absent or NACKs -> display_present = 0". Not fatal,
         * and specifically not worth a reboot loop on a bench board. */
        ESP_LOGW(TAG, "no display at 0x%02X (%s) -- continuing without it",
                 CONFIG_BATMON_DISPLAY_ADDR, esp_err_to_name(err));
        return err;
    }

    s_disp.ctx     = ctx;
    s_disp.enabled = true;

    /* Priority 3, below the sampler's 6: a redraw must never delay a sample
     * (DESIGN.md §2.5, §3.3). */
    if (xTaskCreate(display_task, "display", 3072, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "display on: screen 0 (volts/amps, 2x) pinned, %lu ms refresh. "
                  "'disp screen auto' to cycle all %d",
             (unsigned long)CONFIG_BATMON_DISPLAY_REFRESH_MS, s_disp.n_screens);
    return ESP_OK;
}

bool lcd_present(void)
{
    return s_disp.oled != NULL;
}

void lcd_enable(bool on)
{
    s_disp.enabled = on;
}

bool lcd_enabled(void)
{
    return s_disp.enabled;
}

void lcd_set_screen(int screen)
{
    s_disp.pinned_screen =
        (screen < 0 || screen >= s_disp.n_screens) ? -1 : (int8_t)screen;
}

int lcd_get_screen(void)
{
    return s_disp.pinned_screen;
}

int lcd_n_screens(void)
{
    return s_disp.n_screens;
}

void lcd_show_passkey(uint32_t passkey)
{
    s_disp.passkey = passkey;
}

esp_err_t lcd_set_contrast(uint8_t contrast)
{
    if (!s_disp.oled) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t err = ssd1306_set_contrast(s_disp.oled, contrast);
    if (err == ESP_OK) {
        s_disp.contrast = contrast;
    }
    return err;
}

uint8_t lcd_get_contrast(void)
{
    return s_disp.contrast;
}
