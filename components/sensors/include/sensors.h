/*
 * sensors.h — dual-INA219 role abstraction (DESIGN.md §2.10).
 *
 * The board carries two INA219s, one per pole, so the shunt can go in whichever lead
 * the installation allows. Exactly one of them is looking at the shunt; the other has
 * its differential inputs shorted and contributes the pack voltage, the load-side
 * voltage and a live offset reading.
 *
 * The whole point of this layer is that NOTHING above it needs to know any of that.
 * The fuel gauge asks for a power_sample_t and gets current, voltage and power with a
 * single timestamp, regardless of which pole carries the shunt or how many devices
 * actually answered. Leaking "sensor A" or "sensor B" upwards would spread a wiring
 * detail through the entire firmware.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "ina219.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Where the shunt is physically installed (DESIGN.md §2.10.3). */
typedef enum {
    SENSORS_MODE_AUTO = 0,   /**< resolve by observation; needs a load (§2.10.6) */
    SENSORS_MODE_P    = 1,   /**< shunt in the positive lead, high-side */
    SENSORS_MODE_N    = 2,   /**< shunt in the negative lead, low-side */
    SENSORS_MODE_SINGLE = 3, /**< one device only; software bus correction (§2.9.5) */
} sensors_mode_t;

/** Resolution state. UNKNOWN is not an error — it is the honest state before a load
 *  has been seen, and the gauge must not integrate while it holds (§2.10.6). */
typedef enum {
    SENSORS_ROLE_UNKNOWN = 0,
    SENSORS_ROLE_RESOLVED,
} sensors_role_state_t;

typedef struct {
    uint8_t addr_pos_pole;   /**< 0x40 — sensor at the positive pole */
    uint8_t addr_neg_pole;   /**< 0x41 — sensor at the negative pole */
    sensors_mode_t mode;

    uint32_t r_shunt_uohm;
    uint32_t scl_speed_hz;
    ina219_pga_t pga_max;    /**< §2.9.2 ceiling, applies to the current sensor */
    bool     invert_sign;

    uint8_t  voltage_divisor;   /**< read pack voltage every Nth sample (§4.5) */
    uint8_t  diagnostic_divisor;/**< read load voltage / idle offset every Nth sample */
} sensors_config_t;

#define SENSORS_CONFIG_DEFAULT()                 \
    ((sensors_config_t){                         \
        .addr_pos_pole      = 0x40,              \
        .addr_neg_pole      = 0x41,              \
        .mode               = SENSORS_MODE_AUTO, \
        .r_shunt_uohm       = 10000,             \
        .scl_speed_hz       = 400000,            \
        .pga_max            = INA219_PGA_4,      \
        .invert_sign        = false,             \
        .voltage_divisor    = 8,                 \
        .diagnostic_divisor = 64,                \
    })

/** A complete electrical sample, assembled from whichever devices are present. */
typedef struct {
    int64_t  t_us;          /**< timestamp of the CURRENT reading — the one that dates
                                 the sample, because dt drives the integral (§4.5) */
    int32_t  i_ua;          /**< >0 = charging into the pack */
    uint32_t v_pack_uv;     /**< battery terminal voltage */
    int32_t  p_uw;

    bool     i_valid;       /**< false => do not integrate, do not assume zero */
    bool     v_valid;
    bool     v_is_fresh;    /**< false => v_pack_uv is carried over from an earlier pass */

    /* Diagnostics; updated at diagnostic_divisor cadence, zero until then. */
    uint32_t v_load_uv;     /**< voltage at the load side (§2.10.5) */
    int32_t  idle_offset_ua;/**< shorted sensor's own zero reading */
    int32_t  v_shunt_uv;    /**< raw drop across the shunt */
    ina219_pga_t pga;
    bool     saturated;
} power_sample_t;

typedef struct sensors_ctx_t *sensors_handle_t;

/**
 * Probe both addresses and bring up whatever is present.
 *
 * Succeeds with one device (degraded, §2.10.7) and fails only if neither answers.
 * Does not resolve AUTO — that needs current to be flowing (§2.10.6).
 */
esp_err_t sensors_init(i2c_master_bus_handle_t bus,
                       const sensors_config_t *cfg,
                       sensors_handle_t       *out);

esp_err_t sensors_deinit(sensors_handle_t h);

/**
 * Take one pass. Reads the current sensor every call and the others on their own
 * divisors (§4.5).
 *
 * @return ESP_OK on a usable sample. ESP_ERR_INVALID_STATE while roles are unresolved:
 *         the sample's diagnostic fields are still filled in so `detect` can work, but
 *         `i_valid` is false and the caller must not integrate it.
 */
esp_err_t sensors_read(sensors_handle_t h, power_sample_t *out);

/** Same, but blocking and trigger-aware; for bring-up and calibration. */
esp_err_t sensors_read_blocking(sensors_handle_t h, power_sample_t *out);

/**
 * Try to work out which pole carries the shunt by comparing the two differential
 * channels under load (§2.10.6).
 *
 * Requires real current to be flowing. Returns ESP_ERR_INVALID_STATE and changes
 * nothing when the evidence is too weak — guessing here would invert the sign of every
 * subsequent measurement.
 *
 * @param out_pos_uv  differential seen by the positive-pole sensor (may be NULL)
 * @param out_neg_uv  differential seen by the negative-pole sensor (may be NULL)
 */
