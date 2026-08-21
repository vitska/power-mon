/*
 * ina219.c — see include/ina219.h and DESIGN.md §4.
 */

#include "ina219.h"

#include <stdlib.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ina219";

/* --- registers -------------------------------------------------------------- */

#define REG_CONFIG      0x00
#define REG_SHUNT       0x01
#define REG_BUS         0x02
#define REG_POWER       0x03
#define REG_CURRENT     0x04
#define REG_CALIBRATION 0x05

#define CONFIG_RESET        0x8000u
#define CONFIG_BRNG_32V     (1u << 13)
#define CONFIG_PG_SHIFT     11
#define CONFIG_BADC_SHIFT   7
#define CONFIG_SADC_SHIFT   3

#define MODE_POWERDOWN            0x0
#define MODE_SHUNT_BUS_TRIGGERED  0x3
#define MODE_SHUNT_BUS_CONTINUOUS 0x7

/* Datasheet power-on default of the configuration register. Used as an identity
 * check: the INA219 has no WHO_AM_I, so this is the best evidence available that
 * the thing answering at this address is the part we think it is. */
#define CONFIG_POR_DEFAULT 0x399Fu

/* Bus voltage register layout: [15:3] value, [2] reserved, [1] CNVR, [0] OVF. */
#define BUS_CNVR (1u << 1)
#define BUS_OVF  (1u << 0)

/* Shunt LSB is 10 uV at every PGA setting. */
#define SHUNT_LSB_UV 10

/*
 * Smallest accepted shunt, micro-ohms.
 *
 * The sample struct reports current as int32 microamperes, which saturates at
 * ~2147 A. Full scale is 320 mV / R, so a shunt below ~149 uOhm can produce a
 * reading that does not fit. 1 mOhm (320 A full scale) is already past anything
 * this design targets, so the limit costs nothing and removes the failure mode
 * rather than documenting it.
 */
#define R_SHUNT_MIN_UOHM 1000

/* Bus LSB is 4 mV. */
#define BUS_LSB_UV 4000

/* --- autorange thresholds (DESIGN.md §4.2) ---------------------------------- */

#define RANGE_UP_NUM   90 /* step coarser above 90% of full scale  */
#define RANGE_UP_DEN   100
#define RANGE_DOWN_NUM 40 /* step finer below 40% of the next range */
#define RANGE_DOWN_DEN 100
#define RANGE_DOWN_DWELL 8 /* consecutive quiet samples before stepping finer */

struct ina219_dev_t {
    i2c_master_dev_handle_t dev;
    ina219_config_t         cfg;
    ina219_profile_t        profile;
    ina219_pga_t            pga;

    uint16_t config_shadow;
    uint8_t  quiet_run;      /* consecutive samples below the step-finer threshold */
    bool     range_changed;  /* discard the next conversion */
    int64_t  trigger_t_us;
};

/* --- register access -------------------------------------------------------- */

static esp_err_t reg_write(ina219_handle_t h, uint8_t reg, uint16_t val)
{
    const uint8_t buf[3] = {reg, (uint8_t)(val >> 8), (uint8_t)(val & 0xFF)};
    return i2c_master_transmit(h->dev, buf, sizeof(buf), 100);
}

static esp_err_t reg_read(ina219_handle_t h, uint8_t reg, uint16_t *val)
{
    uint8_t rx[2];
    esp_err_t err = i2c_master_transmit_receive(h->dev, &reg, 1, rx, sizeof(rx), 100);
    if (err != ESP_OK) {
        return err;
    }
    *val = ((uint16_t)rx[0] << 8) | rx[1];
    return ESP_OK;
}

/* --- helpers ---------------------------------------------------------------- */

int32_t ina219_pga_fullscale_uv(ina219_pga_t pga)
{
    return 40000 << (int)pga; /* 40 / 80 / 160 / 320 mV */
}

const char *ina219_pga_str(ina219_pga_t pga)
{
    switch (pga) {
    case INA219_PGA_1: return "/1 (+/-40mV)";
    case INA219_PGA_2: return "/2 (+/-80mV)";
    case INA219_PGA_4: return "/4 (+/-160mV)";
    case INA219_PGA_8: return "/8 (+/-320mV)";
    default:           return "?";
    }
}

