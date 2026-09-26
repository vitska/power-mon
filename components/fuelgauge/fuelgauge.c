/*
 * fuelgauge.c — see fuelgauge.h.
 */

#include "fuelgauge.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "fg";

#define NS      "fg"
#define FG_VER  1

/* A gap longer than this integrates nothing: the interval becomes uncertainty rather
 * than a guess multiplied by a long dt (§5.2 gap guards). */
#define MAX_GAP_US   (5LL * 1000000)
#define DEAD_GAP_US  (60LL * 1000000)

/* Save when SoC has moved this far, or this long has passed. Both bounds matter: the
 * first keeps a fast discharge recorded, the second keeps a slow one from never being
 * written at all. Flash endurance is fine either way -- worst case is a few hundred
 * writes a day against a 100k-cycle part (§6.3). */
#define SAVE_SOC_DELTA_PERMILLE 5
#define SAVE_INTERVAL_S         300

/* Empty anchor (§5.4 B): the low voltage must HOLD this long. One sample is not an
 * empty pack -- a loose clamp, a VBUS lead off while wiring, or a sensor glitch reads
 * 0 V for a moment, and a single-sample anchor latches that as 0 % and saves it. */
#define EMPTY_HOLD_S 10

/* Below this fraction of v_0pct a reading is not a battery at its endpoint: nothing
 * with a 12 V chemistry rests at 8.6 V. It is an absent battery or a disconnected
 * VBUS, and it must not anchor anything. */
#define EMPTY_FLOOR_Q8 186 /* 0.73 */

static struct {
    fg_config_t cfg;

    int64_t  charge_uas;
    int64_t  rem_frac;      /* §5.2: exact remainder, no cumulative truncation */
    int64_t  cum_in_uas;
    int64_t  cum_out_uas;
    uint32_t full_capacity_uah;

    fg_state_t state;
    uint32_t   ocv_uv;
    int32_t    ir_drop_uv;
    bool       voltage_only;

    int64_t  last_t_us;
    int64_t  idle_since_us;  /* 0 = not idle */
    int64_t  full_since_us;  /* 0 = full condition not currently held */
    int64_t  empty_since_us; /* 0 = empty condition not currently held */
    uint32_t s_since_anchor;

    int64_t  q_since_full_uas;  /* unclamped, for capacity learning */
    bool     have_full_anchor;
    uint32_t learn_count;
    uint32_t last_learn_uah;
    uint32_t peukert_factor_q16;

    uint32_t saved_soc_permille;
    int64_t  last_save_us;
    bool     dirty;
} s_fg;

/* --- fixed-point pow, for Peukert --------------------------------------------- */

/*
 * (|i|/i_rated)^(k-1) with no floating point (DESIGN.md §4.3 keeps floats out).
 * Done as exp2((k-1) · log2(r)) in Q16. Worst-case error against the exact function
 * is about 0.4 % across a 0.05..10 ratio range -- two orders of magnitude tighter
 * than Peukert's constant is actually known for any real battery, so the
 * approximation is not the weak link.
 */
static int32_t log2_q16(uint32_t x)
{
    if (x == 0) {
        return INT32_MIN;
    }
    int32_t e = 0;
    while (x >= (2u << 16)) { x >>= 1; e++; }
    while (x <  (1u << 16)) { x <<= 1; e--; }

    /* Repeated squaring peels off one fractional bit at a time. */
    int32_t  frac = 0;
    uint64_t xx   = x;
    for (int i = 0; i < 16; i++) {
        xx = (xx * xx) >> 16;
        frac <<= 1;
        if (xx >= (2ull << 16)) {
            xx >>= 1;
            frac |= 1;
        }
    }
    return (e << 16) + frac;
}

static uint32_t exp2_q16(int32_t y)
{
    /* 2^(1/2^(i+1)) in Q16 -- generated, not typed by hand. */
    static const uint32_t root2[16] = {
        92682, 77936, 71468, 68438, 66971, 66250, 65892, 65714,
        65625, 65580, 65558, 65547, 65542, 65539, 65537, 65537,
    };
    const int32_t  ip = y >> 16;
    const uint32_t fp = (uint32_t)(y & 0xFFFF);

    uint64_t r = 1ull << 16;
    for (int i = 0; i < 16; i++) {
        if (fp & (0x8000u >> i)) {
            r = (r * root2[i]) >> 16;
        }
    }
    if (ip > 0) {
        r <<= ip;
    } else if (ip < 0) {
        r >>= -ip;
    }
    return (r > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)r;
}

