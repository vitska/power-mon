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

/* --- calibration -------------------------------------------------------------- */

/*
 * Integer square root, Newton's method. Avoiding sqrt() keeps the float ban whole
 * (DESIGN.md §4.3). The loop condition is `y < g`, not `y != g`: the naive form
 * oscillates forever between two adjacent values for an input whose root is not
 * exact, which on a console command would simply hang the calling task.
 */
static uint64_t isqrt64(uint64_t x)
{
    if (x == 0) {
        return 0;
    }
    uint64_t g = x;
    uint64_t y = (g + 1) / 2;
    while (y < g) {
        g = y;
        y = (g + x / g) / 2;
    }
    return g;
}

esp_err_t sensors_average(sensors_handle_t h, uint32_t n,
                         int64_t *sum_i_ua, int64_t *sum_v_uv,
                         uint32_t *got, bool *saturated,
                         sensors_progress_cb_t progress, void *progress_ctx)
{
    ESP_RETURN_ON_FALSE(h && sum_i_ua && sum_v_uv && got && saturated, ESP_ERR_INVALID_ARG,
                        TAG, "null");
    *sum_i_ua  = 0;
    *sum_v_uv  = 0;
    *got       = 0;
    *saturated = false;

    /*
     * Freeze auto-ranging for the duration.
     *
     * Two reasons, and the second one is why this is not merely an optimisation. A
     * range change discards the next conversion, so a thrashing autoranger makes every
     * blocking read burn its retry budget -- measured at ~2.5 s per sample near zero
     * current, which turned a 64-sample average into well over a minute. And a
     * measurement whose scale changes underneath it is a worse measurement: the range
     * that was correct when averaging started is the right one to keep.
     */
    const bool auto_c = h->cur_dev  ? ina219_get_autorange(h->cur_dev)  : false;
    const bool auto_v = h->volt_dev ? ina219_get_autorange(h->volt_dev) : false;
    if (h->cur_dev)                        ina219_set_autorange(h->cur_dev, false);
    if (h->volt_dev && h->volt_dev != h->cur_dev) ina219_set_autorange(h->volt_dev, false);

    esp_err_t ret = ESP_OK;
    for (uint32_t k = 0; k < n; k++) {
        power_sample_t s;
        ret = sensors_read_blocking(h, &s);
        if (ret != ESP_OK) {
            break;
        }
        if (s.i_valid) {
            *sum_i_ua += s.i_ua;
        }
        if (s.saturated) {
            *saturated = true;
        }
        if (s.v_valid) {
            *sum_v_uv += s.v_pack_uv;
        }
        (*got)++;
        if (progress) {
            progress(k, progress_ctx);
        }
    }

    if (h->cur_dev)                        ina219_set_autorange(h->cur_dev, auto_c);
    if (h->volt_dev && h->volt_dev != h->cur_dev) ina219_set_autorange(h->volt_dev, auto_v);
    return ret;
}

bool sensors_solve_gain_ppm(int64_t measured, int64_t reference, uint32_t old_ppm,
                           uint32_t *new_ppm)
{
    if (!new_ppm) {
        return false;
    }
    const int64_t am = measured < 0 ? -measured : measured;
    const int64_t ar = reference < 0 ? -reference : reference;

    /* No validation: the entered reference is trusted as-is, at whatever magnitude
     * and sign it was given. Only a literal zero reading is refused, since a ratio
     * against it is not a number. */
    if (am == 0) {
        return false;
    }
    *new_ppm = (uint32_t)(((int64_t)old_ppm * ar) / am);
    return true;
}

