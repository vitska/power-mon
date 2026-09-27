/*
 * fuelgauge.c — see fuelgauge.h.
 */

#include "fuelgauge.h"

#include <string.h>
#include <strings.h>

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

/* State decisions run on a ~10 s average of the current, not on single samples, and
 * with hysteresis: a directional state is entered above TWICE the rest current and
 * left only below it. A current hovering near the rest threshold -- a monitor's own
 * draw, a trickle, sensor noise -- therefore stays SETTLING and its countdown keeps
 * running, instead of flipping CHARGE/SETTLING/DISCHARGE and restarting it. */
#define CLASS_TAU_S 10
/* The time estimate uses a slower ~60 s average, so a load switching on for a moment
 * does not make "time to empty" jump. */
#define EST_TAU_S   60
#define EST_MAX_S   (99 * 24 * 3600) /* past this an estimate says nothing */

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
    bool     filt_init;
    int64_t  i_class_ua;     /* ~10 s average: what the state machine decides on */
    int64_t  i_est_ua;       /* ~60 s average: the rate the time estimate uses */
    int32_t  t_full_s, t_empty_s, settle_s; /* -1 = not applicable */
    int64_t  idle_since_us;  /* SETTLING/REST: when the settle timer last (re)started */
    int64_t  full_since_us;  /* ABSORB: 0 = full-hold window not running */
    int64_t  full_sum_ua;    /* current summed over the full-hold window */
    uint32_t full_n;
    int64_t  empty_since_us; /* DISCHARGE: 0 = empty condition not currently held */
    uint32_t s_since_anchor;
    int64_t  anchor_rem_us;  /* sub-second remainder carried into s_since_anchor */

    /* Capacity learning: the span since the last reference point (FULL, EMPTY or a
     * settled REST). q_since_ref is the effective (Peukert-adjusted) net charge, q_in
     * the raw charge that went IN during it -- a span with real charging inside it is
     * not a clean discharge measurement. */
    bool     have_ref;
    uint32_t ref_soc_permille;
    int64_t  q_since_ref_uas;
    int64_t  q_in_since_ref_uas;
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
 * CHEMISTRY PROFILES. Resting (open-circuit) voltage per cell at 0, 10 ... 100 %, 25 °C,
 * from the published charts for each chemistry; lead-acid flooded is the 12 V chart
 * this gauge was verified against (11.50 / 11.95 / 12.25 / 12.45 / 12.70 V at 0/25/50/
 * 75/100 %), divided by six. None of these is exact for any particular cell -- brand,
 * age and temperature all move them by tens of millivolts -- which is why the count,
 * not the voltage, is the primary SoC source, and why v0/v100 remain settable.
 *
 * v_full is what a charger's absorption / CV stage reaches; with the taper current it
 * is the full anchor. For lithium it sits just under the 100 % resting voltage the
 * validator requires it to meet, since a cell at CV relaxes to about that.
 */