/* --- helpers ------------------------------------------------------------------- */

static int64_t capacity_uas(void)
{
    return (int64_t)s_fg.full_capacity_uah * 3600;
}

static uint32_t soc_from_charge(void)
{
    const int64_t cap = capacity_uas();
    if (cap <= 0) {
        return 0;
    }
    int64_t p = (s_fg.charge_uas * 1000) / cap;
    if (p < 0)    p = 0;
    if (p > 1000) p = 1000;
    return (uint32_t)p;
}

/*
 * Voltage to SoC through the SHAPE of the lead-acid resting-OCV curve (§8.6,
 * v_curve_mode 1), scaled between the configured v_0pct and v_100pct.
 *
 * The shape is the common 25 °C chart for a 12 V flooded pack, taken at its range
 * midpoints: 0 % 11.50 V, 25 % 11.95 V, 50 % 12.25 V, 75 % 12.45 V, 100 % 12.70 V.
 * It is not a straight line -- the voltage climbs fast through the bottom quarter and
 * flattens toward full -- and the straight line this replaced read a rested 12.44 V
 * pack about ten points low.
 *
 * Stored as fractions of the window rather than as volts, so v0/v100 still move the
 * endpoints and the curve stretches with them: a pack whose owner sets a conservative
 * 0 % keeps the curve's shape rather than getting a table that disagrees with them.
 */
#define OCV_POINTS 5
static const uint32_t OCV_SHAPE_Q16[OCV_POINTS] = {
    0,     /*   0 %  11.50 V */
    24576, /*  25 %  11.95 V: (11.95 - 11.50) / 1.20 = 0.375 */
    40960, /*  50 %  12.25 V: 0.625 */
    51883, /*  75 %  12.45 V: 0.7917 */
    65536, /* 100 %  12.70 V */
};

static uint32_t soc_from_ocv(uint32_t ocv_uv)
{
    if (s_fg.cfg.v_100pct_uv <= s_fg.cfg.v_0pct_uv) {
        return 0;
    }
    if (ocv_uv <= s_fg.cfg.v_0pct_uv) {
        return 0;
    }
    if (ocv_uv >= s_fg.cfg.v_100pct_uv) {
        return 1000;
    }
    const uint64_t span = s_fg.cfg.v_100pct_uv - s_fg.cfg.v_0pct_uv;
    const uint32_t x    = (uint32_t)(((uint64_t)(ocv_uv - s_fg.cfg.v_0pct_uv) << 16) / span);

    for (int i = 0; i < OCV_POINTS - 1; i++) {
        const uint32_t lo = OCV_SHAPE_Q16[i], hi = OCV_SHAPE_Q16[i + 1];
        if (x <= hi) {
            const uint32_t step = 1000 / (OCV_POINTS - 1);
            return step * (uint32_t)i + (uint32_t)(((uint64_t)(x - lo) * step) / (hi - lo));
        }
    }
    return 1000;
}

/*
 * The current below which the pack counts as RESTING, for the OCV re-sync. Not the
 * integration deadband: that one exists to stop the counter integrating sensor noise
 * and is a few milliamps. Resting only has to mean "too little current for the
 * terminal voltage to be far from OCV", and a monitor that lives on the pack -- this
 * board, a clock, an alarm -- draws a steady few milliamps to a few tens forever. With
 * the deadband as the threshold, such a pack never rested, never re-synced, and a
 * wrong count stayed wrong indefinitely.
 *
 * C/400: 110 mA on 44 Ah. At that rate the I·R term is well under a millivolt after
 * compensation, and polarisation is a few millivolts -- a fraction of a percent of SoC.
 */
static uint32_t rest_current_ua(void)
{
    const uint32_t c400 = s_fg.cfg.design_capacity_uah / 400;
    return c400 > s_fg.cfg.i_deadband_ua ? c400 : s_fg.cfg.i_deadband_ua;
}

/*
 * Scales a discharge increment by the Peukert factor. Charge is never scaled: the
 * effect is asymmetric in the physics, and applying it to charging would quietly
 * inflate the count on every recharge.
 *
 * The ratio is clamped to 1/32..32 before the exponent. Peukert's law is an empirical
 * fit valid within a couple of decades of the rated rate; outside that it produces
 * confident nonsense, and a clamp is more honest than an extrapolation.
 */
