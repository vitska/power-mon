/*
 * board.h — what differs between the boards the remote runs on, in one place.
 *
 * The board is a build-time choice (menuconfig → "batmon remote display" → Board), set
 * by the board's fragment in boards/ when built with `tools/idf.ps1 -Board <name>`.
 * Each board builds into its own build/<name>/ with its own sdkconfig.<name>, and
 * tools/flash.ps1 picks the build that matches the board it finds on the port.
 *
 *   cyd          ESP32-2432S028R, single micro-USB. ILI9341 320x240, backlight 21,
 *                XPT2046 touch on SPI3.
 *   cydips       ESP32-2432S028R with a 2.8" IPS panel: the CYD, but the picture (and
 *                touch with it) flipped top to bottom, red and blue swapped, and the RGB
 *                LED in use.
 *   cyd2usb      ESP32-2432S028R, USB-C next to the micro-USB. ST7789, otherwise the same.
 *   jc2432w328r  GUITION JC2432W328R. ST7789, backlight 27, touch SHARING the panel's
 *                SPI2 bus (CS 33), an RGB status LED. The panel pins are the CYD's.
 *   jc1732w319   GUITION JC1732W319, the 1.9" "mini CYD": the same ESP32-WROOM-32 and
 *                panel bus, an ST7789 IPS panel of 170x320, and NO touch -- so no touch
 *                screens either, only the console (console.c).
 *
 * All four share the chip and the panel's SPI pins, so those are the defaults below;
 * a board says only where it differs.
 */
#pragma once

#include "sdkconfig.h"

#if CONFIG_REMOTE_BOARD_CYD

#define BOARD_NAME          "cyd"
#define BOARD_PANEL_ST7789  0
/* MX | MV, RGB order: landscape, verified on the board. With MV (row/column exchange)
 * set, MX flips the picture vertically and MY horizontally -- the reverse of what the
 * names suggest, which is how 0x28 (upside down) became 0xE8 (mirrored left-right)
 * before landing here. The BGR bit (0x08) is off: with it, red and blue swapped and
 * orange showed as blue. Touch is mapped for the picture the right way up. */
#define BOARD_MADCTL        0x60
#define BOARD_INVERT        0
#define BOARD_LCD_BL        21
#define BOARD_TOUCH_SHARED  0
#define BOARD_TOUCH_SCLK    25
#define BOARD_TOUCH_MOSI    32
#define BOARD_TOUCH_MISO    39

#elif CONFIG_REMOTE_BOARD_CYDIPS

#define BOARD_NAME          "cydips"
#define BOARD_PANEL_ST7789  0
/* The ESP32-2432S028R with a 2.8" IPS panel: the CYD's board, pins and all, but its
 * panel is the CYD's flipped top to bottom, with red and blue swapped. MV and BGR: 0x28,
 * where the CYD has MV and MX, 0x60. No inversion. Found on the board by elimination:
 * 0x60 upside down, 0xE0 turned 180 degrees, 0xA0 mirrored, and blue showing as orange
 * until the BGR bit. */
#define BOARD_MADCTL        0x28
#define BOARD_INVERT        0
/* The picture flipped top to bottom from the CYD's, so touch is too: the CYD's
 * calibration with Y reversed. */
#define BOARD_TOUCH_X_MIN   200
#define BOARD_TOUCH_X_MAX   3700
#define BOARD_TOUCH_Y_MIN   3800
#define BOARD_TOUCH_Y_MAX   240
#define BOARD_TOUCH_SWAP_XY 0
#define BOARD_LCD_BL        21
#define BOARD_TOUCH_SHARED  0
#define BOARD_TOUCH_SCLK    25
#define BOARD_TOUCH_MOSI    32
#define BOARD_TOUCH_MISO    39
/* The CYD's RGB LED, beside the TF slot: common anode, red 4, green 16, blue 17. */
#define BOARD_HAS_RGB_LED   1
#define BOARD_LED_R         4
#define BOARD_LED_G         16
#define BOARD_LED_B         17
#define BOARD_LED_ON        0
#define BOARD_LED_DUTY_PCT  10

#elif CONFIG_REMOTE_BOARD_CYD2USB

#define BOARD_NAME          "cyd2usb"
#define BOARD_PANEL_ST7789  1
#define BOARD_MADCTL        0x68 /* MX | MV | BGR: landscape on the 2USB board */
#define BOARD_INVERT        1
#define BOARD_LCD_BL        21
#define BOARD_TOUCH_SHARED  0
#define BOARD_TOUCH_SCLK    25
#define BOARD_TOUCH_MOSI    32
#define BOARD_TOUCH_MISO    39

#elif CONFIG_REMOTE_BOARD_JC2432W328R

#define BOARD_NAME          "jc2432w328r"
#define BOARD_PANEL_ST7789  1
/* MX | MV, RGB order, NOT inverted -- unlike the 2USB board's ST7789 on both counts,
 * verified on the board: with INVON the picture came out negative, and with the BGR
 * bit the Bluetooth icon's blue showed as orange. */
#define BOARD_MADCTL        0x60
#define BOARD_INVERT        0
#define BOARD_LCD_BL        27
/* An RGB status LED, common anode (lit LOW): red 4, green 16, blue 17, verified on the
 * board -- the other way round from what some JC2432W328 pin lists give. */