static const fg_chem_profile_t CHEM[FG_CHEM_COUNT] = {
    [FG_CHEM_FLOODED] = {
        .key = "flooded", .name = "Lead-acid (flooded)",
        .ocv_cell_uv = {1916667, 1946667, 1976667, 2001667, 2021667, 2041667,
                        2055000, 2068333, 2083333, 2100000, 2116667},
        .v_full_cell_uv = 2400000, .taper_div = 30, .rated_div = 20,
        .peukert_q8 = 294, .t_rest_s = 600,
        .trust_lo_permille = 1000, .trust_hi_permille = 1000,
    },
    [FG_CHEM_AGM] = {
        .key = "agm", .name = "Lead-acid (AGM)",
        .ocv_cell_uv = {1966667, 1983333, 2000000, 2021667, 2041667, 2058333,
                        2075000, 2091667, 2108333, 2125000, 2141667},
        .v_full_cell_uv = 2400000, .taper_div = 50, .rated_div = 20,
        .peukert_q8 = 282, .t_rest_s = 600,
        .trust_lo_permille = 1000, .trust_hi_permille = 1000,
    },
    [FG_CHEM_GEL] = {
        .key = "gel", .name = "Lead-acid (gel)",
        .ocv_cell_uv = {1966667, 1986667, 2008333, 2030000, 2050000, 2066667,
                        2083333, 2100000, 2116667, 2133333, 2150000},
        .v_full_cell_uv = 2333333, .taper_div = 50, .rated_div = 20,
        .peukert_q8 = 287, .t_rest_s = 600,
        .trust_lo_permille = 1000, .trust_hi_permille = 1000,
    },
    [FG_CHEM_LIFEPO4] = {
        .key = "lifepo4", .name = "LiFePO4",
        .ocv_cell_uv = {2500000, 3000000, 3200000, 3220000, 3250000, 3260000,
                        3270000, 3300000, 3320000, 3350000, 3400000},
        .v_full_cell_uv = 3500000, .taper_div = 20, .rated_div = 5,
        .peukert_q8 = 269, .t_rest_s = 1800,
        .trust_lo_permille = 150, .trust_hi_permille = 950,
    },
    [FG_CHEM_LIION] = {
        .key = "liion", .name = "Li-ion (NMC/NCA)",
        .ocv_cell_uv = {3000000, 3450000, 3600000, 3670000, 3720000, 3770000,
                        3830000, 3900000, 3980000, 4080000, 4170000},
        .v_full_cell_uv = 4180000, .taper_div = 20, .rated_div = 5,
        .peukert_q8 = 266, .t_rest_s = 1200,
        .trust_lo_permille = 1000, .trust_hi_permille = 1000,
    },
    [FG_CHEM_LIPO] = {
        .key = "lipo", .name = "LiPo",
        .ocv_cell_uv = {3270000, 3690000, 3730000, 3770000, 3800000, 3840000,
                        3870000, 3950000, 4020000, 4110000, 4170000},
        .v_full_cell_uv = 4180000, .taper_div = 20, .rated_div = 5,
        .peukert_q8 = 266, .t_rest_s = 1200,
        .trust_lo_permille = 1000, .trust_hi_permille = 1000,
    },
    [FG_CHEM_LTO] = {
        .key = "lto", .name = "LTO (lithium titanate)",
        .ocv_cell_uv = {2000000, 2200000, 2270000, 2310000, 2350000, 2380000,
                        2410000, 2450000, 2500000, 2560000, 2650000},
        .v_full_cell_uv = 2700000, .taper_div = 20, .rated_div = 2,
        .peukert_q8 = 261, .t_rest_s = 900,
        .trust_lo_permille = 1000, .trust_hi_permille = 1000,
    },
    [FG_CHEM_NIMH] = {
        .key = "nimh", .name = "NiMH",
        .ocv_cell_uv = {1000000, 1150000, 1200000, 1220000, 1240000, 1250000,
                        1260000, 1280000, 1300000, 1330000, 1400000},
        /* NiMH chargers end in a trickle rather than a CV taper; C/10 catches both. */
        .v_full_cell_uv = 1450000, .taper_div = 10, .rated_div = 5,
        .peukert_q8 = 282, .t_rest_s = 1800,
        .trust_lo_permille = 150, .trust_hi_permille = 900,
    },
};

const fg_chem_profile_t *fg_chem_profile(fg_chem_t c)
{
    return (unsigned)c < FG_CHEM_COUNT ? &CHEM[c] : NULL;
}

static const fg_chem_profile_t *chem(void)
{
    const fg_chem_profile_t *p = fg_chem_profile((fg_chem_t)s_fg.cfg.chemistry);
    return p ? p : &CHEM[FG_CHEM_FLOODED];
}

bool fg_chem_parse(const char *key, fg_chem_t *out)
{
    static const struct { const char *alias; fg_chem_t c; } ALIASES[] = {
        {"lead", FG_CHEM_FLOODED}, {"wet", FG_CHEM_FLOODED},
        {"lfp", FG_CHEM_LIFEPO4},  {"lifepo", FG_CHEM_LIFEPO4},
        {"li-ion", FG_CHEM_LIION}, {"nmc", FG_CHEM_LIION}, {"nca", FG_CHEM_LIION},
        {"li-po", FG_CHEM_LIPO},
        {"ni-mh", FG_CHEM_NIMH},
    };
    for (int i = 0; i < FG_CHEM_COUNT; i++) {
        if (strcasecmp(key, CHEM[i].key) == 0) {
            *out = (fg_chem_t)i;
            return true;
        }
    }
    for (size_t i = 0; i < sizeof(ALIASES) / sizeof(ALIASES[0]); i++) {
        if (strcasecmp(key, ALIASES[i].alias) == 0) {
            *out = ALIASES[i].c;
            return true;
        }
    }
    return false;
}