static int64_t peukert_scale(int64_t dq_abs, int64_t i_abs_ua)
{
    s_fg.peukert_factor_q16 = 1u << 16;

    if (s_fg.cfg.peukert_q8 == 256 || s_fg.cfg.i_rated_ua == 0 || i_abs_ua <= 0) {
        return dq_abs;
    }

    uint64_t r = ((uint64_t)i_abs_ua << 16) / s_fg.cfg.i_rated_ua;
    const uint64_t lo = (1ull << 16) / 32;
    const uint64_t hi = 32ull << 16;
    if (r < lo) r = lo;
    if (r > hi) r = hi;

    const int32_t km1_q8 = (int32_t)s_fg.cfg.peukert_q8 - 256;
    const int32_t y      = (int32_t)(((int64_t)log2_q16((uint32_t)r) * km1_q8) / 256);
    const uint32_t f     = exp2_q16(y);

    s_fg.peukert_factor_q16 = f;
    return (dq_abs * (int64_t)f) >> 16;
}

static void set_charge_from_soc(uint32_t permille)
{
    s_fg.charge_uas = (capacity_uas() * (int64_t)permille) / 1000;
    s_fg.rem_frac   = 0;
}

/* --- persistence --------------------------------------------------------------- */

static esp_err_t store(void)
{
    nvs_handle_t h;
    esp_err_t    err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    /* Config and state in one namespace but distinct keys, so a config edit does not
     * rewrite the accumulators and a state commit does not touch the config. */
    /* The configuration is NOT written here. config.c owns every setting and
     * persists it in namespace `cfg`; a second copy in this namespace would be a
     * second answer to "what is this gauge set to", and the two would drift the
     * first time one of them was written without the other. What this namespace
     * keeps is the accumulated state below -- history, which nothing else owns. */
    if (err == ESP_OK) err = nvs_set_i64 (h, "q",   s_fg.charge_uas);
    if (err == ESP_OK) err = nvs_set_i64 (h, "in",  s_fg.cum_in_uas);
    if (err == ESP_OK) err = nvs_set_i64 (h, "out", s_fg.cum_out_uas);
    if (err == ESP_OK) err = nvs_set_u32 (h, "cap", s_fg.full_capacity_uah);
    if (err == ESP_OK) err = nvs_set_u32 (h, "lrn", s_fg.learn_count);
    if (err == ESP_OK) err = nvs_set_i64 (h, "qsf", s_fg.q_since_full_uas);
    if (err == ESP_OK) err = nvs_set_u8  (h, "haf", s_fg.have_full_anchor ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8  (h, "st",  (uint8_t)s_fg.state);
    if (err == ESP_OK) err = nvs_set_u8  (h, "ver", FG_VER);
    if (err == ESP_OK) err = nvs_commit(h);

    nvs_close(h);
    if (err == ESP_OK) {
        s_fg.dirty             = false;
        s_fg.saved_soc_permille = soc_from_charge();
    }
    return err;
}

esp_err_t fg_save(void)
{
    return store();
}

esp_err_t fg_init(void)
{
    memset(&s_fg, 0, sizeof(s_fg));
    s_fg.cfg               = FG_CONFIG_LEAD_ACID_12V_44AH();
    s_fg.full_capacity_uah = s_fg.cfg.design_capacity_uah;
    s_fg.state             = FG_UNKNOWN;
    s_fg.voltage_only      = true;

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no stored state -- lead-acid defaults, SoC from voltage");
        return ESP_OK;
    }

    uint8_t ver = 0;
    if (nvs_get_u8(h, "ver", &ver) != ESP_OK || ver != FG_VER) {
        nvs_close(h);
        ESP_LOGW(TAG, "stored state version %u ignored", ver);
        return ESP_OK;
    }

    /* Configuration is read from config.c, not from here -- see the note in
     * store(). An older store may still contain a "cfg" blob; it is ignored. */

    int64_t  i64;
    uint32_t u32;
    uint8_t  u8;
    if (nvs_get_u32(h, "cap", &u32) == ESP_OK && u32 > 0) s_fg.full_capacity_uah = u32;
    if (nvs_get_i64(h, "q",   &i64) == ESP_OK) {
        s_fg.charge_uas   = i64;
        s_fg.voltage_only = false;
    }
    if (nvs_get_i64(h, "in",  &i64) == ESP_OK) s_fg.cum_in_uas  = i64;
    if (nvs_get_i64(h, "out", &i64) == ESP_OK) s_fg.cum_out_uas = i64;
    if (nvs_get_u8 (h, "st",  &u8)  == ESP_OK) s_fg.state = (fg_state_t)u8;
    if (nvs_get_u32(h, "lrn", &u32) == ESP_OK) s_fg.learn_count = u32;
    if (nvs_get_i64(h, "qsf", &i64) == ESP_OK) s_fg.q_since_full_uas = i64;
    if (nvs_get_u8 (h, "haf", &u8)  == ESP_OK) s_fg.have_full_anchor = (u8 != 0);
    nvs_close(h);

    /*
     * A restored count is stale by however long the power was off, and there is no way
     * to know that from here -- so the state is kept but marked un-anchored, and the
     * first rest period re-syncs it against OCV. Trusting a count across an unknown
     * outage is how a gauge ends up confidently wrong.
     */
    s_fg.s_since_anchor = UINT32_MAX;
    s_fg.saved_soc_permille = soc_from_charge();

    /*
     * NEVER %lld here. CONFIG_NEWLIB_NANO_FORMAT drops %ll support, and the failure is
     * not a cosmetic one: the formatter consumes the wrong number of vararg bytes, so
     * every later conversion reads from the wrong stack offset. A trailing %s then
     * dereferences a garbage pointer -- which is exactly how this line boot-looped the
     * board with a load access fault. Print 64-bit values as mAh in 32 bits instead.
     */
    ESP_LOGI(TAG, "restored: %lu.%lu%%, %ld mAh of %lu uAh, state %s",
             (unsigned long)(soc_from_charge() / 10),
             (unsigned long)(soc_from_charge() % 10),
             (long)(s_fg.charge_uas / 3600000),
             (unsigned long)s_fg.full_capacity_uah,
             fg_state_str(s_fg.state));
    return ESP_OK;
}

