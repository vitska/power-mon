/*
 * config.h — every setting, in one struct, with one place that persists it.
 *
 * This is the `cfg_t` of DESIGN.md §6.1: NVS namespace `cfg`, typed keys, written on
 * commit only. Before it existed, settings were spread across whoever happened to use
 * them -- the INA219 handles owned their own trims, the fuel gauge owned its own
 * config blob in its own namespace, and the stream rates lived in the shared context
 * and did not survive a reboot at all. Three owners meant three answers to "what is
 * this board set to", and `options` had to interrogate all of them.
 *
 * THE RULE HERE: this struct is the source of truth, and the drivers are downstream
 * of it. config_load() reads NVS and pushes values into the sensors, the gauge, the
 * panel and the radio; config_commit() writes them back. A setter changes cfg_t and
 * commits, rather than poking a driver and hoping something persists it later.
 *
 * WHAT IS NOT HERE: measurements (values.h), accumulated charge and learned capacity
 * (the fuel gauge's own `fg` namespace -- those are history, not settings), and
 * resolved sensor roles, which are an observation about the wiring rather than a
 * choice about it. The install MODE is a setting; which device ends up in which role
 * is not.
 *
 * MIGRATION. M1 and M2 stored the calibration subset in namespace `cal` with typed
 * keys. A board that has one and no `cfg` yet is read from `cal` once and written to
 * `cfg`, because losing a bench calibration to a refactor would be unforgivable: the
 * numbers were measured against physical hardware with a meter in hand.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "fuelgauge.h"
#include "ina219.h"
#include "sensors.h"

struct app_ctx_s;

typedef struct {
    /* --- sensing and calibration (§2.9, §5.5) --------------------------------- */

    uint32_t shunt_uohm;
    int32_t  i_offset_ua;
    uint32_t i_gain_ppm;
    bool     invert_sign;
    int32_t  v_offset_uv;
    uint32_t v_gain_ppm;
    uint32_t v_divider_q16;
    ina219_vbus_comp_t vbus_comp;
    ina219_pga_t       pga_max;
    sensors_mode_t     install_mode;
    ina219_profile_t   profile;
    ina219_adc_t       cont_adc;

    /* --- the fuel gauge (§5) -------------------------------------------------- */

    /** Pushed into the gauge wholesale, because the gauge validates the shape of it
     *  (v_0pct < v_100pct <= v_full and so on) and that check belongs with the model. */
    fg_config_t gauge;

    /* --- telemetry (CLI.md §5) ------------------------------------------------ */

    bool     stream_enabled;
    bool     stream_csv;
    uint32_t rate_fast_ms;
    uint32_t rate_calc_ms;
    uint32_t rate_diag_ms;
    uint32_t rate_env_ms;

    /* --- the SoC history (history_values.h) ---------------------------------- */

    /** Seconds between history points. The ring is a fixed 288 points, so this is
     *  what sets the span: 300 s covers 24 h, 600 s covers 48 h. */
    uint32_t hist_period_s;

    /* --- the panel (§9.11) --------------------------------------------------- */

    bool    lcd_on;
    int8_t  lcd_screen;   /**< <0 = auto-cycle */
    uint8_t lcd_contrast;

    /* --- the radio (§8.5) ---------------------------------------------------- */

    uint8_t  ble_pair_bonded; /**< 0 = open, 1 = bonded */
    uint32_t ble_passkey;     /**< 0xFFFFFFFF = a fresh random one per pairing */
} cfg_t;

/** The live settings. Read freely; change through here and then commit. */
cfg_t *config(void);

/** Points this module at the sensors it configures. Called once, after bring-up and
 *  before config_load(). */
void config_bind(struct app_ctx_s *ctx);

/** Fills the struct with the compiled-in defaults. Called before any load, so a
 *  missing key keeps a sane value rather than a zero. */
void config_defaults(void);

/**
 * Loads from NVS (migrating a legacy `cal` namespace if that is all there is) and
 * pushes everything into the drivers. Call once, after the sensors are up.
 *
 * @return ESP_ERR_NVS_NOT_FOUND when nothing has ever been saved, which is the normal
 *         first-boot outcome and not an error.
 */
esp_err_t config_load(void);

/** Writes the whole struct to NVS. */
esp_err_t config_commit(void);

/** Pushes the current struct into the sensors, gauge, panel and radio without
 *  touching NVS. Used after a setter, and by config_load(). */
esp_err_t config_apply(void);

/** Erases the stored settings. Live values are untouched. */
esp_err_t config_forget(void);

/** True when settings have been stored. */
bool config_exists(void);

/** Reads the calibration-relevant values back OUT of the drivers into cfg_t, then
 *  commits. The calibration routines solve their results into the INA219 handles
 *  directly -- they measure against the part -- so this is how those results become
 *  configuration rather than a value that dies at the next reboot. */
esp_err_t config_capture_and_commit(void);
