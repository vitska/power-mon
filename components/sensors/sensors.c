/*
 * sensors.c — see include/sensors.h and DESIGN.md §2.10.
 */

#include "sensors.h"

#include <stdlib.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sensors";

/* Detection needs the shunt sensor to dominate the shorted one by this factor before
 * the evidence is considered conclusive (DESIGN.md §2.10.6). */
#define DETECT_RATIO      10
/* ...and the winning reading must be at least this large in absolute terms, so that
 * two near-zero readings cannot produce a large ratio by accident. */
#define DETECT_MIN_UV     500

struct sensors_ctx_t {
    sensors_config_t cfg;

    ina219_handle_t pos;   /* 0x40, positive pole; NULL if absent */
    ina219_handle_t neg;   /* 0x41, negative pole; NULL if absent */

    /* Resolved roles. Both point into {pos, neg} and are never owned. */
    ina219_handle_t cur_dev;
    ina219_handle_t volt_dev;
    sensors_role_state_t role_state;

    uint32_t pass;             /* counts sensors_read() calls, drives the divisors */
    uint32_t v_pack_uv_cache;  /* last good pack voltage, for passes that skip it */
    bool     v_cache_valid;
    uint32_t v_load_uv_cache;
    int32_t  idle_offset_cache;
};

const char *sensors_mode_str(sensors_mode_t m)
{
    switch (m) {
    case SENSORS_MODE_AUTO:   return "auto (unresolved)";
    case SENSORS_MODE_P:      return "P: shunt in positive lead";
    case SENSORS_MODE_N:      return "N: shunt in negative lead";
    case SENSORS_MODE_SINGLE: return "single sensor";
    default:                  return "?";
    }
}

/* --- role assignment -------------------------------------------------------- */

/*
 * Assign the current and voltage roles from the mode and what is actually present.
 *
 * The voltage role deliberately prefers the sensor NOT carrying the shunt: in mode N
 * that device's GND sits at the battery negative, so its VBUS reads the true terminal
 * voltage with no dependence on the shunt channel (§2.10.4). Falling back to the
 * current sensor's own VBUS costs the software correction of §2.9.5 and couples a
 * voltage fault to the current channel, so it is a fallback and not a default.
 */
static void assign_roles(sensors_handle_t h)
{
    h->cur_dev    = NULL;
    h->volt_dev   = NULL;
    h->role_state = SENSORS_ROLE_UNKNOWN;

    switch (h->cfg.mode) {
    case SENSORS_MODE_P:
        h->cur_dev  = h->pos;
        h->volt_dev = h->neg ? h->neg : h->pos;
        break;
    case SENSORS_MODE_N:
        h->cur_dev  = h->neg;
        h->volt_dev = h->pos ? h->pos : h->neg;
        break;
    case SENSORS_MODE_SINGLE:
        h->cur_dev  = h->pos ? h->pos : h->neg;
        h->volt_dev = h->cur_dev;
        break;
    case SENSORS_MODE_AUTO:
    default:
        return; /* stays UNKNOWN until detection succeeds */
    }

    if (!h->cur_dev) {
        ESP_LOGE(TAG, "mode %s needs a sensor that did not answer",
                 sensors_mode_str(h->cfg.mode));
        return;
    }

    /* The current sensor carries the range ceiling and the sign convention; the
     * voltage sensor's differential channel is shorted and stays wide open, since
     * all it ever sees is its own offset. */
    ina219_set_pga_max(h->cur_dev, h->cfg.pga_max);
    ina219_set_invert_sign(h->cur_dev, h->cfg.invert_sign);
    ina219_set_shunt_uohm(h->cur_dev, h->cfg.r_shunt_uohm);

    /* Bus compensation is needed only when the voltage comes from the same device
     * that straddles the shunt (§2.9.5). With a dedicated voltage sensor referenced
     * to the battery negative there is nothing to correct. */
    const bool shared = (h->volt_dev == h->cur_dev);
    ina219_set_vbus_comp(h->volt_dev,
                         (shared && h->cfg.mode != SENSORS_MODE_P)
                             ? INA219_VBUS_COMP_ADD_SHUNT
                             : INA219_VBUS_COMP_NONE);

    h->role_state = SENSORS_ROLE_RESOLVED;
}

/* --- lifecycle -------------------------------------------------------------- */