/* --- the gauge ----------------------------------------------------------------- */

void fg_update(const power_sample_t *s)
{
    if (!s || !s->i_valid) {
        return;
    }

    const int64_t now = s->t_us;
    const int64_t dt  = (s_fg.last_t_us == 0) ? 0 : (now - s_fg.last_t_us);
    s_fg.last_t_us    = now;

    /* --- I·R compensation, the basis of every voltage decision below ---------- */
    if (s->v_valid) {
        const int64_t drop =
            ((int64_t)s->i_ua * (int64_t)s_fg.cfg.r_int_uohm) / 1000000;
        s_fg.ir_drop_uv = (int32_t)drop;

        int64_t ocv = (int64_t)s->v_pack_uv - drop;
        if (ocv < 0) {
            ocv = 0;
        }
        s_fg.ocv_uv = (uint32_t)ocv;
    }

    /* --- integrate (§5.2) ----------------------------------------------------- */
    const bool in_deadband =
        (s->i_ua > -(int32_t)s_fg.cfg.i_deadband_ua &&
         s->i_ua < (int32_t)s_fg.cfg.i_deadband_ua);

    if (dt > 0 && dt < DEAD_GAP_US && !in_deadband) {
        /* A gap between MAX_GAP_US and DEAD_GAP_US still integrates -- at the last
         * known current, which is the honest choice when the alternative is to
         * discard real charge -- but it is not treated as an anchor. */
        const int64_t num  = (int64_t)s->i_ua * dt + s_fg.rem_frac;
        const int64_t dq   = num / 1000000;
        s_fg.rem_frac      = num % 1000000;

        /* Lifetime counters take the RAW coulombs. These answer "how much charge
         * passed through the shunt", which is a measurement, not a model. */
        if (dq > 0) {
            s_fg.cum_in_uas += dq;
        } else {
            s_fg.cum_out_uas -= dq;
        }

        /* The SoC count takes the Peukert-adjusted value, which answers the different
         * question of "how much capacity did that consume". */
        int64_t dq_eff = dq;
        if (dq < 0) {
            dq_eff = -peukert_scale(-dq, -(int64_t)s->i_ua);
        }

        s_fg.charge_uas       += dq_eff;
        s_fg.q_since_full_uas += dq_eff;

        /* Clamp to the physical range. Hitting a clamp is information, not an error:
         * it means the count and the pack have diverged, and the next OCV anchor is
         * what fixes it. */
        if (s_fg.charge_uas < 0) {
            s_fg.charge_uas = 0;
        } else if (s_fg.charge_uas > capacity_uas()) {
            s_fg.charge_uas = capacity_uas();
        }
        s_fg.voltage_only = false;
        s_fg.dirty        = true;
    }

    /* --- idle / rest tracking ------------------------------------------------- */
    const bool at_rest = s->i_ua > -(int32_t)rest_current_ua() &&
                         s->i_ua < (int32_t)rest_current_ua();
    if (at_rest) {
        if (s_fg.idle_since_us == 0) {
            s_fg.idle_since_us = now;
        }
    } else {
        s_fg.idle_since_us = 0;
        if (s_fg.state == FG_RESTING) {
            s_fg.state = FG_COUNTING;
        }
    }

    if (dt > 0) {
        const uint32_t secs = (uint32_t)(dt / 1000000);
        if (s_fg.s_since_anchor != UINT32_MAX) {
            s_fg.s_since_anchor += secs;
        }
    }

    /* --- full detection: absorption voltage AND taper current (§5.4 A) -------- */
    const bool charging = s->i_ua > (int32_t)s_fg.cfg.i_deadband_ua;
    if (s->v_valid && charging && s->v_pack_uv >= s_fg.cfg.v_full_uv &&
        (uint32_t)s->i_ua <= s_fg.cfg.i_taper_ua) {
        if (s_fg.full_since_us == 0) {
            s_fg.full_since_us = now;
        } else if ((now - s_fg.full_since_us) >=
                   (int64_t)s_fg.cfg.t_full_hold_s * 1000000) {
            /* Three conditions together, held: this is the strongest anchor a gauge
             * gets, so it snaps rather than blends. */
            s_fg.charge_uas     = capacity_uas();
            s_fg.rem_frac       = 0;
            s_fg.state          = FG_FULL;
            s_fg.s_since_anchor = 0;
            s_fg.voltage_only   = false;
            s_fg.dirty          = true;
            /* Open a learning window: from a known-full pack, the charge that comes
             * out before the empty anchor IS the capacity. */
            s_fg.q_since_full_uas = 0;
            s_fg.have_full_anchor = true;
        }
    } else {
        s_fg.full_since_us = 0;
        if (s_fg.state == FG_FULL && !charging) {
            s_fg.state = FG_COUNTING;
        }
    }

    /* --- empty detection: at the endpoint under load (§5.4 B) ----------------- */
    const bool discharging = s->i_ua < -(int32_t)s_fg.cfg.i_deadband_ua;
    const uint32_t floor_uv =
        (uint32_t)(((uint64_t)s_fg.cfg.v_0pct_uv * EMPTY_FLOOR_Q8) / 256);
    const bool empty_now = s->v_valid && discharging && s->v_pack_uv >= floor_uv &&
                           s_fg.ocv_uv <= s_fg.cfg.v_0pct_uv;
    if (!empty_now) {
        s_fg.empty_since_us = 0;
    } else if (s_fg.empty_since_us == 0) {
        s_fg.empty_since_us = now;
    }
    if (empty_now &&
        (now - s_fg.empty_since_us) >= (int64_t)EMPTY_HOLD_S * 1000000) {
        s_fg.empty_since_us = 0;
        /*
         * CAPACITY LEARNING (§5.4). A full anchor followed by an empty anchor brackets
         * a complete discharge, and the effective charge that flowed between them is
         * the pack's real capacity.
         *
         * Learned only in this direction, never full-from-empty: lead-acid coulombic
         * efficiency is well under 100 %, so charge going IN exceeds the capacity it
         * restores and learning from a recharge would inflate the figure every cycle.
         */
        if (s_fg.have_full_anchor) {
            const int64_t consumed = -s_fg.q_since_full_uas; /* positive */
            const int64_t needed   =
                (capacity_uas() * (int64_t)s_fg.cfg.learn_min_depth_permille) / 1000;

            if (consumed >= needed && consumed > 0) {
                const uint32_t measured_uah = (uint32_t)(consumed / 3600);
                s_fg.last_learn_uah = measured_uah;

                /* Reject the implausible before blending: a capacity outside half to
                 * one-and-a-half times nameplate is a measurement fault (a missed
                 * anchor, a mis-set shunt), not a battery that changed. */
                const uint32_t lo = s_fg.cfg.design_capacity_uah / 2;
                const uint32_t hi = s_fg.cfg.design_capacity_uah +
                                    s_fg.cfg.design_capacity_uah / 2;
                if (measured_uah >= lo && measured_uah <= hi) {
                    const int64_t delta =
                        (int64_t)measured_uah - (int64_t)s_fg.full_capacity_uah;
                    s_fg.full_capacity_uah = (uint32_t)(
                        (int64_t)s_fg.full_capacity_uah +
                        (delta * (int64_t)s_fg.cfg.learn_blend_q8) / 256);
                    s_fg.learn_count++;
                    /* All three narrowed to 32 bits deliberately: see the
                     * nano-format note in fg_init(). */
                    ESP_LOGI(TAG, "learned capacity: measured %lu uAh over %lu%% "
                                  "depth -> %lu uAh (blended)",
                             (unsigned long)measured_uah,
                             (unsigned long)((consumed * 100) / capacity_uas()),
                             (unsigned long)s_fg.full_capacity_uah);
                } else {
                    ESP_LOGW(TAG, "capacity measurement %lu uAh implausible against "
                                  "%lu nameplate -- ignored",
                             (unsigned long)measured_uah,
                             (unsigned long)s_fg.cfg.design_capacity_uah);
                }
            }
            s_fg.have_full_anchor = false; /* one learn per full-to-empty span */
        }

        s_fg.charge_uas     = 0;
        s_fg.rem_frac       = 0;
        s_fg.state          = FG_EMPTY;
        s_fg.s_since_anchor = 0;
        s_fg.voltage_only   = false;
        s_fg.dirty          = true;
    }

    /* --- resting OCV re-sync (§5.4) ------------------------------------------- */
    if (s->v_valid && s_fg.idle_since_us != 0 &&
        (now - s_fg.idle_since_us) >= (int64_t)s_fg.cfg.t_rest_s * 1000000) {

        const uint32_t ocv_soc = soc_from_ocv(s_fg.ocv_uv);

        /* A count carried over from before boot has never been checked against this
         * pack in this power-up (see fg_init), so the first rest replaces it rather
         * than nudging it: blending a stale 0 % toward the truth at 25 % per rest
         * period takes hours to undo. */
        if (s_fg.voltage_only || s_fg.state == FG_UNKNOWN ||
            s_fg.s_since_anchor == UINT32_MAX) {
            /* No count worth keeping: take the voltage estimate outright. */
            set_charge_from_soc(ocv_soc);
            s_fg.voltage_only = false;
        } else {
            /* Blend, do not snap. A rested OCV is good but not exact, and snapping
             * would make the displayed SoC jump every time the load goes away. */
            const int64_t target = (capacity_uas() * (int64_t)ocv_soc) / 1000;
            const int64_t delta  = target - s_fg.charge_uas;
            s_fg.charge_uas += (delta * (int64_t)s_fg.cfg.ocv_blend_q8) / 256;
        }
        s_fg.state          = FG_RESTING;
        s_fg.s_since_anchor = 0;
        s_fg.idle_since_us  = now; /* re-arm; blend again after another rest period */
        s_fg.dirty          = true;
    } else if (s_fg.state == FG_UNKNOWN && s->v_valid) {
        /*
         * First-ever reading and nothing stored: seed from the compensated voltage so
         * the display is useful immediately. Marked voltage_only, so the first real
         * rest replaces it rather than blending against a guess.
         */
        set_charge_from_soc(soc_from_ocv(s_fg.ocv_uv));
        s_fg.state        = FG_COUNTING;
        s_fg.voltage_only = true;
    }

    /* --- persistence policy (§6.3) ------------------------------------------- */
    if (s_fg.dirty) {
        const uint32_t soc = soc_from_charge();
        const uint32_t moved = (soc > s_fg.saved_soc_permille)
                                   ? (soc - s_fg.saved_soc_permille)
                                   : (s_fg.saved_soc_permille - soc);
        const bool due = (now - s_fg.last_save_us) >=
                         (int64_t)SAVE_INTERVAL_S * 1000000;
        if (moved >= SAVE_SOC_DELTA_PERMILLE || due) {
            s_fg.last_save_us = now;
            store();
        }
    }
}

