/*
 * config.c — see config.h.
 */

#include "config.h"

#include <stddef.h>
#include <string.h>

#include "app_ctx.h"
#include "esp_log.h"
#include "history_values.h"
#include "lcd.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#if CONFIG_BATMON_BLE_ENABLE
#include "ble.h"
#endif

static const char *TAG = "config";

/* Whether telemetry is streaming at boot. A Kconfig choice rather than a hardcoded
 * default because a bench build wants it on and a deployed one does not; once a user
 * has set it the stored value wins over both. */
#ifdef CONFIG_BATMON_STREAM_ON_BOOT
#define BATMON_STREAM_ON_BOOT true
#else
#define BATMON_STREAM_ON_BOOT false
#endif

#define NS     "cfg"
#define NS_OLD "cal" /* M1/M2 calibration-only store, migrated once */

/* Bump only for a layout change an older reader would misinterpret. Adding a key does
 * not qualify: a missing key reads as absent and keeps its compiled-in default. */
#define CFG_VER 1

/* Keys are short because NVS keys are capped at 15 characters, and grouped by prefix
 * so a dump is readable. The calibration ones keep their M1 spelling so the migration
 * is a straight copy rather than a translation table. */
#define K_VER    "ver"
#define K_RSHUNT "rshunt"
#define K_IOFF   "ioff"
#define K_IGAIN  "igain"
#define K_SIGN   "sign"
#define K_VOFF   "voff"
#define K_VGAIN  "vgain"
#define K_VDIV   "vdiv"
#define K_MODE   "mode"
#define K_PGAMAX "pgamax"
#define K_VBCOMP "vbcomp"
#define K_PROF   "prof"
#define K_CADC   "cadc"
#define K_GAUGE  "gauge"
#define K_STRON  "stron"
#define K_STRCSV "strcsv"
#define K_RFAST  "rfast"
#define K_RCALC  "rcalc"
#define K_RDIAG  "rdiag"
#define K_RENV   "renv"
#define K_HPER   "hper"
#define K_LCDON  "lcdon"
#define K_LCDSCR "lcdscr"
#define K_LCDCON "lcdcon"
#define K_BLEPR  "blepair"
#define K_BLEPK  "blepk"

static cfg_t s_cfg;

/* The sensors the settings are pushed into. Set once at startup by config_bind(). */
static app_ctx_t *s_ctx;

cfg_t *config(void)
{
    return &s_cfg;
}

void config_bind(struct app_ctx_s *ctx)
{
    s_ctx = ctx;
}

void config_defaults(void)
{
    s_cfg = (cfg_t){
        .shunt_uohm    = CONFIG_BATMON_SHUNT_UOHM,
        .i_offset_ua   = 0,
        .i_gain_ppm    = 1000000u,
        .invert_sign   = false,
        .v_offset_uv   = 0,
        .v_gain_ppm    = 1000000u,
        .v_divider_q16 = 65536u,
        .vbus_comp     = INA219_VBUS_COMP_ADD_SHUNT,
        .pga_max       = INA219_PGA_4,
        .install_mode  = SENSORS_MODE_AUTO,
        .profile       = INA219_PROFILE_CONTINUOUS,
        .cont_adc      = INA219_ADC_128AVG,

        .gauge = FG_CONFIG_LEAD_ACID_12V_44AH(),

        .stream_enabled = BATMON_STREAM_ON_BOOT,
        .stream_csv     = false,
        .rate_fast_ms   = CONFIG_BATMON_STREAM_FAST_MS,
        .rate_calc_ms   = CONFIG_BATMON_STREAM_CALC_MS,
        .rate_diag_ms   = CONFIG_BATMON_STREAM_DIAG_MS,
        .rate_env_ms    = CONFIG_BATMON_STREAM_ENV_MS,

        .hist_period_s = SOC_HIST_PERIOD_DEFAULT_S,

        .lcd_on       = true,
        .lcd_screen   = 0,
        .lcd_contrast = 0x40,

        .ble_pair_bonded = 0,
        .ble_passkey     = 0xFFFFFFFFu,
    };
}

