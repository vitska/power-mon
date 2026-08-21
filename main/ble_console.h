/*
 * ble_console.h — bridge the M1 console onto the BLE NUS transport.
 *
 * Safe to call when BLE is disabled in Kconfig: the caller is compiled out. A
 * failure here is logged and ignored, never fatal (DESIGN.md §10).
 */

#pragma once

#include "esp_err.h"

esp_err_t ble_console_start(void);