void fg_get(fg_status_t *out)
{
    if (!out) {
        return;
    }
    out->state             = s_fg.state;
    out->rest_current_ua   = rest_current_ua();
    out->soc_permille      = soc_from_charge();
    out->charge_uas        = s_fg.charge_uas;
    out->full_capacity_uah = s_fg.full_capacity_uah;
    out->ocv_uv            = s_fg.ocv_uv;
    out->ir_drop_uv        = s_fg.ir_drop_uv;
    out->cum_in_uas        = s_fg.cum_in_uas;
    out->cum_out_uas       = s_fg.cum_out_uas;
    out->s_since_anchor    = s_fg.s_since_anchor;
    out->voltage_only      = s_fg.voltage_only;
    out->design_capacity_uah = s_fg.cfg.design_capacity_uah;
    out->learn_count       = s_fg.learn_count;
    out->last_learn_uah    = s_fg.last_learn_uah;
    out->q_since_full_uas  = s_fg.q_since_full_uas;
    out->have_full_anchor  = s_fg.have_full_anchor;
    out->peukert_factor_q16 = s_fg.peukert_factor_q16 ? s_fg.peukert_factor_q16
                                                      : (1u << 16);
}

fg_config_t fg_get_config(void)
{
    return s_fg.cfg;
}