static ina219_handle_t try_open(i2c_master_bus_handle_t bus,
                                const sensors_config_t *cfg, uint8_t addr,
                                const char *what)
{
    ina219_config_t ic  = INA219_CONFIG_DEFAULT();
    ic.i2c_addr         = addr;
    ic.scl_speed_hz     = cfg->scl_speed_hz;
    ic.r_shunt_uohm     = cfg->r_shunt_uohm;
    ic.pga              = cfg->pga_max;
    ic.pga_max          = cfg->pga_max;
    ic.vbus_comp        = INA219_VBUS_COMP_NONE; /* set per role in assign_roles() */

    ina219_handle_t d = NULL;
    const esp_err_t err = ina219_init(bus, &ic, &d);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s sensor at 0x%02X: %s", what, addr, esp_err_to_name(err));
        return NULL;
    }
    ESP_LOGI(TAG, "%s sensor at 0x%02X: ok", what, addr);
    return d;
}

esp_err_t sensors_init(i2c_master_bus_handle_t bus,
                       const sensors_config_t *cfg,
                       sensors_handle_t       *out)
{
    ESP_RETURN_ON_FALSE(bus && cfg && out, ESP_ERR_INVALID_ARG, TAG, "null arg");
    ESP_RETURN_ON_FALSE(cfg->addr_pos_pole != cfg->addr_neg_pole, ESP_ERR_INVALID_ARG,
                        TAG, "the two sensors cannot share address 0x%02X -- check the "
                             "A0/A1 straps", cfg->addr_pos_pole);
    ESP_RETURN_ON_FALSE(cfg->voltage_divisor > 0 && cfg->diagnostic_divisor > 0,
                        ESP_ERR_INVALID_ARG, TAG, "divisors must be non-zero");

    sensors_handle_t h = calloc(1, sizeof(struct sensors_ctx_t));
    ESP_RETURN_ON_FALSE(h, ESP_ERR_NO_MEM, TAG, "oom");
    h->cfg = *cfg;

    h->pos = try_open(bus, cfg, cfg->addr_pos_pole, "positive-pole");
    h->neg = try_open(bus, cfg, cfg->addr_neg_pole, "negative-pole");

    if (!h->pos && !h->neg) {
        ESP_LOGE(TAG, "no INA219 answered at 0x%02X or 0x%02X",
                 cfg->addr_pos_pole, cfg->addr_neg_pole);
        free(h);
        return ESP_ERR_NOT_FOUND;
    }

    /* One device present means the pair's diagnostics are gone, but the gauge can
     * still run on the §2.9.5 software correction (§2.10.7). Say so plainly rather
     * than failing: a degraded monitor beats no monitor. */
    if (!h->pos || !h->neg) {
        ESP_LOGW(TAG, "only one sensor present -- degrading to single-sensor mode; "
                      "pack voltage now depends on the shunt channel (DESIGN.md 2.9.5)");
        h->cfg.mode = SENSORS_MODE_SINGLE;
    }

    assign_roles(h);
    *out = h;
    return ESP_OK;
}

esp_err_t sensors_deinit(sensors_handle_t h)
{
    if (!h) {
        return ESP_OK;
    }
    if (h->pos) ina219_deinit(h->pos);
    if (h->neg) ina219_deinit(h->neg);
    free(h);
    return ESP_OK;
}

/* --- reading ---------------------------------------------------------------- */

static void fill_diagnostics(sensors_handle_t h, power_sample_t *out)
{
    /* Load-side voltage comes from the current sensor's own VBUS pin, which is wired
     * to LOAD+ (§2.10.3). Compared against the pack voltage and the shunt drop it
     * isolates cable and connector resistance (§2.10.5). */
    if (h->cur_dev && h->cur_dev != h->volt_dev) {
        ina219_sample_t s;
        if (ina219_read(h->cur_dev, &s) == ESP_OK) {
            h->v_load_uv_cache = s.v_uv;
        }
    }
    /* The shorted sensor reads nothing but its own offset -- a live zero reference
     * that needs no load disconnection. */
    if (h->volt_dev && h->volt_dev != h->cur_dev) {
        ina219_sample_t s;
        if (ina219_read(h->volt_dev, &s) == ESP_OK) {
            h->idle_offset_cache = s.i_ua;
        }
    }
}