/* --- pushing settings into the things they configure -------------------------- */

esp_err_t config_apply(void)
{
    /*
     * The gauge first, and unconditionally: it owns no hardware, validates the shape
     * of what it is given, and a rejected gauge config should not stop the sensor
     * values from being applied.
     */
    const esp_err_t gerr = fg_set_config(&s_cfg.gauge);
    if (gerr != ESP_OK) {
        ESP_LOGW(TAG, "gauge config rejected: %s -- keeping the previous one",
                 esp_err_to_name(gerr));
    }

    /* Pushed, not compared: the history module clamps it and does nothing when the
     * value is the one it already runs on, which is the usual case at boot. */
    soc_history_set_period_s(s_cfg.hist_period_s);
    s_cfg.hist_period_s = soc_history_period_s(); /* keep cfg honest about the clamp */

#if CONFIG_BATMON_DISPLAY_ENABLE
    if (lcd_present()) {
        lcd_enable(s_cfg.lcd_on);
        lcd_set_screen(s_cfg.lcd_screen);
        lcd_set_contrast(s_cfg.lcd_contrast);
    }
#endif

#if CONFIG_BATMON_BLE_ENABLE
    ble_set_sec_mode(s_cfg.ble_pair_bonded ? BLE_SEC_BONDED : BLE_SEC_OPEN);
    ble_set_passkey(s_cfg.ble_passkey);
#endif

    if (!s_ctx || !s_ctx->sensors) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Install mode first: roles must resolve before there is a current device and a
     * voltage device to apply the rest to. A mode that cannot be assigned -- a sensor
     * has gone missing since the save -- is a warning, not a failure, and the
     * remaining values still apply to whatever did answer.
     */
    if (s_cfg.install_mode != SENSORS_MODE_AUTO) {
        const esp_err_t merr = sensors_set_mode(s_ctx->sensors, s_cfg.install_mode);
        if (merr != ESP_OK) {
            ESP_LOGW(TAG, "mode %u could not be applied: %s",
                     (unsigned)s_cfg.install_mode, esp_err_to_name(merr));
        }
    }

    ina219_handle_t cd = sensors_current_dev(s_ctx->sensors);
    ina219_handle_t vd = sensors_voltage_dev(s_ctx->sensors);
    if (!cd || !vd) {
        ESP_LOGW(TAG, "roles unresolved -- sensor settings not applied");
        return ESP_ERR_INVALID_STATE;
    }

    ina219_set_shunt_uohm(cd, s_cfg.shunt_uohm);
    ina219_set_offset_ua(cd, s_cfg.i_offset_ua);
    ina219_set_gain_ppm(cd, s_cfg.i_gain_ppm);
    ina219_set_invert_sign(cd, s_cfg.invert_sign);
    ina219_set_vbus_offset_uv(vd, s_cfg.v_offset_uv);
    ina219_set_vbus_gain_ppm(vd, s_cfg.v_gain_ppm);
    ina219_set_vbus_divider_q16(vd, s_cfg.v_divider_q16);
    ina219_set_pga_max(cd, s_cfg.pga_max);
    ina219_set_vbus_comp(vd, s_cfg.vbus_comp);

    /* Averaging before profile: set_profile() re-applies the config register, so
     * doing it second is what makes the restored averaging reach the part. */
    ina219_set_continuous_adc(cd, s_cfg.cont_adc);
    if (vd != cd) {
        ina219_set_continuous_adc(vd, s_cfg.cont_adc);
    }
    ina219_set_profile(cd, s_cfg.profile);
    if (vd != cd) {
        ina219_set_profile(vd, s_cfg.profile);
    }
    return ESP_OK;
}