esp_err_t sensors_detect_mode(sensors_handle_t h, uint32_t samples,
                              int32_t *out_pos_uv, int32_t *out_neg_uv);

esp_err_t       sensors_set_mode(sensors_handle_t h, sensors_mode_t mode);
sensors_mode_t  sensors_get_mode(sensors_handle_t h);
sensors_role_state_t sensors_get_role_state(sensors_handle_t h);

/** The device acting as the current source, for calibration that must reach the
 *  hardware directly (zero calibration, PGA locking). NULL while unresolved. */
ina219_handle_t sensors_current_dev(sensors_handle_t h);
/** The device supplying pack voltage. May be the same device in SINGLE mode. */
ina219_handle_t sensors_voltage_dev(sensors_handle_t h);

bool sensors_have_pos(sensors_handle_t h);

/** The device at each pole, whatever its role; NULL if absent. For diagnostics that
 *  need each sensor's own reading, like the console's `raw`. */
ina219_handle_t sensors_pos_dev(sensors_handle_t h);
ina219_handle_t sensors_neg_dev(sensors_handle_t h);
bool sensors_have_neg(sensors_handle_t h);

const char *sensors_mode_str(sensors_mode_t m);

/** One-line human summary of what was found and how it is wired. */
void sensors_report(sensors_handle_t h);

/* --- calibration (moved here from the console: this is sensor behaviour, not
 * command-line glue. Nothing below prints -- a caller wanting progress feedback
 * during a long average passes `progress`, called once per sample; a caller wanting
 * exclusive access against a concurrent sampler task still arranges that itself,
 * exactly as before this code lived here. */

typedef void (*sensors_progress_cb_t)(uint32_t sample_index, void *ctx);

/**
 * Averages n samples of current and pack voltage together, freezing autoranging for
 * the duration: a range change discards the next conversion, and a scale that moves
 * mid-average is a worse measurement than a slightly noisy one at a fixed scale.
 */
esp_err_t sensors_average(sensors_handle_t h, uint32_t n,
                         int64_t *sum_i_ua, int64_t *sum_v_uv,
                         uint32_t *got, bool *saturated,
                         sensors_progress_cb_t progress, void *progress_ctx);

/**
 * Solves the gain that would make `measured` read as `reference`, scaled from
 * `old_ppm`. No range validation -- see ina219_set_gain_ppm() / set_vbus_gain_ppm()
 * for the driver's own hardware-sanity floor. Returns false only when `measured` is
 * exactly zero, since a ratio against it is not a number.
 */
bool sensors_solve_gain_ppm(int64_t measured, int64_t reference, uint32_t old_ppm,
                           uint32_t *new_ppm);

/** What `sensors_calibrate_shunt()` found and did. */
typedef struct {
    bool     mode_changed;   /**< true if this call moved the install mode */
    sensors_mode_t mode;     /**< pole the current was found on */
    ina219_handle_t dev;     /**< the device now holding the current role */
    int64_t  v_pos_uv, v_neg_uv;   /**< each pole's raw shunt reading, for display */
    uint32_t got_pos, got_neg;
    bool     sat_pos, sat_neg;
    bool     inverted;
    uint32_t shunt_uohm;
    int32_t  offset_ua;
} sensors_shunt_cal_t;

/**
 * Auto-detects which pole's INA219 is actually wired across the shunt -- whichever
 * reads the larger raw shunt voltage under this known current -- then solves that
 * device's resistance directly from the measured value, never from a fixed or
 * configured shunt. Sets mode (only when both poles answered), sign, shunt
 * resistance, offset (carried over from any existing zero point on the same chip)
 * and unity gain.
 *
 * Returns ESP_ERR_INVALID_ARG for a zero known current, ESP_ERR_NOT_FOUND if neither
 * pole answered, ESP_ERR_INVALID_SIZE if the computed resistance is below the
 * driver's own floor (ina219_set_shunt_uohm()) -- `out->shunt_uohm` is still filled
 * in with what was computed so the caller can report it, but nothing was applied:
 * mode, sign, offset and gain are all left exactly as they were.
 */
esp_err_t sensors_calibrate_shunt(sensors_handle_t h, int64_t known_ua, uint32_t n_samples,
                                  sensors_progress_cb_t progress, void *progress_ctx,
                                  sensors_shunt_cal_t *out);

/**
 * Zero-current calibration (DESIGN.md §5.5): forces PGA/1 and unity gain, averages
 * the raw current, and bakes the mean in as the offset -- no validation of spread or
 * magnitude, on the assumption the load was actually disconnected. Restores PGA,
 * autorange and gain regardless of outcome; on a read error the offset is left
 * unchanged and the error is returned.
 */
esp_err_t sensors_zero_current(sensors_handle_t h, uint32_t n_samples,
                               int32_t *out_offset_ua, int32_t *out_stddev_ua,
                               sensors_progress_cb_t progress, void *progress_ctx);

/** The voltage-channel twin of sensors_zero_current(). No range to force and no
 *  spread check -- see the implementation for why. */
esp_err_t sensors_zero_voltage(sensors_handle_t h, uint32_t n_samples,
                               int32_t *out_offset_uv, uint32_t *out_spread_uv,
                               sensors_progress_cb_t progress, void *progress_ctx);

#ifdef __cplusplus
}
#endif