void fg_config_apply_chem(fg_config_t *cfg, fg_chem_t c, uint8_t cells)
{
    const fg_chem_profile_t *p = fg_chem_profile(c);
    if (!p || cells == 0) {
        return;
    }
    cfg->chemistry   = (uint8_t)c;
    cfg->cells       = cells;
    cfg->v_0pct_uv   = p->ocv_cell_uv[0] * cells;
    cfg->v_100pct_uv = p->ocv_cell_uv[FG_OCV_POINTS - 1] * cells;
    cfg->v_full_uv   = p->v_full_cell_uv * cells;
    cfg->i_taper_ua  = cfg->design_capacity_uah / p->taper_div; /* uAh / h = uA */
    cfg->i_rated_ua  = cfg->design_capacity_uah / p->rated_div;
    cfg->peukert_q8  = p->peukert_q8;
    cfg->t_rest_s    = p->t_rest_s;
}

/*
 * Voltage to SoC through the SHAPE of the chemistry's resting-OCV curve (§8.6,
 * v_curve_mode 1), scaled between the configured v_0pct and v_100pct.
 *
 * The position of the voltage within the configured window is mapped onto the same
 * position within the table's own 0..100 % span, then interpolated between the two
 * neighbouring 10 % points. So v0/v100 move the endpoints and the curve stretches with
 * them: an owner who sets a conservative 0 % keeps the chemistry's shape rather than
 * getting a table that disagrees with them.
 */
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
    const uint32_t *t     = chem()->ocv_cell_uv;
    const uint64_t  span  = s_fg.cfg.v_100pct_uv - s_fg.cfg.v_0pct_uv;
    const uint64_t  tspan = t[FG_OCV_POINTS - 1] - t[0];
    const uint32_t  x     = t[0] + (uint32_t)(((uint64_t)(ocv_uv - s_fg.cfg.v_0pct_uv) * tspan) / span);

    const uint32_t step = 1000 / (FG_OCV_POINTS - 1);
    for (int i = 0; i < FG_OCV_POINTS - 1; i++) {
        if (x <= t[i + 1]) {
            return step * (uint32_t)i +
                   (uint32_t)(((uint64_t)(x - t[i]) * step) / (t[i + 1] - t[i]));
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
 * C/110: 0.4 A on 44 Ah, averaged over ~10 s (CLASS_TAU_S). Below it the pack is
 * SETTLING whatever the direction -- the small, noisy currents of an idle installation
 * are not a charge or a discharge worth leaving the settle countdown for. The I·R
 * compensation still applies at that current, so OCV stays usable.
 */
/*
 * The taper current full detection actually uses: the configured one, scaled by the
 * LEARNED capacity against the nameplate it was set for. A charger's current tapers in
 * proportion to what the battery really holds, so a 45 Ah battery worn to 4 Ah reaches
 * full at about a tenth of the nameplate's taper -- judged against the nameplate
 * figure, it would latch FULL while still charging hard. The configured value keeps
 * its meaning as a C-rate (design / 30 by default).
 */
static uint32_t taper_current_ua(void)
{
    const uint32_t design = s_fg.cfg.design_capacity_uah;
    if (design == 0) {
        return s_fg.cfg.i_taper_ua;
    }
    return (uint32_t)(((uint64_t)s_fg.cfg.i_taper_ua * s_fg.full_capacity_uah) / design);
}

static uint32_t rest_current_ua(void)
{
    const uint32_t c110 = s_fg.cfg.design_capacity_uah / 110;
    return c110 > s_fg.cfg.i_deadband_ua ? c110 : s_fg.cfg.i_deadband_ua;
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
    if (err == ESP_OK) err = nvs_set_u8  (h, "hr",  s_fg.have_ref ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u32 (h, "rsc", s_fg.ref_soc_permille);
    if (err == ESP_OK) err = nvs_set_i64 (h, "qsr", s_fg.q_since_ref_uas);
    if (err == ESP_OK) err = nvs_set_i64 (h, "qir", s_fg.q_in_since_ref_uas);
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
    s_fg.t_full_s = s_fg.t_empty_s = s_fg.settle_s = -1;
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
    if (nvs_get_u32(h, "lrn", &u32) == ESP_OK) s_fg.learn_count = u32;
    if (nvs_get_u8 (h, "hr",  &u8)  == ESP_OK) s_fg.have_ref = (u8 != 0);
    if (nvs_get_u32(h, "rsc", &u32) == ESP_OK) s_fg.ref_soc_permille = u32;
    if (nvs_get_i64(h, "qsr", &i64) == ESP_OK) s_fg.q_since_ref_uas = i64;
    if (nvs_get_i64(h, "qir", &i64) == ESP_OK) s_fg.q_in_since_ref_uas = i64;
    nvs_close(h);
    /* The state itself is not restored: it is re-derived from the first samples.
     * UNKNOWN with voltage_only false means "count restored, do not re-seed". */

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

/* --- capacity learning (§5.4) ------------------------------------------------ */

/*
 * Blends a raw capacity measurement into the learned capacity.
 *
 * Plausibility is deliberately wide: an old battery really does drop to a tenth of
 * its nameplate (45 Ah rated, 4 Ah left), and a gauge that refuses to believe that
 * keeps showing SoC against capacity the pack no longer has. Below 1/20 or above 1.5x
 * nameplate is a measurement fault (a mis-set shunt, a missed anchor), not a battery.
 *
 * The FIRST measurement replaces the nameplate outright: the nameplate is a claim
 * about a new battery, not a measurement of this one, and blending a real 4 Ah
 * against it at 25 % a cycle would take a dozen cycles to get close. After that,
 * each measurement moves capacity in proportion to how deep its span was -- a 20 %
 * span is weaker evidence than an 80 % one.
 */
static bool learn_capacity(uint32_t measured_uah, uint32_t depth_permille)
{
    s_fg.last_learn_uah = measured_uah;

    const uint32_t design = s_fg.cfg.design_capacity_uah;
    if (measured_uah < design / 20 || measured_uah > design + design / 2) {
        ESP_LOGW(TAG, "capacity measurement %lu uAh implausible against %lu nameplate "
                      "-- ignored",
                 (unsigned long)measured_uah, (unsigned long)design);
        return false;
    }

    if (s_fg.learn_count == 0) {
        s_fg.full_capacity_uah = measured_uah;
    } else {
        uint32_t w = (uint32_t)s_fg.cfg.learn_blend_q8 * depth_permille / 500;
        if (w < 1)   w = 1;
        if (w > 256) w = 256;
        const int64_t delta = (int64_t)measured_uah - (int64_t)s_fg.full_capacity_uah;
        s_fg.full_capacity_uah =
            (uint32_t)((int64_t)s_fg.full_capacity_uah + (delta * (int64_t)w) / 256);
    }
    s_fg.learn_count++;
    ESP_LOGI(TAG, "learned capacity: measured %lu uAh over %lu.%lu%% -> %lu uAh",
             (unsigned long)measured_uah,
             (unsigned long)(depth_permille / 10), (unsigned long)(depth_permille % 10),
             (unsigned long)s_fg.full_capacity_uah);
    return true;
}

/*
 * A REFERENCE POINT: SoC is known right now from something other than the count --
 * FULL (100 %), EMPTY (0 %), or a settled resting voltage. If a span is open from an
 * earlier reference, the charge drawn between the two against the SoC they differ by
 * IS the capacity:
 *
 *     capacity = charge drawn / (SoC_before - SoC_now)
 *
 * so any discharge deep enough to clear learn_min_depth measures the battery, whether
 * or not it ever reaches 0 %. Learned only on DISCHARGE spans: charge going in exceeds
 * the capacity it restores (lead-acid coulombic efficiency is well under 100 %), so a
 * span with real charging in it would inflate the figure.
 *
 * Returns true if capacity was learned, so the caller snaps the count to the
 * reference rather than blending toward it with a capacity that just changed.
 */
static bool ref_point(uint32_t soc_now)
{
    bool learned = false;
    if (s_fg.have_ref && soc_now < s_fg.ref_soc_permille) {
        const uint32_t depth = s_fg.ref_soc_permille - soc_now;
        const int64_t  drawn = -s_fg.q_since_ref_uas;
        const int64_t  in_max = capacity_uas() / 50; /* 2 %: float/ripple, not a charge */
        if (depth >= s_fg.cfg.learn_min_depth_permille && drawn > 0 &&
            s_fg.q_in_since_ref_uas <= in_max) {
            const uint32_t measured_uah = (uint32_t)((drawn * 1000 / depth) / 3600);
            learned = learn_capacity(measured_uah, depth);
        }
    }
    s_fg.have_ref           = true;
    s_fg.ref_soc_permille   = soc_now;
    s_fg.q_since_ref_uas    = 0;
    s_fg.q_in_since_ref_uas = 0;
    s_fg.dirty              = true;
    return learned;
}

/* --- the gauge ----------------------------------------------------------------- */

/* Entry actions: timers belong to the state that runs them, so each starts fresh. */
static void enter(fg_state_t next, int64_t now)
{
    if (next == s_fg.state) {
        return;
    }
    s_fg.full_since_us  = 0;
    s_fg.empty_since_us = 0;
    if (next == FG_SETTLING) {
        s_fg.idle_since_us = now;
    }
    s_fg.state = next;
    s_fg.dirty = true;
}

/* A settled rest: re-sync the count to the resting voltage (§5.4). */
static void rest_resync(void)
{
    const uint32_t ocv_soc = soc_from_ocv(s_fg.ocv_uv);

    if (s_fg.voltage_only) {
        /* No count worth keeping: take the voltage outright, flat curve or not. */
        set_charge_from_soc(ocv_soc);
        s_fg.voltage_only   = false;
        s_fg.s_since_anchor = 0;
        ref_point(ocv_soc);
        return;
    }

    /* On the flat part of a flat chemistry (LiFePO4, NiMH) the resting voltage says
     * too little to correct a count, or to measure capacity against. */
    if (ocv_soc > chem()->trust_lo_permille && ocv_soc < chem()->trust_hi_permille) {
        return;
    }

    const bool learned = ref_point(ocv_soc);
    if (learned || s_fg.s_since_anchor == UINT32_MAX) {
        /* Snap: either capacity just changed under the count, or the count was
         * carried over from before boot and never checked against this pack. */
        set_charge_from_soc(ocv_soc);
    } else {
        /* Blend, do not snap: a rested OCV is good but not exact. */
        const int64_t target = (capacity_uas() * (int64_t)ocv_soc) / 1000;
        const int64_t delta  = target - s_fg.charge_uas;
        s_fg.charge_uas += (delta * (int64_t)s_fg.cfg.ocv_blend_q8) / 256;
    }
    s_fg.s_since_anchor = 0;
    s_fg.dirty          = true;
}

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

    if (dt >= DEAD_GAP_US) {
        /* Charge flowed that was never measured: the open learning span no longer
         * knows how much was drawn, so it closes rather than learn from a hole. */
        s_fg.have_ref = false;
    } else if (dt > 0 && !in_deadband) {
        /* A gap between MAX_GAP_US and DEAD_GAP_US still integrates -- at the last
         * known current, which is the honest choice when the alternative is to
         * discard real charge -- but it is not treated as an anchor. */
        const int64_t num  = (int64_t)s->i_ua * dt + s_fg.rem_frac;
        const int64_t dq   = num / 1000000;
        s_fg.rem_frac      = num % 1000000;

        /* Lifetime counters take the RAW coulombs. These answer "how much charge
         * passed through the shunt", which is a measurement, not a model. */
        if (dq > 0) {
            s_fg.cum_in_uas         += dq;
            s_fg.q_in_since_ref_uas += dq;
        } else {
            s_fg.cum_out_uas -= dq;
        }

        /* The SoC count takes the Peukert-adjusted value, which answers the different
         * question of "how much capacity did that consume". */
        int64_t dq_eff = dq;
        if (dq < 0) {
            dq_eff = -peukert_scale(-dq, -(int64_t)s->i_ua);
        }

        s_fg.charge_uas      += dq_eff;
        s_fg.q_since_ref_uas += dq_eff;

        /* Clamp to the physical range. Hitting a clamp is information, not an error:
         * it means the count and the pack have diverged, and the next reference point
         * is what fixes it. */
        if (s_fg.charge_uas < 0) {
            s_fg.charge_uas = 0;
        } else if (s_fg.charge_uas > capacity_uas()) {
            s_fg.charge_uas = capacity_uas();
        }
        s_fg.dirty = true;
    }

    /* Sub-second remainder carried, not dropped: samples arrive every ~100 ms, and
     * truncating each dt to whole seconds added 0 every time. */
    if (dt > 0 && s_fg.s_since_anchor != UINT32_MAX) {
        s_fg.anchor_rem_us  += dt;
        s_fg.s_since_anchor += (uint32_t)(s_fg.anchor_rem_us / 1000000);
        s_fg.anchor_rem_us  %= 1000000;
    }

    /* --- bootstrap --------------------------------------------------------------- */
    if (s_fg.state == FG_UNKNOWN && s_fg.voltage_only) {
        if (!s->v_valid) {
            return;
        }
        /* First-ever reading and nothing stored: seed from the compensated voltage so
         * the display is useful immediately. Still voltage_only, so the first settled
         * rest replaces it rather than blending against a guess. */
        set_charge_from_soc(soc_from_ocv(s_fg.ocv_uv));
    }

    /* --- classify: which state the pack is in NOW ---------------------------------
     *
     * Direction uses the REST threshold, not the integration deadband: resting only
     * has to mean "too little current for the terminal voltage to be far from OCV",
     * and a pack with a monitor on it always draws a few milliamps. Absorption is
     * checked first and on the deadband: at v_full a pack is on a charger however
     * small the current, and a charger tapering below the rest threshold must still
     * reach FULL. FULL and EMPTY are sticky while the direction that produced them
     * continues; resting after either goes through SETTLING like any other rest. */
    /* Filtered currents, first-order: x += (i - x) * dt / (tau + dt). Seeded from the
     * first sample so a fresh boot does not start from zero. */
    if (!s_fg.filt_init) {
        s_fg.i_class_ua = s_fg.i_est_ua = s->i_ua;
        s_fg.filt_init  = true;
    } else if (dt > 0 && dt < DEAD_GAP_US) {
        const int64_t ct = (int64_t)CLASS_TAU_S * 1000000, et = (int64_t)EST_TAU_S * 1000000;
        s_fg.i_class_ua += ((s->i_ua - s_fg.i_class_ua) * dt) / (ct + dt);
        s_fg.i_est_ua   += ((s->i_ua - s_fg.i_est_ua) * dt) / (et + dt);
    }

    const int64_t ri = rest_current_ua();
    const int64_t fi = s_fg.i_class_ua;
    const bool was_chg = s_fg.state == FG_CHARGE || s_fg.state == FG_ABSORB ||
                         s_fg.state == FG_FULL;
    const bool was_dsg = s_fg.state == FG_DISCHARGE || s_fg.state == FG_EMPTY;
    /* Hysteresis: stay in a direction down to the rest current, enter one only above
     * twice it. */
    const bool chg_now = fi > (was_chg ? ri : 2 * ri);
    const bool dsg_now = fi < -(was_dsg ? ri : 2 * ri);
    const bool at_absorb = s->v_valid && s->v_pack_uv >= s_fg.cfg.v_full_uv &&
                           fi > (int64_t)s_fg.cfg.i_deadband_ua;
    fg_state_t next;
    if (at_absorb) {
        next = (s_fg.state == FG_FULL) ? FG_FULL : FG_ABSORB;
    } else if (chg_now) {
        next = (s_fg.state == FG_FULL) ? FG_FULL : FG_CHARGE;
    } else if (dsg_now) {
        next = (s_fg.state == FG_EMPTY) ? FG_EMPTY : FG_DISCHARGE;
    } else {
        next = (s_fg.state == FG_REST) ? FG_REST : FG_SETTLING;
    }
    enter(next, now);

    /* --- per-state work: each state looks for its own anchor ---------------------- */
    switch (s_fg.state) {
    case FG_ABSORB: {
        /*
         * FULL (§5.4 A): absorption voltage AND taper current, held. The taper is
         * judged on the MEAN current over the hold window, not on every sample: a CV
         * charger's current ripples, and a per-sample check restarted the window on
         * every excursion above the taper, so full never latched.
         */
        if (s_fg.full_since_us == 0) {
            s_fg.full_since_us = now;
            s_fg.full_sum_ua   = 0;
            s_fg.full_n        = 0;
        }
        s_fg.full_sum_ua += s->i_ua;
        s_fg.full_n++;
        if ((now - s_fg.full_since_us) >= (int64_t)s_fg.cfg.t_full_hold_s * 1000000) {
            const int64_t mean_ua = s_fg.full_sum_ua / (int64_t)s_fg.full_n;
            s_fg.full_since_us = 0; /* judge the next window afresh */
            if (mean_ua <= (int64_t)taper_current_ua()) {
                ref_point(1000);
                s_fg.charge_uas     = capacity_uas();
                s_fg.rem_frac       = 0;
                s_fg.s_since_anchor = 0;
                s_fg.voltage_only   = false;
                enter(FG_FULL, now);
            }
        }
        break;
    }

    case FG_DISCHARGE: {
        /*
         * EMPTY (§5.4 B): the compensated voltage at the 0 % endpoint under load,
         * held. Below EMPTY_FLOOR of v_0pct it is not a battery at its endpoint but
         * an absent one or a disconnected VBUS, and must not anchor anything.
         */
        const uint32_t floor_uv =
            (uint32_t)(((uint64_t)s_fg.cfg.v_0pct_uv * EMPTY_FLOOR_Q8) / 256);
        const bool empty_now = s->v_valid && s->v_pack_uv >= floor_uv &&
                               s_fg.ocv_uv <= s_fg.cfg.v_0pct_uv;
        if (!empty_now) {
            s_fg.empty_since_us = 0;
        } else if (s_fg.empty_since_us == 0) {
            s_fg.empty_since_us = now;
        } else if ((now - s_fg.empty_since_us) >= (int64_t)EMPTY_HOLD_S * 1000000) {
            ref_point(0);
            s_fg.charge_uas     = 0;
            s_fg.rem_frac       = 0;
            s_fg.s_since_anchor = 0;
            s_fg.voltage_only   = false;
            enter(FG_EMPTY, now);
        }
        break;
    }

    case FG_FULL:
    case FG_EMPTY:
        /* Held at an endpoint: the reference stays pinned to it. Hours on a float
         * charger after FULL would otherwise count as charge going in during the
         * span and block the discharge after it from ever being learned from. */
        s_fg.q_since_ref_uas    = 0;
        s_fg.q_in_since_ref_uas = 0;
        break;

    case FG_SETTLING:
        if (s->v_valid &&
            (now - s_fg.idle_since_us) >= (int64_t)s_fg.cfg.t_rest_s * 1000000) {
            rest_resync();
            s_fg.idle_since_us = now;
            enter(FG_REST, now);
        }
        break;

    case FG_REST:
        /* Keep re-syncing while the rest continues: the voltage is still relaxing,
         * and each period brings it closer to true OCV. */
        if (s->v_valid &&
            (now - s_fg.idle_since_us) >= (int64_t)s_fg.cfg.t_rest_s * 1000000) {
            rest_resync();
            s_fg.idle_since_us = now;
        }
        break;

    default:
        break;
    }

    /* --- time estimates ----------------------------------------------------------
     *
     * Chosen by the state, so they can never contradict it: to full while charging,
     * to empty while discharging, the settling countdown while settling, nothing at
     * rest. The rate is the ~60 s average; a discharge rate is Peukert-scaled, since
     * that is the rate at which the count actually falls. */
    s_fg.t_full_s = s_fg.t_empty_s = s_fg.settle_s = -1;
    const int64_t dband = s_fg.cfg.i_deadband_ua;
    switch (s_fg.state) {
    case FG_FULL:
        s_fg.t_full_s = 0;
        break;
    case FG_EMPTY:
        s_fg.t_empty_s = 0;
        break;
    case FG_CHARGE:
    case FG_ABSORB:
        if (s_fg.i_est_ua > dband) {
            int64_t left = capacity_uas() - s_fg.charge_uas;
            if (left < 0) left = 0;
            const int64_t t = left / s_fg.i_est_ua;
            s_fg.t_full_s = (int32_t)(t > EST_MAX_S ? EST_MAX_S : t);
        }
        break;
    case FG_DISCHARGE:
        if (s_fg.i_est_ua < -dband) {
            const uint32_t pf = s_fg.peukert_factor_q16 ? s_fg.peukert_factor_q16 : (1u << 16);
            const int64_t  rate = ((-s_fg.i_est_ua) * (int64_t)pf) >> 16;
            const int64_t  t = rate > 0 ? s_fg.charge_uas / rate : EST_MAX_S;
            s_fg.t_empty_s = (int32_t)(t > EST_MAX_S ? EST_MAX_S : t);
        }
        break;
    case FG_SETTLING: {
        const int64_t left = (int64_t)s_fg.cfg.t_rest_s - (now - s_fg.idle_since_us) / 1000000;
        s_fg.settle_s = (int32_t)(left > 0 ? left : 0);
        break;
    }
    default:
        break;
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
    out->taper_current_ua  = taper_current_ua();
    out->t_full_s          = s_fg.t_full_s;
    out->t_empty_s         = s_fg.t_empty_s;
    out->settle_s          = s_fg.settle_s;
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
    out->have_ref          = s_fg.have_ref;
    out->ref_soc_permille  = s_fg.ref_soc_permille;
    out->q_since_ref_uas   = s_fg.q_since_ref_uas;
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
    if (cfg->chemistry >= FG_CHEM_COUNT || cfg->cells == 0 || cfg->cells > 32) {
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
        /* A deliberately set capacity replaces what was learned; the next
         * measurement is again the first, and replaces it outright. */
        s_fg.learn_count = 0;
    }
    return store();
}

esp_err_t fg_set_chemistry(fg_chem_t c, uint8_t cells)
{
    fg_config_t cfg = s_fg.cfg;
    fg_config_apply_chem(&cfg, c, cells);
    const esp_err_t err = fg_set_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    /* A different battery, or the same one described differently: either way the
     * count was accumulated against a curve that no longer applies. Re-seed from the
     * next voltage sample, as on a first boot. Lifetime counters are history and stay. */
    s_fg.state            = FG_UNKNOWN;
    s_fg.voltage_only     = true;
    s_fg.have_ref         = false;
    s_fg.s_since_anchor   = UINT32_MAX;
    s_fg.idle_since_us    = 0;
    s_fg.full_since_us    = 0;
    s_fg.empty_since_us   = 0;
    s_fg.dirty            = true;
    return ESP_OK;
}

esp_err_t fg_set_soc_permille(uint32_t permille)
{
    if (permille > 1000) {
        return ESP_ERR_INVALID_ARG;
    }
    set_charge_from_soc(permille);
    s_fg.voltage_only   = false;
    s_fg.s_since_anchor = 0;
    /* A typed-in SoC is not a measurement: no capacity is learned across it. */
    s_fg.have_ref       = false;
    return store();
}

esp_err_t fg_set_full(void)
{
    ref_point(1000);
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
    s_fg.learn_count       = 0;
    /* A reset abandons the learning span too: it is no longer bracketed by a
     * trustworthy reference point. */
    s_fg.have_ref           = false;
    s_fg.q_since_ref_uas    = 0;
    s_fg.q_in_since_ref_uas = 0;
    return store();
}

char fg_state_code(fg_state_t s)
{
    switch (s) {
    case FG_CHARGE:    return 'C';
    case FG_ABSORB:    return 'A';
    case FG_FULL:      return 'F';
    case FG_DISCHARGE: return 'D';
    case FG_EMPTY:     return 'E';
    case FG_SETTLING:  return 'S';
    case FG_REST:      return 'R';
    case FG_UNKNOWN:
    default:           return 'U';
    }
}

const char *fg_state_str(fg_state_t s)
{
    switch (s) {
    case FG_CHARGE:    return "CHARGE";
    case FG_ABSORB:    return "ABSORB";
    case FG_FULL:      return "FULL";
    case FG_DISCHARGE: return "DISCHARGE";
    case FG_EMPTY:     return "EMPTY";
    case FG_SETTLING:  return "SETTLING";
    case FG_REST:      return "REST";
    case FG_UNKNOWN:
    default:           return "UNKNOWN";
    }
}