/* Full-scale expressed in raw counts, for range comparisons without a divide. */
static int32_t pga_fullscale_counts(ina219_pga_t pga)
{
    return 4000 << (int)pga; /* 40 mV / 10 uV = 4000 */
}

static uint32_t adc_time_us(ina219_adc_t adc)
{
    switch (adc) {
    case INA219_ADC_9BIT:   return 84;
    case INA219_ADC_10BIT:  return 148;
    case INA219_ADC_11BIT:  return 276;
    case INA219_ADC_12BIT:  return 532;
    case INA219_ADC_2AVG:   return 1060;
    case INA219_ADC_4AVG:   return 2130;
    case INA219_ADC_8AVG:   return 4260;
    case INA219_ADC_16AVG:  return 8510;
    case INA219_ADC_32AVG:  return 17020;
    case INA219_ADC_64AVG:  return 34050;
    case INA219_ADC_128AVG: return 68100;
    default:                return 68100;
    }
}

static uint16_t build_config(const struct ina219_dev_t *h)
{
    ina219_adc_t badc, sadc;
    uint16_t     mode;

    if (h->profile == INA219_PROFILE_CONTINUOUS) {
        badc = h->cfg.bus_adc_continuous;
        sadc = h->cfg.shunt_adc_continuous;
        mode = MODE_SHUNT_BUS_CONTINUOUS;
    } else {
        badc = h->cfg.bus_adc_triggered;
        sadc = h->cfg.shunt_adc_triggered;
        /* Triggered mode returns to power-down after each conversion by itself;
         * that auto-power-down is the entire point of the low-power profile. */
        mode = MODE_SHUNT_BUS_TRIGGERED;
    }

    return CONFIG_BRNG_32V
         | ((uint16_t)h->pga << CONFIG_PG_SHIFT)
         | ((uint16_t)badc << CONFIG_BADC_SHIFT)
         | ((uint16_t)sadc << CONFIG_SADC_SHIFT)
         | mode;
}

static esp_err_t apply_config(ina219_handle_t h)
{
    const uint16_t cfg = build_config(h);
    esp_err_t err = reg_write(h, REG_CONFIG, cfg);
    if (err == ESP_OK) {
        h->config_shadow = cfg;
        h->trigger_t_us  = esp_timer_get_time();
    }
    return err;
}

uint32_t ina219_conversion_time_us(ina219_handle_t h)
{
    if (h->profile == INA219_PROFILE_CONTINUOUS) {
        return adc_time_us(h->cfg.bus_adc_continuous)
             + adc_time_us(h->cfg.shunt_adc_continuous);
    }
    return adc_time_us(h->cfg.bus_adc_triggered)
         + adc_time_us(h->cfg.shunt_adc_triggered);
}

/* --- lifecycle -------------------------------------------------------------- */

