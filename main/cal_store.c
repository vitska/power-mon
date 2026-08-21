/*
 * cal_store.c — see cal_store.h.
 */

#include "cal_store.h"

#include <string.h>

#include "esp_log.h"
#include "ina219.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sensors.h"

static const char *TAG = "cal_store";

#define NS "cal"

/* Bump only for a layout change that an older reader would misinterpret. Adding a
 * key does not qualify: a missing key reads as absent and keeps its live default. */
#define CAL_VER 1

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
#define K_VPATH  "vpath"
#define K_PROF   "prof"
#define K_CADC   "cadc"

bool cal_store_exists(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    uint8_t         ver = 0;
    const esp_err_t err = nvs_get_u8(h, K_VER, &ver);
    nvs_close(h);
    return err == ESP_OK && ver == CAL_VER;
}

esp_err_t cal_store_save(app_ctx_t *ctx)
{
    if (!ctx->sensors) {
        return ESP_ERR_INVALID_STATE;
    }
    ina219_handle_t cd = sensors_current_dev(ctx->sensors);
    ina219_handle_t vd = sensors_voltage_dev(ctx->sensors);
    if (!cd || !vd) {
        return ESP_ERR_INVALID_STATE;
    }

    nvs_handle_t h;
    esp_err_t    err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    /* Write the version LAST, so a power cut mid-save leaves a store that
     * cal_store_exists() rejects rather than one that loads half a calibration. */
    err = nvs_erase_all(h);
    if (err == ESP_OK) err = nvs_set_u32(h, K_RSHUNT, ina219_get_shunt_uohm(cd));
    if (err == ESP_OK) err = nvs_set_i32(h, K_IOFF,   ina219_get_offset_ua(cd));
    if (err == ESP_OK) err = nvs_set_u32(h, K_IGAIN,  ina219_get_gain_ppm(cd));
    if (err == ESP_OK) err = nvs_set_u8 (h, K_SIGN,   ina219_get_invert_sign(cd) ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_i32(h, K_VOFF,   ina219_get_vbus_offset_uv(vd));
    if (err == ESP_OK) err = nvs_set_u32(h, K_VGAIN,  ina219_get_vbus_gain_ppm(vd));
    if (err == ESP_OK) err = nvs_set_u32(h, K_VDIV,   ina219_get_vbus_divider_q16(vd));
    if (err == ESP_OK) err = nvs_set_u8 (h, K_MODE,   (uint8_t)sensors_get_mode(ctx->sensors));
    /* The range ceiling and the bus-comp mode are not calibration constants, but they
     * change how a raw register becomes a reading just as much as gain does -- and a
     * ceiling that silently drops from /8 to /4 on reboot turns a working measurement
     * into a saturated one. */
    if (err == ESP_OK) err = nvs_set_u8 (h, K_PGAMAX, (uint8_t)ina219_get_pga_max(cd));
    if (err == ESP_OK) err = nvs_set_u8 (h, K_VBCOMP, (uint8_t)ina219_get_vbus_comp(vd));
    if (err == ESP_OK) err = nvs_set_u32(h, K_VPATH,
                                        sensors_get_r_vpath_uohm(ctx->sensors));
    /*
     * The sampling profile and its averaging belong here for the same reason the range
     * ceiling does: they decide what a reading IS, not merely how fast it arrives. A
     * board configured for 10 Hz telemetry that silently reverts to 7.3 Hz on the next
     * power cycle looks like the firmware dropping samples.
     */
    if (err == ESP_OK) err = nvs_set_u8 (h, K_PROF, (uint8_t)ina219_get_profile(cd));
    if (err == ESP_OK) err = nvs_set_u8 (h, K_CADC,
                                        (uint8_t)ina219_get_continuous_adc(cd));
    if (err == ESP_OK) err = nvs_set_u8 (h, K_VER,    CAL_VER);
    if (err == ESP_OK) err = nvs_commit(h);

    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t cal_store_load(app_ctx_t *ctx)
{
    if (!ctx->sensors) {
        return ESP_ERR_INVALID_STATE;
    }

    nvs_handle_t h;
    esp_err_t    err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return ESP_ERR_NVS_NOT_FOUND; /* namespace absent: nothing saved yet */
    }

    uint8_t ver = 0;
    if (nvs_get_u8(h, K_VER, &ver) != ESP_OK || ver != CAL_VER) {
        nvs_close(h);
        ESP_LOGW(TAG, "stored calibration is incomplete or version %u -- ignoring", ver);
        return ESP_ERR_NVS_NOT_FOUND;
    }

    /*
     * Install mode first: roles must resolve before there is a current device and a
     * voltage device to apply the rest to. A mode that cannot be assigned (a sensor
     * has gone missing since the save) is a warning, not a failure -- the remaining
     * values still apply to whatever did answer.
     */
    uint8_t mode = 0;
    if (nvs_get_u8(h, K_MODE, &mode) == ESP_OK && mode != SENSORS_MODE_AUTO) {
        const esp_err_t merr = sensors_set_mode(ctx->sensors, (sensors_mode_t)mode);
        if (merr != ESP_OK) {
            ESP_LOGW(TAG, "stored mode %u could not be applied: %s", mode,
                     esp_err_to_name(merr));
        }
    }

    ina219_handle_t cd = sensors_current_dev(ctx->sensors);
    ina219_handle_t vd = sensors_voltage_dev(ctx->sensors);
    if (!cd || !vd) {
        nvs_close(h);
        ESP_LOGW(TAG, "roles unresolved -- stored calibration not applied");
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t u32;
    int32_t  i32;
    uint8_t  u8;

    if (nvs_get_u32(h, K_RSHUNT, &u32) == ESP_OK) ina219_set_shunt_uohm(cd, u32);
    if (nvs_get_i32(h, K_IOFF,   &i32) == ESP_OK) ina219_set_offset_ua(cd, i32);
    if (nvs_get_u32(h, K_IGAIN,  &u32) == ESP_OK) ina219_set_gain_ppm(cd, u32);
    if (nvs_get_u8 (h, K_SIGN,   &u8)  == ESP_OK) ina219_set_invert_sign(cd, u8 != 0);
    if (nvs_get_i32(h, K_VOFF,   &i32) == ESP_OK) ina219_set_vbus_offset_uv(vd, i32);
    if (nvs_get_u32(h, K_VGAIN,  &u32) == ESP_OK) ina219_set_vbus_gain_ppm(vd, u32);
    if (nvs_get_u32(h, K_VDIV,   &u32) == ESP_OK) ina219_set_vbus_divider_q16(vd, u32);
    if (nvs_get_u8 (h, K_PGAMAX, &u8)  == ESP_OK) ina219_set_pga_max(cd, (ina219_pga_t)u8);
    if (nvs_get_u8 (h, K_VBCOMP, &u8)  == ESP_OK) ina219_set_vbus_comp(vd, (ina219_vbus_comp_t)u8);
    if (nvs_get_u32(h, K_VPATH,  &u32) == ESP_OK) sensors_set_r_vpath_uohm(ctx->sensors, u32);

    /* Averaging before profile: set_profile() re-applies the config register, so doing
     * it second means the restored averaging actually reaches the part. */
    if (nvs_get_u8(h, K_CADC, &u8) == ESP_OK) {
        ina219_set_continuous_adc(cd, (ina219_adc_t)u8);
        if (vd != cd) {
            ina219_set_continuous_adc(vd, (ina219_adc_t)u8);
        }
    }
    if (nvs_get_u8(h, K_PROF, &u8) == ESP_OK) {
        ina219_set_profile(cd, (ina219_profile_t)u8);
        if (vd != cd) {
            ina219_set_profile(vd, (ina219_profile_t)u8);
        }
    }

    nvs_close(h);

    ESP_LOGI(TAG, "restored: shunt %lu uOhm, i offset %ld uA gain %lu ppm, "
                  "v offset %ld uV gain %lu ppm, ceiling %s, %s",
             (unsigned long)ina219_get_shunt_uohm(cd),
             (long)ina219_get_offset_ua(cd),
             (unsigned long)ina219_get_gain_ppm(cd),
             (long)ina219_get_vbus_offset_uv(vd),
             (unsigned long)ina219_get_vbus_gain_ppm(vd),
             ina219_pga_str(ina219_get_pga_max(cd)),
             ina219_get_profile(cd) == INA219_PROFILE_TRIGGERED ? "triggered"
             : (ina219_get_continuous_adc(cd) == INA219_ADC_128AVG ? "continuous 128x"
                                                                  : "fast 64x"));
    return ESP_OK;
}

esp_err_t cal_store_forget(void)
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
