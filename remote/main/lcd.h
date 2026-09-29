/*
 * lcd.h — the 320x240 panel and the few drawing primitives the UI needs.
 *
 * No framebuffer: 150 KB of the original ESP32's ~300 KB of RAM is too much to give a
 * display when the BLE stack needs its share. Everything is drawn straight to the
 * panel -- rectangles, text, and bitmaps rendered a strip at a time into one small
 * DMA buffer. The UI redraws only what changed, which is what makes that enough.
 */
#pragma once

#include <stdint.h>

#define LCD_W 320
#define LCD_H 240

/* RGB565. */
#define RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

#define C_BLACK   RGB(0, 0, 0)
#define C_WHITE   RGB(255, 255, 255)
#define C_GREY    RGB(120, 120, 120)
#define C_DIM     RGB(50, 50, 55)
/* The graph's grid. C_DIM against C_PANEL is a difference of a few counts in each
 * channel -- invisible on a TN panel at any angle but straight on. */
#define C_GRID    RGB(78, 90, 115)
#define C_PANEL   RGB(22, 26, 34)
#define C_HEADER  RGB(30, 44, 70)
#define C_GREEN   RGB(60, 220, 90)
#define C_YELLOW  RGB(240, 210, 40)
#define C_ORANGE  RGB(255, 150, 30)
#define C_RED     RGB(240, 60, 50)
#define C_RED_DIM RGB(70, 14, 12)   /* the dark half of a low-SoC blink */
#define C_CYAN    RGB(60, 200, 230)
#define C_BLUE    RGB(70, 130, 240)

void lcd_init(void);

void lcd_fill(int x, int y, int w, int h, uint16_t color);

/** Draws w x h pixels of RGB565 from `px`, row-major. Blocks until sent. */
void lcd_blit(int x, int y, int w, int h, const uint16_t *px);

/*
 * ZERO-COPY STRIP RENDERING, for callers that draw their own content a band at a time.
 *
 * lcd_strip() hands back the DMA buffer that is free right now; draw straight into it
 * and lcd_blit_strip() sends it without copying anything. That is the whole point: a
 * full-width band is 6400 pixels, and copying and byte-swapping it costs more than a
 * tenth of a millisecond that the panel is not doing anything with.
 *
 * The price is that the CALLER swaps the byte order, with lcd_px(). Do it per colour
 * rather than per pixel -- a graph has a handful of colours and thousands of pixels --
 * and the swap disappears from the cost entirely.
 *
 * Call lcd_strip() AGAIN after every lcd_blit_strip(): the buffers alternate, so the
 * pointer changes each time. Holding the old one writes into a transfer in flight.
 */
#define LCD_STRIP_PX (LCD_W * 20)
uint16_t *lcd_strip(void);

/** Sends w x h pixels (at most LCD_STRIP_PX) from the buffer lcd_strip() just gave. */
void lcd_blit_strip(int x, int y, int w, int h);

/** RGB565 in the panel's byte order, for writing into lcd_strip(). */
static inline uint16_t lcd_px(uint16_t c)
{
    return (uint16_t)((c >> 8) | (c << 8));
}

/** 5x7 font scaled by `scale` (6*scale px per character, 8*scale high), drawn with
 *  its background so that redrawing a field overwrites the old text. */
void lcd_text(int x, int y, const char *s, int scale, uint16_t fg, uint16_t bg);

/** Width in pixels of `s` at `scale`. */
int lcd_text_w(const char *s, int scale);

/** Text padded with background to exactly `w` px, so a shorter new value erases a
 *  longer old one. Left-aligned. */
void lcd_text_field(int x, int y, int w, const char *s, int scale, uint16_t fg, uint16_t bg);