esp_err_t ina219_init(i2c_master_bus_handle_t bus,
                      const ina219_config_t  *cfg,
                      ina219_handle_t        *out)
{
    ESP_RETURN_ON_FALSE(bus && cfg && out, ESP_ERR_INVALID_ARG, TAG, "null arg");
    ESP_RETURN_ON_FALSE(cfg->r_shunt_uohm >= R_SHUNT_MIN_UOHM, ESP_ERR_INVALID_ARG,
                        TAG, "shunt must be >= %d uOhm", R_SHUNT_MIN_UOHM);

    if (i2c_master_probe(bus, cfg->i2c_addr, 100) != ESP_OK) {
        ESP_LOGE(TAG, "no device answers at 0x%02X", cfg->i2c_addr);
        return ESP_ERR_NOT_FOUND;
    }

    ina219_handle_t h = calloc(1, sizeof(struct ina219_dev_t));
    ESP_RETURN_ON_FALSE(h, ESP_ERR_NO_MEM, TAG, "oom");

    /* ESP_GOTO_ON_ERROR assigns to a variable that must be named `ret`. */
    esp_err_t ret = ESP_OK;

    h->cfg     = *cfg;
    h->pga     = cfg->pga;
    h->profile = INA219_PROFILE_CONTINUOUS;

    /* A range above the ceiling is a configuration error that would only show up as
     * a damaged part on a low-side build (DESIGN.md 2.9.2), so clamp it loudly. */
    if (h->cfg.pga_max > INA219_PGA_8) {
        h->cfg.pga_max = INA219_PGA_8;
    }
    if (h->pga > h->cfg.pga_max) {
        ESP_LOGW(TAG, "initial PGA %s exceeds the ceiling %s; clamping",
                 ina219_pga_str(h->pga), ina219_pga_str(h->cfg.pga_max));
        h->pga = h->cfg.pga_max;
    }

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = cfg->i2c_addr,
        .scl_speed_hz    = cfg->scl_speed_hz ? cfg->scl_speed_hz : 400000,
    };
    ESP_GOTO_ON_ERROR(i2c_master_bus_add_device(bus, &dev_cfg, &h->dev),
                      fail, TAG, "add device");

    /* Reset, then verify the part identifies as an INA219 by its POR config value. */
    ESP_GOTO_ON_ERROR(reg_write(h, REG_CONFIG, CONFIG_RESET), fail, TAG, "reset");
    vTaskDelay(pdMS_TO_TICKS(5));

    uint16_t por = 0;
    ESP_GOTO_ON_ERROR(reg_read(h, REG_CONFIG, &por), fail, TAG, "read config");
    if (por != CONFIG_POR_DEFAULT) {
        ESP_LOGE(TAG,
                 "device at 0x%02X is not an INA219: config after reset = 0x%04X, "
                 "expected 0x%04X",
                 cfg->i2c_addr, por, CONFIG_POR_DEFAULT);
        ret = ESP_ERR_INVALID_RESPONSE;
        goto fail;
    }

    /* Calibration register stays 0: we never read the Current or Power registers,
     * so the on-chip multiplier has nothing to do. See the header. */
    ESP_GOTO_ON_ERROR(reg_write(h, REG_CALIBRATION, 0), fail, TAG, "cal");
    ESP_GOTO_ON_ERROR(apply_config(h), fail, TAG, "config");

    ESP_LOGI(TAG,
             "INA219 at 0x%02X: shunt %lu uOhm, PGA %s%s, conv %lu us",
             cfg->i2c_addr, (unsigned long)h->cfg.r_shunt_uohm,
             ina219_pga_str(h->pga), h->cfg.autorange ? " (auto)" : " (locked)",
             (unsigned long)ina219_conversion_time_us(h));

    *out = h;
    return ESP_OK;

fail:
    if (h->dev) {
        i2c_master_bus_rm_device(h->dev);
    }
    free(h);
    return ret;
}

esp_err_t ina219_deinit(ina219_handle_t h)
{
    if (!h) {
        return ESP_OK;
    }
    reg_write(h, REG_CONFIG, MODE_POWERDOWN);
    if (h->dev) {
        i2c_master_bus_rm_device(h->dev);
    }
    free(h);
    return ESP_OK;
}

esp_err_t ina219_set_profile(ina219_handle_t h, ina219_profile_t profile)
{
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "null");
    h->profile       = profile;
    h->range_changed = true; /* the in-flight conversion, if any, is now stale */
    return apply_config(h);
}

esp_err_t ina219_trigger(ina219_handle_t h)
{
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "null");
    if (h->profile == INA219_PROFILE_CONTINUOUS) {
        return ESP_OK;
    }
    /* In triggered mode, rewriting the configuration register starts a conversion. */
    return apply_config(h);
}

/* --- conversion ------------------------------------------------------------- */

/*
 * Raw -> SI, integer throughout (DESIGN.md §4.3).
 *
 * Widths: v_shunt_uv is at most +/-320000, so v_shunt_uv * 1000000 reaches 3.2e11
 * and must be computed in 64 bits. The result is bounded by the shunt value and
 * fits comfortably in int32 for any sane shunt (a 1 mOhm shunt at full scale is
 * 320 A = 3.2e8 uA, still inside int32).
 */
