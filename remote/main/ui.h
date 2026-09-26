/*
 * ui.h — the three screens: the dashboard, the board picker, the pairing keypad.
 */
#pragma once

/** Runs the UI forever: touch, screen changes, redraws. Call from its own task. */
void ui_run(void);