esp_err_t sensors_calibrate_shunt(sensors_handle_t h, int64_t known_ua, uint32_t n_samples,
                                  sensors_progress_cb_t progress, void *progress_ctx,
                                  sensors_shunt_cal_t *out)
{
    ESP_RETURN_ON_FALSE(h && out, ESP_ERR_INVALID_ARG, TAG, "null");
    *out = (sensors_shunt_cal_t){0};

    const int64_t aref = known_ua < 0 ? -known_ua : known_ua;
    if (aref == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ina219_handle_t pos = h->pos;
    ina219_handle_t neg = h->neg;

    int64_t  sum_p = 0, sum_n = 0;
    uint32_t got_p = 0, got_n = 0;
    bool     sat_p = false, sat_n = false;
    for (uint32_t i = 0; i < n_samples; i++) {
        ina219_sample_t smp;
        if (pos && ina219_read_blocking(pos, &smp) == ESP_OK) {
            sum_p += smp.v_shunt_uv;
            got_p++;
            sat_p |= smp.saturated;
        }
        if (neg && ina219_read_blocking(neg, &smp) == ESP_OK) {
            sum_n += smp.v_shunt_uv;
            got_n++;
            sat_n |= smp.saturated;
        }
        if (progress) {
            progress(i, progress_ctx);
        }
    }

    const int64_t v_p = got_p ? sum_p / (int64_t)got_p : 0;
    const int64_t v_n = got_n ? sum_n / (int64_t)got_n : 0;
    const int64_t a_p = v_p < 0 ? -v_p : v_p;
    const int64_t a_n = v_n < 0 ? -v_n : v_n;

    out->v_pos_uv = v_p;
    out->v_neg_uv = v_n;
    out->got_pos  = got_p;
    out->got_neg  = got_n;
    out->sat_pos  = sat_p;
    out->sat_neg  = sat_n;

    /* No validation: whichever sensor sees the larger magnitude is used, whatever
     * that magnitude is. Only "neither sensor exists" stops it -- nothing to read. */
    const bool      use_neg = got_n && (!got_p || a_n >= a_p);
    ina219_handle_t cd      = use_neg ? neg : pos;
    const int64_t   v       = use_neg ? v_n : v_p;
    if (!cd) {
        return ESP_ERR_NOT_FOUND;
    }

    /* A zero point on this same chip: keep it as a voltage (offset * R, before the
     * sign), take it out of the reading, carry it to the new resistance. */
    const bool    same_dev = cd == h->cur_dev;
    const int64_t r_old    = ina219_get_shunt_uohm(cd);
    const int64_t v_off    = same_dev ? ((int64_t)ina219_get_offset_ua(cd) * r_old) / 1000000 : 0;
    const int64_t vc       = v - v_off;
    const int64_t avc      = vc < 0 ? -vc : vc;

    /* R in uOhm = V[uV] / I[uA] * 1e6. No range check: whatever this works out to is
     * set, and the driver's own floor (ina219_set_shunt_uohm) is the only backstop. */
    const int64_t r_new = (avc * 1000000LL) / aref;

    /* Mode first: it decides which device the rest applies to. */
    const sensors_mode_t want_mode = use_neg ? SENSORS_MODE_N : SENSORS_MODE_P;
    out->mode_changed = (h->cfg.mode != want_mode) && got_p && got_n;
    out->mode          = want_mode;
    if (out->mode_changed) {
        sensors_set_mode(h, want_mode);
    }

    /* Raw voltage and reference must agree in sign after the board's own inversion. */
    const bool invert = (vc < 0) != (known_ua < 0);
    ina219_set_invert_sign(cd, invert);
    ina219_set_shunt_uohm(cd, (uint32_t)r_new);
    const int32_t new_offset = (same_dev && r_new) ? (int32_t)((v_off * 1000000) / r_new) : 0;
    ina219_set_offset_ua(cd, new_offset);
    ina219_set_gain_ppm(cd, 1000000);

    out->dev        = cd;
    out->inverted    = invert;
    out->shunt_uohm  = (uint32_t)r_new;
    out->offset_ua   = new_offset;
    return ESP_OK;
}

esp_err_t sensors_zero_current(sensors_handle_t h, uint32_t n_samples,
                               int32_t *out_offset_ua, int32_t *out_stddev_ua,
                               sensors_progress_cb_t progress, void *progress_ctx)
{
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "null");
    ina219_handle_t dev = h->cur_dev;
    if (!dev) {
        ESP_LOGE(TAG, "no current sensor -- roles unresolved");
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Force the finest range: the offset we are measuring is a shunt-voltage offset,
     * and PGA/1 resolves it 8x better than PGA/8. Unity gain while measuring: convert()
     * subtracts the offset BEFORE applying gain, so an offset measured through a
     * non-unity gain is stored in the wrong domain and comes out scaled by 1/gain. It
     * cancels only when gain happens to be exactly 1.0 -- which it is on a fresh board,
     * and is not once the gain has been trimmed, so re-running this after a gain trim
     * would otherwise be subtly wrong. Both restored below with the range settings.
     */
    const ina219_pga_t saved_pga  = ina219_get_pga(dev);
    const bool         saved_auto = ina219_get_autorange(dev);
    const int32_t      saved_off  = ina219_get_offset_ua(dev);
    const uint32_t     saved_gain = ina219_get_gain_ppm(dev);

    ina219_set_autorange(dev, false);
    ina219_set_pga(dev, INA219_PGA_1);
    ina219_set_offset_ua(dev, 0); /* measure raw, not residual */
    ina219_set_gain_ppm(dev, 1000000);

    int64_t   sum = 0, sum_sq = 0;
    uint32_t  got = 0;
    esp_err_t err = ESP_OK;
    for (uint32_t i = 0; i < n_samples; i++) {
        ina219_sample_t s;
        err = ina219_read_blocking(dev, &s);
        if (err != ESP_OK) {
            break;
        }
        sum    += s.i_ua;
        sum_sq += (int64_t)s.i_ua * (int64_t)s.i_ua;
        got++;
        if (progress) {
            progress(i, progress_ctx);
        }
    }

    const int32_t mean = got ? (int32_t)(sum / (int64_t)got) : 0;
    int32_t       stddev = 0;
    if (got >= 2) {
        /* var = E[x^2] - E[x]^2, population variance. Adequate here; the sample-vs-
         * population distinction is noise next to the measurement it describes. */
        int64_t var = (sum_sq / (int64_t)got) - ((int64_t)mean * (int64_t)mean);
        if (var < 0) {
            var = 0; /* can only be rounding */
        }
        stddev = (int32_t)isqrt64((uint64_t)var);
    }

    if (out_offset_ua) *out_offset_ua = mean;
    if (out_stddev_ua) *out_stddev_ua = stddev;

    /* Restore range and gain regardless of outcome. */
    ina219_set_autorange(dev, saved_auto);
    ina219_set_pga(dev, saved_pga);
    ina219_set_gain_ppm(dev, saved_gain);

    if (err != ESP_OK) {
        ina219_set_offset_ua(dev, saved_off);
        return err;
    }

    /* No validation: the measured mean is baked in as the offset regardless of
     * spread. A noisy sample (current actually flowing) is on the person running
     * this, not something the firmware second-guesses. */
    ina219_set_offset_ua(dev, mean);
    return ESP_OK;
}

/*
 * The voltage-channel twin of the above. Same shape, three differences worth naming:
 *
 *  - There is no range to force. The bus channel has one fixed 4 mV/LSB scale, so
 *    nothing corresponds to dropping the PGA to /1 for resolution.
 *  - Gain is forced to unity for the same reason it is above: convert() subtracts the
 *    offset BEFORE gain, so an offset measured through a non-unity gain is stored in
 *    the wrong domain.
 *  - No validation on magnitude, deliberately: a caller expecting VBUS at ground and
 *    getting a floating 3.4 V instead bakes that in. That used to be rejected here;
 *    it no longer is, per the same "trust what was measured" policy as every other
 *    calibration path in this file.
 */
esp_err_t sensors_zero_voltage(sensors_handle_t h, uint32_t n_samples,
                               int32_t *out_offset_uv, uint32_t *out_spread_uv,
                               sensors_progress_cb_t progress, void *progress_ctx)
{
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "null");
    ina219_handle_t dev = h->volt_dev;
    if (!dev) {
        ESP_LOGE(TAG, "no voltage sensor -- roles unresolved");
        return ESP_ERR_INVALID_STATE;
    }

    const int32_t  saved_off  = ina219_get_vbus_offset_uv(dev);
    const uint32_t saved_gain = ina219_get_vbus_gain_ppm(dev);

    ina219_set_vbus_offset_uv(dev, 0);
    ina219_set_vbus_gain_ppm(dev, 1000000);

    int64_t   sum = 0;
    uint32_t  lo = UINT32_MAX, hi = 0, got = 0;
    esp_err_t err = ESP_OK;

    for (uint32_t i = 0; i < n_samples; i++) {
        power_sample_t s;
        err = sensors_read_blocking(h, &s);
        if (err != ESP_OK) {
            break;
        }
        if (!s.v_valid) {
            continue;
        }
        sum += s.v_pack_uv;
        if (s.v_pack_uv < lo) lo = s.v_pack_uv;
        if (s.v_pack_uv > hi) hi = s.v_pack_uv;
        got++;
        if (progress) {
            progress(i, progress_ctx);
        }
    }

    if (err != ESP_OK || got == 0) {
        ina219_set_vbus_offset_uv(dev, saved_off);
        ina219_set_vbus_gain_ppm(dev, saved_gain);
        return err != ESP_OK ? err : ESP_ERR_INVALID_STATE;
    }

    const int32_t mean = (int32_t)(sum / (int64_t)got);
    if (out_offset_uv) *out_offset_uv = mean;
    if (out_spread_uv) *out_spread_uv = (lo <= hi) ? (hi - lo) : 0;

    ina219_set_vbus_gain_ppm(dev, saved_gain);

    /* No validation: the measured mean is baked in as the offset regardless of
     * magnitude, whether or not VBUS was actually at ground. */
    ina219_set_vbus_offset_uv(dev, mean);
    return ESP_OK;
}
