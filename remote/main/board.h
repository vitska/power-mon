/*
 * board.h — what differs between the boards the remote runs on, in one place.
 *
 * The board is a build-time choice (menuconfig → "batmon remote display" → Board), set
 * by the board's fragment in boards/ when built with `tools/idf.ps1 -Board <name>`.
 * Each board builds into its own build/<name>/ with its own sdkconfig.<name>, and
 * tools/flash.ps1 picks the build that matches the board it finds on the port.
 *
 * All of them are a classic ESP32-WROOM-32, a 320x240 SPI panel and an XPT2046
 * resistive touch controller. They differ in which controller drives the panel, where
 * its backlight is, and whether the touch controller has an SPI bus of its own.
 *
 *   cyd          ESP32-2432S028R, single micro-USB. ILI9341, backlight 21, touch on SPI3.
 *   cyd2usb      ESP32-2432S028R, USB-C next to the micro-USB. ST7789, otherwise the same.
 *   jc2432w328r  GUITION JC2432W328R. ST7789, backlight 27, touch SHARING the panel's
 *                SPI2 bus (CS 33). The panel pins are the CYD's.
 */
#pragma once

#include "sdkconfig.h"

/* The panel's bus is the same on every board: SPI2, reset tied to EN. */
#define BOARD_LCD_SCLK 14
#define BOARD_LCD_MOSI 13
#define BOARD_LCD_MISO 12
#define BOARD_LCD_CS   15
#define BOARD_LCD_DC   2

/* The touch controller's chip select is the same on every board too; what changes is
 * the bus it hangs off. */
#define BOARD_TOUCH_CS 33

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
/* No touch bus of its own: the XPT2046 sits on the panel's SCLK/MOSI/MISO, which is
 * what frees GPIO 25 and 32 for the capacitive JC2432W328C's I2C controller. */
#define BOARD_TOUCH_SHARED  1

#else
#error "no board selected: menuconfig -> batmon remote display -> Board"
#endif

/* The status LED (led.c) is on only where a board says it has one. */
#ifndef BOARD_HAS_RGB_LED
#define BOARD_HAS_RGB_LED 0
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
