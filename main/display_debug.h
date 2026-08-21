/*
 * display_debug.h — debug screens on the 128x32 OLED (DESIGN.md §9.11).
 *
 * Every entry point is safe to call when no panel is fitted: display_debug_start()
 * returns the probe error and the rest become no-ops. Absence of the display is a
 * normal configuration (§10), not a failure the console has to guard against.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_ctx.h"
#include "esp_err.h"

/** Probes the panel and starts the render task. Returns the probe error if no
 *  display answered; the caller should carry on regardless. */
esp_err_t display_debug_start(app_ctx_t *ctx);

bool display_debug_present(void);

/** Blanks or restores the panel. The render task keeps running either way, so
 *  re-enabling shows fresh numbers immediately. */
void display_debug_enable(bool on);
bool display_debug_enabled(void);

/** Pins one screen, or pass a negative value to resume auto-cycling. */
void display_debug_set_screen(int screen);
int  display_debug_get_screen(void); /**< <0 when auto-cycling */
int  display_debug_n_screens(void);

esp_err_t display_debug_set_contrast(uint8_t contrast);

/** Shows a BLE pairing passkey, overriding the normal screens; 0 clears it.
 *  Safe to call from the NimBLE host task: it only stores the value. */
void display_debug_show_passkey(uint32_t passkey);
