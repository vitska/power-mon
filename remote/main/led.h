/*
 * led.h — the board's RGB status LED, where it has one (board.h, BOARD_HAS_RGB_LED).
 *
 * Red while there is no connection to the monitor. Blue blinks with Bluetooth
 * traffic, as the header's icon flashes: a short blink for every notification
 * received, a longer one for every command sent. So a steady flicker is a working
 * link, and red or dark says what the screen would, from across the room.
 *
 * On a board without the LED both calls do nothing.
 */
#pragma once

#include "link.h"

void led_init(void);

/** Call from the UI loop with the latest model. */
void led_update(const link_model_t *m);
