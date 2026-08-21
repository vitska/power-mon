/*
 * ina219.h — register-level INA219 driver for bat-monitor.
 *
 * DESIGN.md §2.2, §4.1–§4.3.
 *
 * Deliberate design points, restated here because they are easy to "helpfully" undo:
 *
 *  - The Current (0x04) and Power (0x03) registers are NOT used as data sources.
 *    Current and power are computed in software from the raw shunt and bus readings
 *    so that shunt resistance, offset, gain and the PGA range all apply in one place.
 *    The calibration register stays 0.
 *
 *  - No floating point anywhere. Charge integration downstream is int64 microampere-
 *    seconds and must stay exact; a float creeping in here would propagate.
 *
 *  - The driver owns PGA auto-ranging (§4.2). Callers see amperes, not ranges.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Programmable gain amplifier setting — sets the shunt full-scale range.
 *  The LSB is 10 uV at every setting; the PGA changes the range, not the resolution. */
typedef enum {
    INA219_PGA_1 = 0, /**< +/- 40 mV  */
    INA219_PGA_2 = 1, /**< +/- 80 mV  */
    INA219_PGA_4 = 2, /**< +/- 160 mV */
    INA219_PGA_8 = 3, /**< +/- 320 mV */
} ina219_pga_t;

/** ADC resolution / hardware averaging, shared encoding for the bus and shunt ADCs. */
typedef enum {
    INA219_ADC_9BIT    = 0x0, /**<   84 us */
    INA219_ADC_10BIT   = 0x1, /**<  148 us */
    INA219_ADC_11BIT   = 0x2, /**<  276 us */
    INA219_ADC_12BIT   = 0x3, /**<  532 us */
    INA219_ADC_2AVG    = 0x9, /**< 1.06 ms */
    INA219_ADC_4AVG    = 0xA, /**< 2.13 ms */
    INA219_ADC_8AVG    = 0xB, /**< 4.26 ms */
    INA219_ADC_16AVG   = 0xC, /**< 8.51 ms */
    INA219_ADC_32AVG   = 0xD, /**< 17.0 ms */
    INA219_ADC_64AVG   = 0xE, /**< 34.1 ms */
    INA219_ADC_128AVG  = 0xF, /**< 68.1 ms */
} ina219_adc_t;

/** Bus-voltage correction for a low-side shunt (DESIGN.md §2.9.5).
 *  With the shunt in the negative lead, the VBUS pin measures pack+ relative to the
 *  system return, not to the battery negative. The two differ by exactly the shunt
 *  drop, which the same conversion pair already gives us -- so the correction is
 *  exact and free. Which way it goes depends on which sense lead went to which node. */
typedef enum {
    INA219_VBUS_COMP_NONE = 0,
    INA219_VBUS_COMP_ADD_SHUNT = 1,
    INA219_VBUS_COMP_SUB_SHUNT = 2,
} ina219_vbus_comp_t;

/** Sampling profile — see DESIGN.md §4.1 and §9.4.
 *  CONTINUOUS trades ~1 mA of sensor current for maximum averaging (tiers T0/T1).
 *  TRIGGERED converts once on demand and auto-powers-down to ~6 uA (tiers T2/T3). */
typedef enum {
    INA219_PROFILE_CONTINUOUS = 0,
    INA219_PROFILE_TRIGGERED  = 1,
} ina219_profile_t;