esp_err_t sensors_read(sensors_handle_t h, power_sample_t *out)
{
    ESP_RETURN_ON_FALSE(h && out, ESP_ERR_INVALID_ARG, TAG, "null");

    *out = (power_sample_t){0};
    out->t_us = esp_timer_get_time();

    if (h->role_state != SENSORS_ROLE_RESOLVED) {
        /* Not an error to report loudly on every pass -- it is the documented state
         * before a load has been seen. i_valid stays false so nothing integrates. */
        return ESP_ERR_INVALID_STATE;
    }

    h->pass++;

    /* Current first: its timestamp dates the sample, and a delayed current reading
     * distorts dt and therefore the integral (§4.5). */
    ina219_sample_t cs;
    const esp_err_t cerr = ina219_read(h->cur_dev, &cs);
    if (cerr == ESP_OK) {
        out->t_us       = cs.t_us;
        out->i_ua       = cs.i_ua;
        out->v_shunt_uv = cs.v_shunt_uv;
        out->pga        = cs.pga;
        out->saturated  = cs.saturated;
        out->i_valid    = true;
        if (h->cur_dev == h->volt_dev) {
            /* SINGLE mode: same device */
            h->v_pack_uv_cache = cs.v_uv;
            h->v_cache_valid   = true;
            out->v_is_fresh    = true;
        }
    }

    /* Voltage on its own divisor: it is never integrated, so paying full rate for it
     * would be power spent for nothing (§4.5). */
    if (h->volt_dev != h->cur_dev && (h->pass % h->cfg.voltage_divisor) == 0) {
        ina219_sample_t vs;
        if (ina219_read(h->volt_dev, &vs) == ESP_OK) {
            h->v_pack_uv_cache = vs.v_uv;
            h->v_cache_valid   = true;
            out->v_is_fresh    = true;
        }
    }

    if ((h->pass % h->cfg.diagnostic_divisor) == 0) {
        fill_diagnostics(h, out);
    }

    out->v_pack_uv      = h->v_pack_uv_cache;
    out->v_valid        = h->v_cache_valid;
    out->v_load_uv      = h->v_load_uv_cache;
    out->idle_offset_ua = h->idle_offset_cache;

    if (out->i_valid && out->v_valid) {
        out->p_uw = (int32_t)(((int64_t)out->i_ua * (int64_t)out->v_pack_uv) / 1000000);
    }

    if (!out->i_valid) {
        return cerr; /* propagate NOT_FINISHED / range discard / bus error verbatim */
    }
    return ESP_OK;
}

esp_err_t sensors_read_blocking(sensors_handle_t h, power_sample_t *out)
{
    ESP_RETURN_ON_FALSE(h && out, ESP_ERR_INVALID_ARG, TAG, "null");
    ESP_RETURN_ON_FALSE(h->role_state == SENSORS_ROLE_RESOLVED, ESP_ERR_INVALID_STATE,
                        TAG, "roles unresolved -- run detect or set the mode");

    *out = (power_sample_t){0};

    ina219_sample_t cs;
    ESP_RETURN_ON_ERROR(ina219_read_blocking(h->cur_dev, &cs), TAG, "current");
    out->t_us       = cs.t_us;
    out->i_ua       = cs.i_ua;
    out->v_shunt_uv = cs.v_shunt_uv;
    out->pga        = cs.pga;
    out->saturated  = cs.saturated;
    out->i_valid    = true;

    ina219_sample_t vs;
    if (h->volt_dev == h->cur_dev) {
        vs = cs;
    } else {
        ESP_RETURN_ON_ERROR(ina219_read_blocking(h->volt_dev, &vs), TAG, "voltage");
        out->idle_offset_ua = vs.i_ua;
    }
    out->v_pack_uv   = vs.v_uv;
    out->v_valid     = true;
    out->v_is_fresh  = true;
    h->v_pack_uv_cache = out->v_pack_uv;
    h->v_cache_valid   = true;

    out->p_uw = (int32_t)(((int64_t)out->i_ua * (int64_t)out->v_pack_uv) / 1000000);
    return ESP_OK;
}

/* --- detection -------------------------------------------------------------- */

static int32_t mean_abs_shunt_uv(ina219_handle_t d, uint32_t samples)
{
    int64_t  acc = 0;
    uint32_t n   = 0;
    for (uint32_t i = 0; i < samples; i++) {
        ina219_sample_t s;
        if (ina219_read_blocking(d, &s) != ESP_OK) {
            continue;
        }
        acc += (s.v_shunt_uv < 0) ? -s.v_shunt_uv : s.v_shunt_uv;
        n++;
    }
    return n ? (int32_t)(acc / (int64_t)n) : 0;
}