esp_err_t config_capture_and_commit(void)
{
    if (!s_ctx || !s_ctx->sensors) {
        return ESP_ERR_INVALID_STATE;
    }
    ina219_handle_t cd = sensors_current_dev(s_ctx->sensors);
    ina219_handle_t vd = sensors_voltage_dev(s_ctx->sensors);
    if (!cd || !vd) {
        return ESP_ERR_INVALID_STATE;
    }

    s_cfg.shunt_uohm    = ina219_get_shunt_uohm(cd);
    s_cfg.i_offset_ua   = ina219_get_offset_ua(cd);
    s_cfg.i_gain_ppm    = ina219_get_gain_ppm(cd);
    s_cfg.invert_sign   = ina219_get_invert_sign(cd);
    s_cfg.v_offset_uv   = ina219_get_vbus_offset_uv(vd);
    s_cfg.v_gain_ppm    = ina219_get_vbus_gain_ppm(vd);
    s_cfg.v_divider_q16 = ina219_get_vbus_divider_q16(vd);
    s_cfg.vbus_comp     = ina219_get_vbus_comp(vd);
    s_cfg.pga_max       = ina219_get_pga_max(cd);
    s_cfg.install_mode  = sensors_get_mode(s_ctx->sensors);
    s_cfg.profile       = ina219_get_profile(cd);
    s_cfg.cont_adc      = ina219_get_continuous_adc(cd);
    s_cfg.gauge         = fg_get_config();
    return config_commit();
}

/* --- persistence -------------------------------------------------------------- */

bool config_exists(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    uint8_t         ver = 0;
    const esp_err_t err = nvs_get_u8(h, K_VER, &ver);
    nvs_close(h);
    return err == ESP_OK && ver == CFG_VER;
}