static void convert(ina219_handle_t h, int16_t raw_shunt, uint16_t raw_bus,
                    ina219_sample_t *s)
{
    const int32_t v_shunt_uv = (int32_t)raw_shunt * SHUNT_LSB_UV;

    int64_t i_ua = ((int64_t)v_shunt_uv * 1000000) / (int64_t)h->cfg.r_shunt_uohm;
    i_ua -= h->cfg.i_offset_ua;
    i_ua = (i_ua * (int64_t)h->cfg.i_gain_ppm) / 1000000;

    /* Sense leads swapped is a wiring choice, not a fault; correcting it here beats
     * asking someone to unsolder a shunt. Applied after gain so the calibration
     * constants stay in the physical sense of the hardware (DESIGN.md 2.9). */
    if (h->cfg.invert_sign) {
        i_ua = -i_ua;
    }

    /* Saturate rather than wrap. R_SHUNT_MIN_UOHM already makes this unreachable
     * for a legal configuration; it stays as a guard because silently wrapping a
     * current reading would be indistinguishable from a real reversal of sign. */
    if (i_ua > INT32_MAX)      i_ua = INT32_MAX;
    else if (i_ua < INT32_MIN) i_ua = INT32_MIN;

    /* Bus register: value is bits [15:3], LSB 4 mV. */
    const uint32_t v_raw_uv = (uint32_t)(raw_bus >> 3) * BUS_LSB_UV;
    int64_t v_uv = (int64_t)(((uint64_t)v_raw_uv * h->cfg.vbus_divider_q16) >> 16);

    /* Bus-voltage conversion curve: offset then gain, mirroring the current channel
     * so one mental model covers both. Applied here -- after the divider, before the
     * shunt compensation below -- because the divider and these two trims all
     * describe the input path, whereas the compensation is a reference shift that
     * must stay exact (see the comment on vbus_comp). */
    v_uv -= h->cfg.v_offset_uv;
    v_uv = (v_uv * (int64_t)h->cfg.v_gain_ppm) / 1000000;

    /*
     * Low-side bus-voltage correction (DESIGN.md 2.9.5).
     *
     * With the shunt in the negative lead, VBUS measures pack+ against the system
     * return while the true terminal voltage is against the battery negative. They
     * differ by exactly the shunt drop -- and it comes from the same conversion pair,
     * so this is an exact correction rather than an estimate.
     *
     * Uses the RAW shunt voltage, not the calibrated current: the physical drop is
     * what displaces the reference, regardless of what we think the offset is.
     *
     * The divider ratio deliberately does not apply -- it scales the bus input only;
     * the shunt drop is on the far side of it.
     */
    switch (h->cfg.vbus_comp) {
    case INA219_VBUS_COMP_ADD_SHUNT: v_uv += v_shunt_uv; break;
    case INA219_VBUS_COMP_SUB_SHUNT: v_uv -= v_shunt_uv; break;
    case INA219_VBUS_COMP_NONE:
    default:                                             break;
    }
    if (v_uv < 0) {
        v_uv = 0; /* only reachable near 0 V with a large drop; clamp, do not wrap */
    }

    s->v_shunt_uv = v_shunt_uv;
    s->raw_shunt  = raw_shunt;
    s->raw_bus    = raw_bus;
    s->pga        = h->pga;
    s->v_uv       = (uint32_t)v_uv;
    s->i_ua       = (int32_t)i_ua;
    s->p_uw       = (int32_t)(((int64_t)s->i_ua * (int64_t)s->v_uv) / 1000000);

    const int32_t fs = pga_fullscale_counts(h->pga);
    s->saturated = (raw_shunt >= fs - 1) || (raw_shunt <= -fs + 1);
}

/*
 * PGA auto-ranging (DESIGN.md §4.2).
 *
 * Returns true if the range changed, in which case the caller must discard the
 * current sample: it was converted on the old range and the next one has not
 * happened yet.
 */
static bool autorange_step(ina219_handle_t h, int16_t raw)
{
    if (!h->cfg.autorange) {
        return false;
    }

    const int32_t mag = (raw < 0) ? -(int32_t)raw : (int32_t)raw;
    const int32_t fs  = pga_fullscale_counts(h->pga);

    /* Too hot: step coarser immediately. Clipping loses data outright, so this
     * branch has no dwell — it reacts on a single sample. */
    if (h->pga < h->cfg.pga_max && mag > (fs * RANGE_UP_NUM) / RANGE_UP_DEN) {
        h->pga++;
        h->quiet_run = 0;
        if (apply_config(h) == ESP_OK) {
            ESP_LOGD(TAG, "range -> %s (hot)", ina219_pga_str(h->pga));
            return true;
        }
        return false;
    }

    /* Comfortably quiet: step finer, but only after a dwell, and only if the
     * signal would sit below 40% of the finer range's full scale. The gap between
     * the two thresholds is the hysteresis that stops this oscillating. */
    if (h->pga > INA219_PGA_1) {
        const int32_t fs_finer = pga_fullscale_counts(h->pga - 1);
        if (mag < (fs_finer * RANGE_DOWN_NUM) / RANGE_DOWN_DEN) {
            if (++h->quiet_run >= RANGE_DOWN_DWELL) {
                h->pga--;
                h->quiet_run = 0;
                if (apply_config(h) == ESP_OK) {
                    ESP_LOGD(TAG, "range -> %s (quiet)", ina219_pga_str(h->pga));
                    return true;
                }
            }
            return false;
        }
    }

    h->quiet_run = 0;
    return false;
}