esp_err_t sensors_detect_mode(sensors_handle_t h, uint32_t samples,
                              int32_t *out_pos_uv, int32_t *out_neg_uv)
{
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "null");
    ESP_RETURN_ON_FALSE(h->pos && h->neg, ESP_ERR_INVALID_STATE, TAG,
                        "detection needs both sensors");
    if (samples == 0) {
        samples = 32;
    }

    /* Widen both devices so a large detection current cannot clip, and read the
     * differential each one sees. The device across the shunt tracks the load; the
     * shorted one does not move off its offset. */
    const ina219_pga_t saved_pos = ina219_get_pga(h->pos);
    const ina219_pga_t saved_neg = ina219_get_pga(h->neg);
    ina219_set_autorange(h->pos, false);
    ina219_set_autorange(h->neg, false);
    ina219_set_pga(h->pos, ina219_get_pga_max(h->pos));
    ina219_set_pga(h->neg, ina219_get_pga_max(h->neg));

    const int32_t pos_uv = mean_abs_shunt_uv(h->pos, samples);
    const int32_t neg_uv = mean_abs_shunt_uv(h->neg, samples);

    ina219_set_pga(h->pos, saved_pos);
    ina219_set_pga(h->neg, saved_neg);
    ina219_set_autorange(h->pos, true);
    ina219_set_autorange(h->neg, true);

    if (out_pos_uv) *out_pos_uv = pos_uv;
    if (out_neg_uv) *out_neg_uv = neg_uv;

    const int32_t hi = (pos_uv > neg_uv) ? pos_uv : neg_uv;
    const int32_t lo = (pos_uv > neg_uv) ? neg_uv : pos_uv;

    /*
     * Refuse on weak evidence rather than guessing. At zero current both readings are
     * near zero and the two modes are genuinely indistinguishable (DESIGN.md 2.10.6);
     * picking one would invert the sign of every subsequent measurement and the gauge
     * would be confidently wrong for as long as it ran.
     */
    if (hi < DETECT_MIN_UV || (int64_t)hi < (int64_t)lo * DETECT_RATIO) {
        ESP_LOGW(TAG,
                 "inconclusive: positive-pole %ld uV, negative-pole %ld uV. Apply a "
                 "larger load (need one channel >%d uV and >%dx the other).",
                 (long)pos_uv, (long)neg_uv, DETECT_MIN_UV, DETECT_RATIO);
        return ESP_ERR_INVALID_STATE;
    }

    const sensors_mode_t m = (pos_uv > neg_uv) ? SENSORS_MODE_P : SENSORS_MODE_N;
    ESP_LOGI(TAG, "detected %s (positive %ld uV vs negative %ld uV)",
             sensors_mode_str(m), (long)pos_uv, (long)neg_uv);
    return sensors_set_mode(h, m);
}

/* --- accessors -------------------------------------------------------------- */

esp_err_t sensors_set_mode(sensors_handle_t h, sensors_mode_t mode)
{
    ESP_RETURN_ON_FALSE(h && mode <= SENSORS_MODE_SINGLE, ESP_ERR_INVALID_ARG, TAG,
                        "bad mode");
    h->cfg.mode = mode;
    assign_roles(h);
    return (h->role_state == SENSORS_ROLE_RESOLVED || mode == SENSORS_MODE_AUTO)
               ? ESP_OK
               : ESP_ERR_INVALID_STATE;
}

sensors_mode_t       sensors_get_mode(sensors_handle_t h)       { return h->cfg.mode; }
sensors_role_state_t sensors_get_role_state(sensors_handle_t h) { return h->role_state; }
ina219_handle_t      sensors_current_dev(sensors_handle_t h)    { return h->cur_dev; }
ina219_handle_t      sensors_voltage_dev(sensors_handle_t h)    { return h->volt_dev; }
bool                 sensors_have_pos(sensors_handle_t h)       { return h->pos != NULL; }
bool                 sensors_have_neg(sensors_handle_t h)       { return h->neg != NULL; }
ina219_handle_t      sensors_pos_dev(sensors_handle_t h)        { return h->pos; }
ina219_handle_t      sensors_neg_dev(sensors_handle_t h)        { return h->neg; }

void sensors_report(sensors_handle_t h)
{
    ESP_LOGI(TAG, "mode        : %s", sensors_mode_str(h->cfg.mode));
    ESP_LOGI(TAG, "positive 0x%02X: %s", h->cfg.addr_pos_pole,
             h->pos ? "present" : "absent");
    ESP_LOGI(TAG, "negative 0x%02X: %s", h->cfg.addr_neg_pole,
             h->neg ? "present" : "absent");

    if (h->role_state != SENSORS_ROLE_RESOLVED) {
        ESP_LOGW(TAG, "roles UNRESOLVED -- not integrating. Apply a load and run "
                      "'detect', or set the mode explicitly.");
        return;
    }
    ESP_LOGI(TAG, "current from: %s",
             h->cur_dev == h->pos ? "positive pole" : "negative pole");
    ESP_LOGI(TAG, "voltage from: %s%s",
             h->volt_dev == h->pos ? "positive pole" : "negative pole",
             h->volt_dev == h->cur_dev ? " (shared, software bus correction on)" : "");
}