esp_err_t config_commit(void)
{
    nvs_handle_t h;
    esp_err_t    err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    /* Erase first so a key removed in a later version cannot linger and be read by
     * an older build. The version key is written LAST, so an interrupted commit
     * leaves a store that reads as absent rather than as half-written. */
    err = nvs_erase_all(h);

    if (err == ESP_OK) err = nvs_set_u32(h, K_RSHUNT, s_cfg.shunt_uohm);
    if (err == ESP_OK) err = nvs_set_i32(h, K_IOFF,   s_cfg.i_offset_ua);
    if (err == ESP_OK) err = nvs_set_u32(h, K_IGAIN,  s_cfg.i_gain_ppm);
    if (err == ESP_OK) err = nvs_set_u8 (h, K_SIGN,   s_cfg.invert_sign ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_i32(h, K_VOFF,   s_cfg.v_offset_uv);
    if (err == ESP_OK) err = nvs_set_u32(h, K_VGAIN,  s_cfg.v_gain_ppm);
    if (err == ESP_OK) err = nvs_set_u32(h, K_VDIV,   s_cfg.v_divider_q16);
    if (err == ESP_OK) err = nvs_set_u8 (h, K_VBCOMP, (uint8_t)s_cfg.vbus_comp);
    if (err == ESP_OK) err = nvs_set_u8 (h, K_PGAMAX, (uint8_t)s_cfg.pga_max);
    if (err == ESP_OK) err = nvs_set_u8 (h, K_MODE,   (uint8_t)s_cfg.install_mode);
    if (err == ESP_OK) err = nvs_set_u8 (h, K_PROF,   (uint8_t)s_cfg.profile);
    if (err == ESP_OK) err = nvs_set_u8 (h, K_CADC,   (uint8_t)s_cfg.cont_adc);

    /* The gauge config goes as a blob: it is the fuel gauge's own struct, it validates
     * it as a whole, and splitting it into fifteen keys here would put its layout in
     * two places. */
    if (err == ESP_OK) err = nvs_set_blob(h, K_GAUGE, &s_cfg.gauge, sizeof(s_cfg.gauge));

    if (err == ESP_OK) err = nvs_set_u8 (h, K_STRON,  s_cfg.stream_enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8 (h, K_STRCSV, s_cfg.stream_csv ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u32(h, K_RFAST,  s_cfg.rate_fast_ms);
    if (err == ESP_OK) err = nvs_set_u32(h, K_RCALC,  s_cfg.rate_calc_ms);
    if (err == ESP_OK) err = nvs_set_u32(h, K_RDIAG,  s_cfg.rate_diag_ms);
    if (err == ESP_OK) err = nvs_set_u32(h, K_RENV,   s_cfg.rate_env_ms);
    if (err == ESP_OK) err = nvs_set_u32(h, K_HPER,   s_cfg.hist_period_s);

    if (err == ESP_OK) err = nvs_set_u8 (h, K_LCDON,  s_cfg.lcd_on ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_i8 (h, K_LCDSCR, s_cfg.lcd_screen);
    if (err == ESP_OK) err = nvs_set_u8 (h, K_LCDCON, s_cfg.lcd_contrast);

    if (err == ESP_OK) err = nvs_set_u8 (h, K_BLEPR,  s_cfg.ble_pair_bonded);
    if (err == ESP_OK) err = nvs_set_u32(h, K_BLEPK,  s_cfg.ble_passkey);

    if (err == ESP_OK) err = nvs_set_u8 (h, K_VER,    CFG_VER);
    if (err == ESP_OK) err = nvs_commit(h);

    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "commit failed: %s", esp_err_to_name(err));
    }
    return err;
}

/* Reads whatever is present, leaving defaults in place for absent keys. */
static void read_all(nvs_handle_t h)
{
    uint32_t u32;
    int32_t  i32;
    uint8_t  u8;
    int8_t   i8;
    size_t   len;

    if (nvs_get_u32(h, K_RSHUNT, &u32) == ESP_OK) s_cfg.shunt_uohm = u32;
    if (nvs_get_i32(h, K_IOFF,   &i32) == ESP_OK) s_cfg.i_offset_ua = i32;
    if (nvs_get_u32(h, K_IGAIN,  &u32) == ESP_OK) s_cfg.i_gain_ppm = u32;
    if (nvs_get_u8 (h, K_SIGN,   &u8)  == ESP_OK) s_cfg.invert_sign = (u8 != 0);
    if (nvs_get_i32(h, K_VOFF,   &i32) == ESP_OK) s_cfg.v_offset_uv = i32;
    if (nvs_get_u32(h, K_VGAIN,  &u32) == ESP_OK) s_cfg.v_gain_ppm = u32;
    if (nvs_get_u32(h, K_VDIV,   &u32) == ESP_OK) s_cfg.v_divider_q16 = u32;
    if (nvs_get_u8 (h, K_VBCOMP, &u8)  == ESP_OK) s_cfg.vbus_comp = (ina219_vbus_comp_t)u8;
    if (nvs_get_u8 (h, K_PGAMAX, &u8)  == ESP_OK) s_cfg.pga_max = (ina219_pga_t)u8;
    if (nvs_get_u8 (h, K_MODE,   &u8)  == ESP_OK) s_cfg.install_mode = (sensors_mode_t)u8;
    if (nvs_get_u8 (h, K_PROF,   &u8)  == ESP_OK) s_cfg.profile = (ina219_profile_t)u8;
    if (nvs_get_u8 (h, K_CADC,   &u8)  == ESP_OK) s_cfg.cont_adc = (ina219_adc_t)u8;

    /*
     * The gauge blob is fg_config_t as it was when saved. Fields are only ever
     * appended to it, so ANY shorter blob is an older layout: take what it has, and
     * let the defaults stand for the rest -- `g` starts as the defaults, and the read
     * overwrites only the bytes that were stored.
     *
     * This used to name the two lengths it would accept, which meant every appended
     * field silently reset every gauge setting on the board it was added to, until
     * someone remembered to add the old size to the list. The rule the struct actually
     * guarantees is "shorter is older", so that is what is checked.
     */
    len = sizeof(s_cfg.gauge);
    fg_config_t g = s_cfg.gauge;
    if (nvs_get_blob(h, K_GAUGE, &g, &len) == ESP_OK && len <= sizeof(g)) {
        s_cfg.gauge = g;
    }

    if (nvs_get_u8 (h, K_STRON,  &u8)  == ESP_OK) s_cfg.stream_enabled = (u8 != 0);
    if (nvs_get_u8 (h, K_STRCSV, &u8)  == ESP_OK) s_cfg.stream_csv = (u8 != 0);
    if (nvs_get_u32(h, K_RFAST,  &u32) == ESP_OK) s_cfg.rate_fast_ms = u32;
    if (nvs_get_u32(h, K_RCALC,  &u32) == ESP_OK) s_cfg.rate_calc_ms = u32;
    if (nvs_get_u32(h, K_RDIAG,  &u32) == ESP_OK) s_cfg.rate_diag_ms = u32;
    if (nvs_get_u32(h, K_RENV,   &u32) == ESP_OK) s_cfg.rate_env_ms = u32;
    if (nvs_get_u32(h, K_HPER,   &u32) == ESP_OK) s_cfg.hist_period_s = u32;

    if (nvs_get_u8 (h, K_LCDON,  &u8)  == ESP_OK) s_cfg.lcd_on = (u8 != 0);
    if (nvs_get_i8 (h, K_LCDSCR, &i8)  == ESP_OK) s_cfg.lcd_screen = i8;
    if (nvs_get_u8 (h, K_LCDCON, &u8)  == ESP_OK) s_cfg.lcd_contrast = u8;

    if (nvs_get_u8 (h, K_BLEPR,  &u8)  == ESP_OK) s_cfg.ble_pair_bonded = u8;
    if (nvs_get_u32(h, K_BLEPK,  &u32) == ESP_OK) s_cfg.ble_passkey = u32;
}

esp_err_t config_load(void)
{
    nvs_handle_t h;
    bool         found = false;

    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t ver = 0;
        if (nvs_get_u8(h, K_VER, &ver) == ESP_OK && ver == CFG_VER) {
            read_all(h);
            found = true;
        } else {
            ESP_LOGW(TAG, "stored config is incomplete or version %u -- ignoring", ver);
        }
        nvs_close(h);
    }

    /*
     * MIGRATION. M1/M2 kept the calibration subset in namespace `cal`. Those numbers
     * were measured against physical hardware with a meter in hand, so they are read
     * across and rewritten into `cfg` rather than lost to a refactor. The old
     * namespace is left in place: if this build is rolled back, the calibration is
     * still there.
     */
    if (!found && nvs_open(NS_OLD, NVS_READONLY, &h) == ESP_OK) {
        uint8_t ver = 0;
        if (nvs_get_u8(h, K_VER, &ver) == ESP_OK && ver == 1) {
            read_all(h); /* the legacy keys are a subset, spelled identically */
            found = true;
            ESP_LOGW(TAG, "migrated calibration from the legacy '%s' namespace", NS_OLD);
        }
        nvs_close(h);
        if (found) {
            config_commit();
        }
    }

    const esp_err_t aerr = config_apply();

    if (!found) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    ESP_LOGI(TAG, "restored: shunt %lu uOhm, i gain %lu ppm, v gain %lu ppm, "
                  "cap %lu uAh",
             (unsigned long)s_cfg.shunt_uohm, (unsigned long)s_cfg.i_gain_ppm,
             (unsigned long)s_cfg.v_gain_ppm,
             (unsigned long)s_cfg.gauge.design_capacity_uah);
    return aerr;
}

esp_err_t config_forget(void)
{
    nvs_handle_t h;
    esp_err_t    err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_all(h);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}