esp_err_t fg_set_config(const fg_config_t *cfg)
{
    if (!cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Refuse the shapes that would divide by zero or invert the scale, rather than
     * discovering them as a nonsense SoC later. */
    if (cfg->design_capacity_uah == 0 ||
        cfg->v_100pct_uv <= cfg->v_0pct_uv ||
        cfg->v_full_uv < cfg->v_100pct_uv ||
        cfg->ocv_blend_q8 == 0 || cfg->ocv_blend_q8 > 256 ||
        cfg->learn_blend_q8 == 0 || cfg->learn_blend_q8 > 256 ||
        cfg->learn_min_depth_permille < 100 ||
        cfg->learn_min_depth_permille > 1000) {
        return ESP_ERR_INVALID_ARG;
    }
    /* k below 1.0 would mean a fast discharge yields MORE capacity. 2.0 is already
     * far past any real chemistry. */
    if (cfg->peukert_q8 < 256 || cfg->peukert_q8 > 512) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint32_t old_cap = s_fg.cfg.design_capacity_uah;
    s_fg.cfg = *cfg;

    /* A capacity change rescales the learned capacity with it, so SoC as a percentage
     * is preserved rather than jumping because the denominator moved. */
    if (old_cap != cfg->design_capacity_uah && old_cap > 0) {
        const uint32_t soc = soc_from_charge();
        s_fg.full_capacity_uah = cfg->design_capacity_uah;
        set_charge_from_soc(soc);
    }
    return store();
}

esp_err_t fg_set_soc_permille(uint32_t permille)
{
    if (permille > 1000) {
        return ESP_ERR_INVALID_ARG;
    }
    set_charge_from_soc(permille);
    s_fg.state          = FG_COUNTING;
    s_fg.voltage_only   = false;
    s_fg.s_since_anchor = 0;
    return store();
}

esp_err_t fg_set_full(void)
{
    s_fg.charge_uas     = capacity_uas();
    s_fg.rem_frac       = 0;
    s_fg.state          = FG_FULL;
    s_fg.voltage_only   = false;
    s_fg.s_since_anchor = 0;
    return store();
}

esp_err_t fg_reset(void)
{
    s_fg.charge_uas   = 0;
    s_fg.rem_frac     = 0;
    s_fg.cum_in_uas   = 0;
    s_fg.cum_out_uas  = 0;
    s_fg.state        = FG_UNKNOWN;
    s_fg.voltage_only = true;
    s_fg.full_capacity_uah = s_fg.cfg.design_capacity_uah;
    /* A reset abandons the learn window too: the span it was measuring is no longer
     * bracketed by a trustworthy full anchor. */
    s_fg.q_since_full_uas = 0;
    s_fg.have_full_anchor = false;
    return store();
}

const char *fg_state_str(fg_state_t s)
{
    switch (s) {
    case FG_COUNTING: return "COUNTING";
    case FG_RESTING:  return "RESTING";
    case FG_FULL:     return "FULL";
    case FG_EMPTY:    return "EMPTY";
    case FG_UNKNOWN:
    default:          return "UNKNOWN";
    }
}
