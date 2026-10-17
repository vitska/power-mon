/*
 * touch.h — the XPT2046 resistive touch controller, reported as taps.
 *
 * The UI only needs taps: every control is a button. A tap is reported once, on the
 * press edge, at the position of that press; holding a finger down does not repeat.
 */
#pragma once

#include <stdbool.h>

void touch_init(void);

/** True once per new press, with its screen position in (*x, *y). */
bool touch_tap(int *x, int *y);

/** Log every press on the console, raw and mapped (for calibrating). */
void touch_log(bool on);

/** The XPT2046's last two pressure conversions, as the UI task last read them: a press
 *  is z1 + 4095 - z2 above 400. For diagnosing a panel that never registers one. */
void touch_pressure(int *z1, int *z2);