esp_err_t ina219_read(ina219_handle_t h, ina219_sample_t *out)
{
    ESP_RETURN_ON_FALSE(h && out, ESP_ERR_INVALID_ARG, TAG, "null");

    uint16_t bus = 0;
    ESP_RETURN_ON_ERROR(reg_read(h, REG_BUS, &bus), TAG, "read bus");

    if (!(bus & BUS_CNVR)) {
        return ESP_ERR_NOT_FINISHED;
    }

    uint16_t shunt_u = 0;
    ESP_RETURN_ON_ERROR(reg_read(h, REG_SHUNT, &shunt_u), TAG, "read shunt");

    /*
     * CNVR is cleared by reading the Power register (or by rewriting Config).
     * We do not use the Power value, but we must read it anyway or CNVR stays
     * set and every subsequent poll reports a stale conversion as fresh.
     * This is the single most easily-missed detail in the whole driver.
     */
    uint16_t discard = 0;
    ESP_RETURN_ON_ERROR(reg_read(h, REG_POWER, &discard), TAG, "clear CNVR");
    (void)discard;

    const int16_t raw_shunt = (int16_t)shunt_u;

    /* One conversion is sacrificed after an externally-forced range or profile
     * change (§4.2): this reading was converted before the new settings applied. */
    if (h->range_changed) {
        h->range_changed = false;
        return ESP_ERR_INVALID_STATE;
    }

    /* An autorange step rewrites the config register, which both restarts the
     * conversion and clears CNVR, so the next reading is guaranteed fresh and on
     * the new range. Only this one sample is lost. */
    if (autorange_step(h, raw_shunt)) {
        return ESP_ERR_INVALID_STATE;
    }

    convert(h, raw_shunt, bus, out);
    out->t_us = esp_timer_get_time();

    if (bus & BUS_OVF) {
        /* Only meaningful when the calibration register is programmed, which it
         * is not (see the header). Kept as a tripwire in case that ever changes. */
        ESP_LOGW(TAG, "math overflow flag set");
    }
    return ESP_OK;
}

esp_err_t ina219_read_blocking(ina219_handle_t h, ina219_sample_t *out)
{
    ESP_RETURN_ON_FALSE(h && out, ESP_ERR_INVALID_ARG, TAG, "null");

    for (int attempt = 0; attempt < 4; attempt++) {
        ESP_RETURN_ON_ERROR(ina219_trigger(h), TAG, "trigger");

        const uint32_t wait_us = ina219_conversion_time_us(h) + 1000;
        vTaskDelay(pdMS_TO_TICKS((wait_us / 1000) + 2));

        esp_err_t err = ina219_read(h, out);
        if (err == ESP_OK) {
            return ESP_OK;
        }
        /* NOT_FINISHED or a discarded range change: go round again. Four attempts
         * covers the worst case of two consecutive range steps. */
        if (err != ESP_ERR_NOT_FINISHED && err != ESP_ERR_INVALID_STATE) {
            return err;
        }
    }
    return ESP_ERR_TIMEOUT;
}

/* --- runtime configuration -------------------------------------------------- */

esp_err_t ina219_set_shunt_uohm(ina219_handle_t h, uint32_t r)
{
    ESP_RETURN_ON_FALSE(h && r >= R_SHUNT_MIN_UOHM, ESP_ERR_INVALID_ARG, TAG,
                        "shunt must be >= %d uOhm", R_SHUNT_MIN_UOHM);
    h->cfg.r_shunt_uohm = r;
    return ESP_OK;
}

esp_err_t ina219_set_offset_ua(ina219_handle_t h, int32_t o)
{
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "null");
    h->cfg.i_offset_ua = o;
    return ESP_OK;
}

