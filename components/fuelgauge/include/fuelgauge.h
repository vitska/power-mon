/*
 * fuelgauge.h — coulomb counting with voltage re-anchoring (DESIGN.md §5).
 *
 * The gauge is a charge integrator whose absolute reference is periodically restored
 * from voltage. Neither half works alone: integration drifts without bound, and
 * voltage under load says almost nothing about state of charge.
 *
 * TWO SEPARATE VOLTAGE PAIRS, which is the one place this deviates from Appendix B
 * and the reason is worth stating:
 *
 *   v_0pct / v_100pct  — RESTING open-circuit voltage at the ends of the SoC scale.
 *                        These map voltage to SoC. For a 12 V lead-acid that is about
 *                        11.80 V and 12.70 V.
 *   v_full             — the ABSORPTION voltage that, together with taper current,
 *                        means "charging has finished". About 14.4 V.
 *
 * Appendix B lists 1.80 and 2.40 V/cell, which are the discharge cutoff and the
 * absorption setpoint — the operating window, not the OCV window. Using that pair for
 * the SoC map would read a fully rested 12.70 V battery as 53 %. So the anchors use
 * the operating window and the map uses the OCV window, and both are configurable.
 *
 * CURRENT COMPENSATION. Terminal voltage under load is not OCV; it differs by I·R
 * through the internal resistance. Every voltage-based decision here works on the
 * compensated estimate, never the raw terminal reading:
 *
 *     ocv_uv = v_terminal_uv - (i_ua * r_int_uohm) / 1e6
 *
 * With the sign convention of §2.8 (I > 0 charging), that subtracts the charging rise
 * and adds back the discharge sag, which is what makes a mid-discharge OCV estimate
 * usable at all.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "sensors.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FG_UNKNOWN = 0, /**< no trustworthy reference yet */
    FG_COUNTING,    /**< integrating, current outside the deadband */
    FG_RESTING,     /**< idle long enough that OCV is meaningful */
    FG_FULL,        /**< absorption voltage reached at taper current */
    FG_EMPTY,       /**< hit the empty endpoint under load */
} fg_state_t;

typedef struct {
    uint32_t design_capacity_uah; /**< nameplate, e.g. 44 Ah = 44000000 */
    uint32_t v_0pct_uv;           /**< resting OCV at 0 % */
    uint32_t v_100pct_uv;         /**< resting OCV at 100 % */
    uint32_t v_full_uv;           /**< absorption voltage for full detection */
    uint32_t r_int_uohm;          /**< internal resistance, for I·R compensation */
    uint32_t i_taper_ua;          /**< charge current below this + v_full = full */
    uint32_t i_deadband_ua;       /**< |i| below this integrates as zero (§5.2) */
    uint32_t t_rest_s;            /**< idle time before OCV is trusted */
    uint32_t t_full_hold_s;       /**< how long the full condition must hold */
    uint16_t ocv_blend_q8;        /**< re-anchor strength, 256 = snap (§5.4) */

    /*
     * Peukert (§5.2). Discharging faster than the rate the capacity was specified at
     * takes more OUT of the pack than the coulombs suggest:
     *
     *     dq_effective = dq × (|i| / i_rated) ^ (k − 1)
     *
     * k = 1.0 disables it. Lead-acid is ~1.15, which is why it matters here and not
     * on the lithium chemistries. Applied to the SoC count only -- the lifetime
     * coulomb counters stay raw, because charge that flowed, flowed.
     */
    uint16_t peukert_q8;          /**< k in Q8; 256 = 1.00, 294 ≈ 1.15 */
    uint32_t i_rated_ua;          /**< the rate the nameplate capacity assumes */

    /* Capacity learning (§5.4). */
    uint16_t learn_min_depth_permille; /**< minimum discharge depth to learn from */
    uint16_t learn_blend_q8;           /**< how hard to move capacity, 256 = snap */
} fg_config_t;

/** Defaults for a 12 V / 44 A·h flooded lead-acid — the battery this was brought up
 *  on. Chemistry-specific values are commented with where they come from. */
#define FG_CONFIG_LEAD_ACID_12V_44AH()                  \
    ((fg_config_t){                                     \
        .design_capacity_uah = 44000000u,               \
        .v_0pct_uv           = 11800000u, /* resting */ \
        .v_100pct_uv         = 12700000u, /* resting */ \
        .v_full_uv           = 14400000u, /* 2.40 V/cell, Appendix B */ \
        .r_int_uohm          = 6000u,     /* ~6 mOhm typical for this size */ \
        .i_taper_ua          = 1466667u,  /* design/30, §8.3 tag 0x0020 */ \
        .i_deadband_ua       = 3000u,     /* §5.2 */    \
        .t_rest_s            = 600u,      /* lead-acid settles slowly */ \
        .t_full_hold_s       = 60u,       /* §8.3 tag 0x0021 */ \
        .ocv_blend_q8        = 64u,       /* 0.25, §8.3 tag 0x0023 */ \
        .peukert_q8          = 294u,      /* k 1.15, Appendix B lead-acid */ \
        .i_rated_ua          = 2200000u,  /* C/20 = 2.2 A: how car batteries are rated */ \
        .learn_min_depth_permille = 600u, /* §8.3 tag 0x0024 */ \
        .learn_blend_q8      = 64u,       /* 0.25 -- learn slowly, it is a big claim */ \
    })

typedef struct {
    fg_state_t state;
    uint32_t   soc_permille;      /**< 0..1000 */
    int64_t    charge_uas;        /**< accumulated charge above empty */
    uint32_t   full_capacity_uah; /**< learned; starts at design */
    uint32_t   ocv_uv;            /**< last I·R-compensated estimate */
    int32_t    ir_drop_uv;        /**< the compensation applied, for display */
    int64_t    cum_in_uas;        /**< lifetime charge in */
    int64_t    cum_out_uas;       /**< lifetime charge out (positive) */
    uint32_t   s_since_anchor;    /**< seconds since the last voltage re-anchor */
    bool       voltage_only;      /**< true while SoC comes from OCV, not counting */

    uint32_t   design_capacity_uah; /**< for SoH: learned / design */
    uint32_t   learn_count;         /**< how many times capacity has been learned */
    uint32_t   last_learn_uah;      /**< the last raw measurement, before blending */
    int64_t    q_since_full_uas;    /**< effective charge since the last full anchor */
    bool       have_full_anchor;    /**< a learn window is open */
    uint32_t   peukert_factor_q16;  /**< the multiplier in use right now, for display */
} fg_status_t;

/** Loads config and state from NVS, or seeds the lead-acid defaults. */
esp_err_t fg_init(void);

/** Feed one accepted sample. Safe to call at the sampler's full rate; it does its own
 *  dt measurement from the sample timestamp. */
void fg_update(const power_sample_t *s);

void fg_get(fg_status_t *out);
fg_config_t fg_get_config(void);

/** Validates and stores a whole config. Rejects an inverted or zero-width voltage
 *  window, or a zero capacity, rather than dividing by them later. */
esp_err_t fg_set_config(const fg_config_t *cfg);

/** Force SoC — the user knows better (§8.4 cmd 0x02). */
esp_err_t fg_set_soc_permille(uint32_t permille);

/** Declare the pack full now (§8.4 cmd 0x03). */
esp_err_t fg_set_full(void);

/** Back to UNKNOWN, counters zeroed, next voltage sample re-bootstraps. */
esp_err_t fg_reset(void);

/** Commit state and config. Called automatically on significant change. */
esp_err_t fg_save(void);

const char *fg_state_str(fg_state_t s);

#ifdef __cplusplus
}
#endif