#define BOARD_HAS_RGB_LED   1
#define BOARD_LED_R         4
#define BOARD_LED_G         16
#define BOARD_LED_B         17
#define BOARD_LED_ON        0
/* Lit at a tenth of full brightness (PWM, led.c): full drive glares in a dim room,
 * and half still lit the bench. */
#define BOARD_LED_DUTY_PCT  10
/* No touch bus of its own: the XPT2046 sits on the panel's SCLK/MOSI/MISO, which is
 * what frees GPIO 25 and 32 for the capacitive JC2432W328C's I2C controller. */
#define BOARD_TOUCH_SHARED  1
/* Both axes run the other way from the 2432S028's: raw X is high at the left, raw Y high
 * at the top. With the CYD's calibration every tap landed mirrored through the centre.
 * Measured on the board with `touch log on`, taps just inside the corners: top-left
 * 3651,3500  top-right 562,3241  bottom-right 541,548  bottom-left 3710,747. */
#define BOARD_TOUCH_X_MIN   3700
#define BOARD_TOUCH_X_MAX   550
#define BOARD_TOUCH_Y_MIN   3400
#define BOARD_TOUCH_Y_MAX   650
#define BOARD_TOUCH_SWAP_XY 0

#elif CONFIG_REMOTE_BOARD_JC1732W319

#define BOARD_NAME          "jc1732w319"
#define BOARD_PANEL_ST7789  1
/* A 240x320 controller showing 170 columns of it, 35 in from each side. Landscape
 * (MV) turns those columns into rows, so the offset lands on Y: rows 35..204. 35 on
 * both sides is also why rotating 180 degrees needs no other offset. IPS, so inverted;
 * RGB order.
 *
 * The panel pins are GUITION's W3-series panel bus (the JC2432W328R's): the headers
 * break out every other usable GPIO, and 2, 13, 14 and 15 are exactly the ones left.
 * The backlight is 21, active high, as on the CYD -- found on the board with the
 * console's `pin` command, as no pin list for it is published. Not the Sunton
 * ESP32-1732S019, which is an ESP32-S3 on different pins. */
#define BOARD_LCD_W         320
#define BOARD_LCD_H         170
#define BOARD_LCD_Y_OFF     35
#define BOARD_MADCTL        0x60
#define BOARD_INVERT        1
#define BOARD_LCD_BL        21
#define BOARD_HAS_TOUCH     0

#else
#error "no board selected: menuconfig -> batmon remote display -> Board"
#endif

/* --- the defaults: the ESP32-2432S028 family's ------------------------------------- */

#ifndef BOARD_LCD_W
#define BOARD_LCD_W         320
#endif
#ifndef BOARD_LCD_H
#define BOARD_LCD_H         240
#endif
/* Where the visible picture starts in the controller's memory, in landscape. */
#ifndef BOARD_LCD_X_OFF
#define BOARD_LCD_X_OFF     0
#endif
#ifndef BOARD_LCD_Y_OFF
#define BOARD_LCD_Y_OFF     0
#endif
/* The panel's bus: SPI2, reset tied to EN (-1: no reset pin to drive). */
#ifndef BOARD_LCD_SCLK
#define BOARD_LCD_SCLK      14
#define BOARD_LCD_MOSI      13
#define BOARD_LCD_MISO      12
#define BOARD_LCD_CS        15
#define BOARD_LCD_DC        2
#endif
#ifndef BOARD_LCD_RST
#define BOARD_LCD_RST       -1
#endif
#ifndef BOARD_LCD_SPI_MODE
#define BOARD_LCD_SPI_MODE  0
#endif

/* Touch: an XPT2046 on CS 33, on its own bus or the panel's (BOARD_TOUCH_SHARED). A
 * board without touch has none of the screens that need it (ui.c), and is set up from
 * the console instead. */
#ifndef BOARD_HAS_TOUCH
#define BOARD_HAS_TOUCH     1
#endif
#define BOARD_TOUCH_CS      33

/* Touch calibration: the XPT2046's raw reading at the picture's left, right, top and
 * bottom edges, after the optional X/Y swap. A pair given high-to-low mirrors that axis.
 * Measured per board with the console's `touch log on`: tap the corners, read the raw
 * values. These are the ESP32-2432S028's. */
#ifndef BOARD_TOUCH_X_MIN
#define BOARD_TOUCH_X_MIN   200
#define BOARD_TOUCH_X_MAX   3700
#define BOARD_TOUCH_Y_MIN   240
#define BOARD_TOUCH_Y_MAX   3800
#define BOARD_TOUCH_SWAP_XY 0
#endif

/* The status LED (led.c) is on only where a board says it has one. */
#ifndef BOARD_HAS_RGB_LED
#define BOARD_HAS_RGB_LED   0
#endif
#ifndef BOARD_LED_DUTY_PCT
#define BOARD_LED_DUTY_PCT  100
#endif

/* The panel overrides apply on top of the board's own settings. */
#if CONFIG_REMOTE_PANEL_INVERT_FLIP
#define BOARD_PANEL_INVERT (!BOARD_INVERT)
#else
#define BOARD_PANEL_INVERT BOARD_INVERT
#endif

#if CONFIG_REMOTE_PANEL_BGR_FLIP
#define BOARD_PANEL_MADCTL (BOARD_MADCTL ^ 0x08)
#else
#define BOARD_PANEL_MADCTL BOARD_MADCTL
#endif