esp_err_t ina219_set_gain_ppm(ina219_handle_t h, uint32_t g)
{
    /* +/-10% is far wider than the part's +/-0.5% spec; anything outside it means
     * the shunt value is wrong, and trimming gain would only hide that. */
    ESP_RETURN_ON_FALSE(h && g >= 900000 && g <= 1100000, ESP_ERR_INVALID_ARG, TAG,
                        "gain out of range");
    h->cfg.i_gain_ppm = g;
    return ESP_OK;
}

esp_err_t ina219_set_vbus_divider_q16(ina219_handle_t h, uint32_t q16)
{
    ESP_RETURN_ON_FALSE(h && q16 >= 65536, ESP_ERR_INVALID_ARG, TAG,
                        "divider ratio must be >= 1.0");
    h->cfg.vbus_divider_q16 = q16;
    return ESP_OK;
}

esp_err_t ina219_set_vbus_offset_uv(ina219_handle_t h, int32_t offset_uv)
{
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "null");
    h->cfg.v_offset_uv = offset_uv;
    return ESP_OK;
}

esp_err_t ina219_set_vbus_gain_ppm(ina219_handle_t h, uint32_t g)
{
    /* Same +/-10% bound as the current channel: a bus reading further out than that
     * means the divider ratio is wrong, and trimming gain would only mask it. */
    ESP_RETURN_ON_FALSE(h && g >= 900000 && g <= 1100000, ESP_ERR_INVALID_ARG, TAG,
                        "gain out of range");
    h->cfg.v_gain_ppm = g;
    return ESP_OK;
}

esp_err_t ina219_set_autorange(ina219_handle_t h, bool en)
{
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "null");
    h->cfg.autorange = en;
    h->quiet_run     = 0;
    return ESP_OK;
}

esp_err_t ina219_set_pga(ina219_handle_t h, ina219_pga_t pga)
{
    ESP_RETURN_ON_FALSE(h && (int)pga >= 0 && (int)pga <= (int)INA219_PGA_8,
                        ESP_ERR_INVALID_ARG, TAG, "bad pga");
    h->pga           = pga;
    h->quiet_run     = 0;
    h->range_changed = true;
    return apply_config(h);
}

esp_err_t ina219_set_pga_max(ina219_handle_t h, ina219_pga_t pga_max)
{
    ESP_RETURN_ON_FALSE(h && (int)pga_max >= 0 && (int)pga_max <= (int)INA219_PGA_8,
                        ESP_ERR_INVALID_ARG, TAG, "bad pga_max");
    h->cfg.pga_max = pga_max;
    if (h->pga > pga_max) {
        return ina219_set_pga(h, pga_max);
    }
    return ESP_OK;
}

esp_err_t ina219_set_invert_sign(ina219_handle_t h, bool invert)
{
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "null");
    h->cfg.invert_sign = invert;
    return ESP_OK;
}

esp_err_t ina219_set_vbus_comp(ina219_handle_t h, ina219_vbus_comp_t comp)
{
    ESP_RETURN_ON_FALSE(h && comp <= INA219_VBUS_COMP_SUB_SHUNT, ESP_ERR_INVALID_ARG,
                        TAG, "bad vbus_comp");
    h->cfg.vbus_comp = comp;
    return ESP_OK;
}

uint32_t     ina219_get_shunt_uohm(ina219_handle_t h) { return h->cfg.r_shunt_uohm; }
int32_t      ina219_get_offset_ua(ina219_handle_t h)  { return h->cfg.i_offset_ua; }
uint32_t     ina219_get_gain_ppm(ina219_handle_t h)   { return h->cfg.i_gain_ppm; }
uint32_t     ina219_get_vbus_divider_q16(ina219_handle_t h) { return h->cfg.vbus_divider_q16; }
int32_t      ina219_get_vbus_offset_uv(ina219_handle_t h)   { return h->cfg.v_offset_uv; }
uint32_t     ina219_get_vbus_gain_ppm(ina219_handle_t h)    { return h->cfg.v_gain_ppm; }
bool         ina219_get_autorange(ina219_handle_t h)  { return h->cfg.autorange; }
ina219_pga_t ina219_get_pga(ina219_handle_t h)        { return h->pga; }
ina219_pga_t ina219_get_pga_max(ina219_handle_t h)    { return h->cfg.pga_max; }
bool         ina219_get_invert_sign(ina219_handle_t h){ return h->cfg.invert_sign; }
ina219_vbus_comp_t ina219_get_vbus_comp(ina219_handle_t h) { return h->cfg.vbus_comp; }
