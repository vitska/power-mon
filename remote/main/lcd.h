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
#define C_PANEL   RGB(22, 26, 34)
#define C_HEADER  RGB(30, 44, 70)
#define C_GREEN   RGB(60, 220, 90)
#define C_YELLOW  RGB(240, 210, 40)
#define C_ORANGE  RGB(255, 150, 30)
#define C_RED     RGB(240, 60, 50)
#define C_CYAN    RGB(60, 200, 230)
#define C_BLUE    RGB(70, 130, 240)

void lcd_init(void);

void lcd_fill(int x, int y, int w, int h, uint16_t color);

/** Draws w x h pixels of RGB565 from `px`, row-major. Blocks until sent. */
void lcd_blit(int x, int y, int w, int h, const uint16_t *px);

/** The shared strip buffer, LCD_STRIP_PX pixels, for callers that render their own
 *  content a strip at a time and hand it to lcd_blit(). Byte order is handled by
 *  lcd_blit(); write plain RGB565 values. */
#define LCD_STRIP_PX (LCD_W * 20)
uint16_t *lcd_strip(void);

/** 5x7 font scaled by `scale` (6*scale px per character, 8*scale high), drawn with
 *  its background so that redrawing a field overwrites the old text. */
void lcd_text(int x, int y, const char *s, int scale, uint16_t fg, uint16_t bg);

/** Width in pixels of `s` at `scale`. */
int lcd_text_w(const char *s, int scale);

/** Text padded with background to exactly `w` px, so a shorter new value erases a
 *  longer old one. Left-aligned. */
void lcd_text_field(int x, int y, int w, const char *s, int scale, uint16_t fg, uint16_t bg);
