/*
 * cal_store.h — calibration that survives a power cycle.
 *
 * DESIGN.md §6.1 puts user configuration in an NVS namespace `cfg` with typed keys,
 * as part of M3. This is the calibration subset of that, brought forward: a gauge you
 * have to re-calibrate after every reboot is not a gauge anybody will trust, and the
 * numbers involved are measured against physical hardware, so losing them costs a
 * bench session rather than a keystroke. M3's config layer absorbs this namespace
 * rather than replacing it.
 *
 * What is stored is everything that turns a raw register into a reading:
 *
 *   shunt resistance, current offset and gain, sign
 *   divider ratio, voltage offset and gain
 *   the shunt's install mode (roles must resolve before the rest can apply)
 *
 * Typed keys rather than one blob, per §6.1. A blob would need its own CRC and
 * migration; NVS already gives per-key atomicity, and a key added next milestone
 * simply reads as absent on an older store.
 */

#pragma once

#include <stdbool.h>

#include "app_ctx.h"
#include "esp_err.h"

/** Writes the live calibration to NVS. Returns ESP_ERR_INVALID_STATE if roles are
 *  unresolved, since there is nothing coherent to store yet. */
esp_err_t cal_store_save(app_ctx_t *ctx);

/** Applies a stored calibration to the sensors. ESP_ERR_NVS_NOT_FOUND when nothing
 *  has been saved, which is the normal first-boot outcome and not an error. */
esp_err_t cal_store_load(app_ctx_t *ctx);

/** Erases the stored calibration. The live values are left alone -- use
 *  `cal reset` for those. */
esp_err_t cal_store_forget(void);

/** True when a saved calibration exists. */
bool cal_store_exists(void);
