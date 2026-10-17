/*
 * ui.h — the screens: the dashboard, and on a board with touch the board picker,
 * settings, calibration keypads and the pairing keypad.
 */
#pragma once

#include <stdbool.h>

/** Runs the UI forever: touch, screen changes, redraws. Call from its own task. */
void ui_run(void);

/** Opens (true) or leaves (false) the firmware update screen, which is what makes the
 *  remote visible to the phone app. For the console; safe from any task. */
void ui_update_mode(bool on);