typedef struct {
    uint8_t  i2c_addr;          /**< 0x40..0x4F; 0x40 default */
    uint32_t scl_speed_hz;      /**< 100000 or 400000 */

    uint32_t r_shunt_uohm;      /**< shunt resistance, micro-ohms (10000 = 10 mOhm).
                                     Minimum 1000; below that the full-scale current
                                     no longer fits the int32 sample field. */
    int32_t  i_offset_ua;       /**< zero-current calibration, subtracted from readings */
    uint32_t i_gain_ppm;        /**< gain trim; 1000000 = unity */
    uint32_t vbus_divider_q16;  /**< external divider ratio, Q16.16; 65536 = 1.0 */
    int32_t  v_offset_uv;       /**< bus-voltage zero trim, subtracted before gain */
    uint32_t v_gain_ppm;        /**< bus-voltage gain trim; 1000000 = unity */

    bool         autorange;     /**< run the PGA state machine (§4.2) */
    ina219_pga_t pga;           /**< initial (or locked, if !autorange) range */
    ina219_pga_t pga_max;       /**< widest range auto-ranging may select. On a low-side
                                     build the inputs cannot survive the full 320 mV
                                     excursion below ground (§2.9.2), so this ceiling is
                                     a hardware safety limit, not a preference. */

    bool               invert_sign; /**< negate current; for reversed sense leads (§2.9) */
    ina219_vbus_comp_t vbus_comp;   /**< low-side bus-voltage correction (§2.9.5) */

    ina219_adc_t bus_adc_continuous;
    ina219_adc_t shunt_adc_continuous;
    ina219_adc_t bus_adc_triggered;
    ina219_adc_t shunt_adc_triggered;
} ina219_config_t;

/** Sensible starting point matching DESIGN.md §4.1 / §2.3.
 *  Defaults assume the LOW-SIDE topology of §2.9 with grounding option L1: the PGA is
 *  capped at /4 and bus-voltage compensation is on. For a high-side build set
 *  pga_max = INA219_PGA_8 and vbus_comp = INA219_VBUS_COMP_NONE. */
#define INA219_CONFIG_DEFAULT()                        \
    ((ina219_config_t){                                \
        .i2c_addr             = 0x40,                  \
        .scl_speed_hz         = 400000,                \
        .r_shunt_uohm         = 10000,                 \
        .i_offset_ua          = 0,                     \
        .i_gain_ppm           = 1000000,               \
        .vbus_divider_q16     = 65536,                         .v_offset_uv          = 0,                             .v_gain_ppm           = 1000000,               \
        .autorange            = true,                  \
        .pga                  = INA219_PGA_4,          \
        .pga_max              = INA219_PGA_4,          \
        .invert_sign          = false,                 \
        .vbus_comp            = INA219_VBUS_COMP_ADD_SHUNT, \
        .bus_adc_continuous   = INA219_ADC_128AVG,     \
        .shunt_adc_continuous = INA219_ADC_128AVG,     \
        .bus_adc_triggered    = INA219_ADC_12BIT,      \
        .shunt_adc_triggered  = INA219_ADC_4AVG,       \
    })

typedef struct {
    int64_t  t_us;        /**< esp_timer timestamp of the read that produced this */
    uint32_t v_uv;        /**< bus voltage, microvolts, divider-corrected */
    int32_t  i_ua;        /**< current, microamperes; >0 = charging (§2.8) */
    int32_t  p_uw;        /**< power, microwatts, signed like the current */
    int32_t  v_shunt_uv;  /**< raw shunt voltage, microvolts, offset NOT applied */
    int16_t  raw_shunt;   /**< register 0x01 as read */
    uint16_t raw_bus;     /**< register 0x02 as read, including CNVR/OVF flags */
    ina219_pga_t pga;     /**< range in force for this sample */
    bool     saturated;   /**< shunt reading is at/over the PGA full scale */
} ina219_sample_t;

typedef struct ina219_dev_t *ina219_handle_t;

/**
 * Probe, reset and configure the device.
 *
 * Verifies the post-reset configuration register reads the datasheet default
 * (0x399F) before trusting the bus — a NACK-free but wrong device is a common
 * outcome of a mis-wired I2C bring-up and is worth catching here rather than as
 * implausible currents three layers up.
 *
 * @return ESP_ERR_NOT_FOUND if nothing answers, ESP_ERR_INVALID_RESPONSE if
 *         something answers but is not an INA219.
 */
esp_err_t ina219_init(i2c_master_bus_handle_t bus,
                      const ina219_config_t  *cfg,
                      ina219_handle_t        *out);

esp_err_t ina219_deinit(ina219_handle_t h);

/** Switch sampling profile; rewrites the configuration register. */
esp_err_t ina219_set_profile(ina219_handle_t h, ina219_profile_t profile);

/**
 * Start one conversion. Required in TRIGGERED profile before each read; a no-op
 * (returns ESP_OK) in CONTINUOUS.
 */
esp_err_t ina219_trigger(ina219_handle_t h);

/**
 * Read one sample.
 *
 * @return ESP_OK                 sample is valid and fresh
 *         ESP_ERR_NOT_FINISHED   no new conversion since the last read; try again
 *         ESP_ERR_INVALID_STATE  the PGA range just changed; this conversion was
 *                                taken on the old range and has been discarded
 *         (other)                I2C transport error
 *
 * Callers integrating charge must treat every non-ESP_OK return as "no sample",
 * never as "zero current" (DESIGN.md §5.2 gap handling).
 */
esp_err_t ina219_read(ina219_handle_t h, ina219_sample_t *out);

/** Convenience: trigger (if needed), wait out the conversion, read. Blocking.
 *  Bring-up and calibration use this; the sampler task does not. */
esp_err_t ina219_read_blocking(ina219_handle_t h, ina219_sample_t *out);

/** Worst-case conversion time of the active profile, both channels, microseconds. */
uint32_t ina219_conversion_time_us(ina219_handle_t h);

/* --- runtime calibration / configuration ------------------------------------ */

esp_err_t ina219_set_shunt_uohm(ina219_handle_t h, uint32_t r_shunt_uohm);
esp_err_t ina219_set_offset_ua(ina219_handle_t h, int32_t offset_ua);
esp_err_t ina219_set_gain_ppm(ina219_handle_t h, uint32_t gain_ppm);
esp_err_t ina219_set_vbus_divider_q16(ina219_handle_t h, uint32_t q16);

/* Bus-voltage conversion curve. The divider ratio describes the hardware; these two
 * trim what is left after it -- ADC gain error and the offset of the divider itself.
 * Both apply BEFORE the low-side shunt compensation, because that compensation is an
 * exact physical correction and must not be scaled by a calibration constant. */
esp_err_t ina219_set_vbus_offset_uv(ina219_handle_t h, int32_t offset_uv);
esp_err_t ina219_set_vbus_gain_ppm(ina219_handle_t h, uint32_t gain_ppm);
esp_err_t ina219_set_autorange(ina219_handle_t h, bool enable);
esp_err_t ina219_set_pga(ina219_handle_t h, ina219_pga_t pga);
esp_err_t ina219_set_pga_max(ina219_handle_t h, ina219_pga_t pga_max);
esp_err_t ina219_set_invert_sign(ina219_handle_t h, bool invert);
esp_err_t ina219_set_vbus_comp(ina219_handle_t h, ina219_vbus_comp_t comp);

uint32_t     ina219_get_shunt_uohm(ina219_handle_t h);
int32_t      ina219_get_offset_ua(ina219_handle_t h);
uint32_t     ina219_get_gain_ppm(ina219_handle_t h);
uint32_t     ina219_get_vbus_divider_q16(ina219_handle_t h);
int32_t      ina219_get_vbus_offset_uv(ina219_handle_t h);
uint32_t     ina219_get_vbus_gain_ppm(ina219_handle_t h);
bool         ina219_get_autorange(ina219_handle_t h);
ina219_pga_t ina219_get_pga(ina219_handle_t h);
ina219_pga_t ina219_get_pga_max(ina219_handle_t h);
bool         ina219_get_invert_sign(ina219_handle_t h);
ina219_vbus_comp_t ina219_get_vbus_comp(ina219_handle_t h);

/** Shunt full-scale in microvolts for a given range. */
int32_t ina219_pga_fullscale_uv(ina219_pga_t pga);

/** Human-readable range name, e.g. "/8 (+/-320mV)". */
const char *ina219_pga_str(ina219_pga_t pga);

#ifdef __cplusplus
}
#endif
