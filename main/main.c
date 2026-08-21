/*
 * main.c — bat-monitor M1: bring-up.
 *
 * Scope (DESIGN.md §11): IDF project, I2C, INA219 drivers, console dump.
 * Exit criterion: voltage and current match a bench meter within 1% over +/-2 A.
 *
 * The board carries two INA219s, one per pole (§2.10). Everything above the sensors
 * component deals in power_sample_t and does not know or care which pole carries the
 * shunt.
 *
 * What is deliberately NOT here yet: charge integration, NVS, BLE, the display,
 * the temperature sensor, power management. Each arrives at its own milestone.
 */

#include <stdio.h>
#include <string.h>

#include "app_ctx.h"
#include "ble_console.h"
#include "cal_store.h"
#include "display_debug.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fixed_fmt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fuelgauge.h"
#include "ina219.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "sensors.h"

static const char *TAG = "main";

/*
 * Kconfig `bool` symbols are #defined to 1 when set and left UNDEFINED when not,
 * so referring to them in C expressions breaks the build in exactly the
 * configuration nobody tests. Normalise them to real constants here, once.
 */
#ifdef CONFIG_BATMON_I2CA_INTERNAL_PULLUPS
#define BATMON_INTERNAL_PULLUPS true
#else
#define BATMON_INTERNAL_PULLUPS false
#endif

#ifdef CONFIG_BATMON_STREAM_ON_BOOT
#define BATMON_STREAM_ON_BOOT true
#else
#define BATMON_STREAM_ON_BOOT false
#endif

#ifdef CONFIG_BATMON_ANTENNA_EXTERNAL
#define BATMON_ANTENNA_EXTERNAL 1
#else
#define BATMON_ANTENNA_EXTERNAL 0
#endif

#ifdef CONFIG_BATMON_INVERT_SIGN
#define BATMON_INVERT_SIGN true
#else
#define BATMON_INVERT_SIGN false
#endif

/*
 * Install mode (DESIGN.md §2.10.3). Mode P is high-side and needs no range ceiling;
 * both low-side cases cap the PGA at /4 because the inputs cannot survive the full
 * 320 mV excursion below ground (§2.9.2).
 */
#if defined(CONFIG_BATMON_INSTALL_MODE_P)
#define BATMON_INSTALL_MODE SENSORS_MODE_P
#define BATMON_PGA_MAX      INA219_PGA_8
#elif defined(CONFIG_BATMON_INSTALL_MODE_N)
#define BATMON_INSTALL_MODE SENSORS_MODE_N
#define BATMON_PGA_MAX      INA219_PGA_4
#else /* CONFIG_BATMON_INSTALL_MODE_AUTO */
#define BATMON_INSTALL_MODE SENSORS_MODE_AUTO
#define BATMON_PGA_MAX      INA219_PGA_4
#endif

static app_ctx_t s_ctx;

app_ctx_t *app_ctx(void)
{
    return &s_ctx;
}

bool sensor_lock_take(app_ctx_t *ctx, uint32_t timeout_ms)
{
    if (!ctx->sensor_lock) {
        return true; /* no sampler running: nothing to contend with */
    }
    return xSemaphoreTake(ctx->sensor_lock, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void sensor_lock_give(app_ctx_t *ctx)
{
    if (ctx->sensor_lock) {
        xSemaphoreGive(ctx->sensor_lock);
    }
}

/* --- statistics -------------------------------------------------------------- */

void stats_reset(sample_stats_t *s)
{
    memset(s, 0, sizeof(*s));
    s->min_ua = INT32_MAX;
    s->max_ua = INT32_MIN;
    s->min_uv = UINT32_MAX;
    s->max_uv = 0;
}

void stats_add(sample_stats_t *s, const power_sample_t *smp)
{
    if (smp->i_valid) {
        s->n++;
        s->sum_ua    += smp->i_ua;
        s->sum_sq_ua += (int64_t)smp->i_ua * (int64_t)smp->i_ua;
        if (smp->i_ua < s->min_ua) s->min_ua = smp->i_ua;
        if (smp->i_ua > s->max_ua) s->max_ua = smp->i_ua;
    }
    /* Voltage is only sampled every Nth pass (DESIGN.md §4.5), so accumulate it only
     * on the passes where it is genuinely new. Otherwise the same reading would be
     * counted eight times and the min/max would look far more stable than it is. */
    if (smp->v_valid && smp->v_is_fresh) {
        s->n_v++;
        s->sum_uv += smp->v_pack_uv;
        if (smp->v_pack_uv < s->min_uv) s->min_uv = smp->v_pack_uv;
        if (smp->v_pack_uv > s->max_uv) s->max_uv = smp->v_pack_uv;
    }
}

int32_t stats_mean_ua(const sample_stats_t *s)
{
    return s->n ? (int32_t)(s->sum_ua / (int64_t)s->n) : 0;
}

/* Divides by the voltage count, not the current count: voltage is sampled on its own
 * divisor (DESIGN.md 4.5), so the two differ by that factor. */
int32_t stats_mean_uv(const sample_stats_t *s)
{
    return s->n_v ? (int32_t)(s->sum_uv / (int64_t)s->n_v) : 0;
}

/*
 * Integer square root, Newton's method. Avoiding sqrt() keeps the float ban whole
 * (DESIGN.md §4.3) and costs nothing at these call rates.
 *
 * The loop condition is `y < g`, not `y != g`: the naive form oscillates forever
 * between two adjacent values for inputs whose root is not exact, which on a
 * console command would simply hang the calling task.
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

int32_t stats_stddev_ua(const sample_stats_t *s)
{
    if (s->n < 2) {
        return 0;
    }
    const int64_t mean = s->sum_ua / (int64_t)s->n;
    /* var = E[x^2] - E[x]^2, population variance. Adequate here; the sample-vs-
     * population distinction is noise next to the measurement it describes. */
    int64_t var = (s->sum_sq_ua / (int64_t)s->n) - (mean * mean);
    if (var < 0) {
        var = 0; /* can only be rounding */
    }
    return (int32_t)isqrt64((uint64_t)var);
}

/* --- zero-current calibration (DESIGN.md §5.5) ------------------------------- */

esp_err_t run_zero_calibration(app_ctx_t *ctx, uint32_t n_samples,
                               int32_t *out_offset_ua, int32_t *out_stddev_ua)
{
    /* Calibration reaches past the role abstraction on purpose: it is a property of
     * one physical device, not of the assembled measurement. */
    ina219_handle_t dev = sensors_current_dev(ctx->sensors);
    if (!dev) {
        ESP_LOGE(TAG, "no current sensor -- roles unresolved");
        return ESP_ERR_INVALID_STATE;
    }

    /* Held for the whole run: see the comment on sensor_lock. */
    if (!sensor_lock_take(ctx, 2000)) {
        ESP_LOGE(TAG, "sensor busy");
        return ESP_ERR_TIMEOUT;
    }

    /*
     * Force the finest range: the offset we are measuring is a shunt-voltage
     * offset, and PGA/1 resolves it 8x better than PGA/8.
     */
    const ina219_pga_t saved_pga  = ina219_get_pga(dev);
    const bool         saved_auto = ina219_get_autorange(dev);
    const int32_t      saved_off  = ina219_get_offset_ua(dev);
    const uint32_t     saved_gain = ina219_get_gain_ppm(dev);

    ina219_set_autorange(dev, false);
    ina219_set_pga(dev, INA219_PGA_1);
    ina219_set_offset_ua(dev, 0); /* measure raw, not residual */

    /*
     * Unity gain while measuring. convert() subtracts the offset BEFORE applying
     * gain, so an offset measured through a non-unity gain is stored in the wrong
     * domain and comes out scaled by 1/gain. It cancels only when gain happens to be
     * exactly 1.0 -- which it is on a fresh board, and is not once the gain has been
     * trimmed, so re-running `zero` after a gain trim would otherwise be subtly
     * wrong. Restored below with the range settings.
     */
    ina219_set_gain_ppm(dev, 1000000);

    sample_stats_t st;
    stats_reset(&st);

    esp_err_t err = ESP_OK;
    for (uint32_t i = 0; i < n_samples; i++) {
        ina219_sample_t s;
        err = ina219_read_blocking(dev, &s);
        if (err != ESP_OK) {
            break;
        }
        const power_sample_t ps = {.i_ua = s.i_ua, .i_valid = true};
        stats_add(&st, &ps);
        if ((i % 32) == 0) {
            printf(".");
            fflush(stdout);
        }
    }
    printf("\n");

    const int32_t mean   = stats_mean_ua(&st);
    const int32_t stddev = stats_stddev_ua(&st);

    if (out_offset_ua) *out_offset_ua = mean;
    if (out_stddev_ua) *out_stddev_ua = stddev;

    /* Restore range and gain regardless of outcome. */
    ina219_set_autorange(dev, saved_auto);
    ina219_set_pga(dev, saved_pga);
    ina219_set_gain_ppm(dev, saved_gain);

    if (err != ESP_OK) {
        ina219_set_offset_ua(dev, saved_off);
        sensor_lock_give(ctx);
        return err;
    }

    /*
     * Expected noise: the shunt LSB is 10 uV, so one count is 10 uV / R. Anything
     * much beyond a few counts means current was flowing -- the load was not really
     * disconnected -- and calibrating on it would bake a real current into the offset
     * permanently. Refuse rather than guess.
     */
    const int32_t lsb_ua =
        (int32_t)((10LL * 1000000LL) / (int64_t)ina219_get_shunt_uohm(dev));
    const int32_t noise_limit = 5 * lsb_ua;

    if (stddev > noise_limit) {
        ESP_LOGW(TAG,
                 "calibration rejected: stddev %ld uA exceeds %ld uA -- is the load "
                 "really disconnected?",
                 (long)stddev, (long)noise_limit);
        ina219_set_offset_ua(dev, saved_off);
        sensor_lock_give(ctx);
        return ESP_ERR_INVALID_STATE;
    }

    ina219_set_offset_ua(dev, mean);
    sensor_lock_give(ctx);
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
 *  - The reject test is on magnitude rather than spread. At 0 V a real reading is a
 *    couple of counts; anything approaching a volt means VBUS is not actually at
 *    ground, and calibrating on it would bake that voltage into every future reading.
 *    Observed in practice: with the sensor grounds left unconnected, VBUS floated to
 *    3.448 V -- steady, plausible, and completely wrong. Rejecting it is what turned
 *    a silent 3.4 V calibration error into a five-minute wiring fix.
 */
esp_err_t run_zero_voltage_calibration(app_ctx_t *ctx, uint32_t n_samples,
                                       int32_t *out_offset_uv, uint32_t *out_spread_uv)
{
    ina219_handle_t dev = sensors_voltage_dev(ctx->sensors);
    if (!dev) {
        ESP_LOGE(TAG, "no voltage sensor -- roles unresolved");
        return ESP_ERR_INVALID_STATE;
    }

    if (!sensor_lock_take(ctx, 2000)) {
        ESP_LOGE(TAG, "sensor busy");
        return ESP_ERR_TIMEOUT;
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
        err = sensors_read_blocking(ctx->sensors, &s);
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
        if ((i % 32) == 0) {
            printf(".");
            fflush(stdout);
        }
    }
    printf("\n");

    if (err != ESP_OK || got == 0) {
        ina219_set_vbus_offset_uv(dev, saved_off);
        ina219_set_vbus_gain_ppm(dev, saved_gain);
        sensor_lock_give(ctx);
        return err != ESP_OK ? err : ESP_ERR_INVALID_STATE;
    }

    const int32_t mean = (int32_t)(sum / (int64_t)got);
    if (out_offset_uv) *out_offset_uv = mean;
    if (out_spread_uv) *out_spread_uv = (lo <= hi) ? (hi - lo) : 0;

    ina219_set_vbus_gain_ppm(dev, saved_gain);

    /* 0.5 V is ~125 bus counts: far more than any credible offset, and far less than
     * any pack someone might have left connected. */
    if (mean > 500000 || mean < -500000) {
        ESP_LOGW(TAG, "zero-voltage rejected: %ld uV is a real voltage, not an offset",
                 (long)mean);
        ina219_set_vbus_offset_uv(dev, saved_off);
        sensor_lock_give(ctx);
        return ESP_ERR_INVALID_STATE;
    }

    ina219_set_vbus_offset_uv(dev, mean);
    sensor_lock_give(ctx);
    return ESP_OK;
}

/* --- sampler ----------------------------------------------------------------- */

static void print_sample_csv(app_ctx_t *ctx, const power_sample_t *s)
{
    char bv[24], bi[24], bp[24], bsh[24], bsoc[24], bq[24];

    if (!ctx->stream_csv_header_done) {
        ctx->stream_csv_header_done = true;
        printf("ms,volts,amps,watts,shunt_mv,pga,sat,soc_pct,charge_ah,state\n");
    }

    fg_status_t fg;
    fg_get(&fg);

    /* Unquoted, no spaces, fixed column order: parseable by anything, including a
     * five-line awk script. Milliseconds since boot rather than a wall clock, because
     * this device has no idea what time it is. */
    printf("%lu,%s,%s,%s,%s,%s,%d,%s,%s,%s\n",
           (unsigned long)(s->t_us / 1000),
           FMT_V(bv, s->v_pack_uv),
           FMT_A(bi, s->i_ua),
           FMT_W(bp, s->p_uw),
           FMT_MV(bsh, s->v_shunt_uv),
           ina219_pga_str(s->pga),
           s->saturated ? 1 : 0,
           fixed_fmt(bsoc, sizeof(bsoc), fg.soc_permille, 10, 1),
           fixed_fmt(bq, sizeof(bq), fg.charge_uas / 3600, 1000000, 3),
           fg_state_str(fg.state));
}

static void print_sample_line(const power_sample_t *s, const sample_stats_t *w)
{
    char bv[24], bi[24], bp[24], bsh[24], bmean[24], bsd[24];

    printf("V=%9s V  I=%11s A  P=%10s W  | shunt %8s mV  pga %-14s%s | "
           "win n=%-5lu mean %9s A  sd %9s A\n",
           FMT_V(bv, s->v_pack_uv),
           FMT_A(bi, s->i_ua),
           FMT_W(bp, s->p_uw),
           FMT_MV(bsh, s->v_shunt_uv),
           ina219_pga_str(s->pga),
           s->saturated ? " SAT" : "",
           (unsigned long)w->n,
           FMT_A(bmean, stats_mean_ua(w)),
           FMT_A(bsd, stats_stddev_ua(w)));
}

/*
 * The sampler ticks faster than the sensor converts and polls CNVR, per
 * DESIGN.md §4.1. In M1 it also owns the console stream; from M3 it will push
 * onto a queue and own nothing else.
 */
static void sampler_task(void *arg)
{
    app_ctx_t *ctx = arg;

    int64_t next_print_us = esp_timer_get_time();
    bool    warned_unresolved = false;

    for (;;) {
        power_sample_t s;

        /* A short wait, not portMAX_DELAY: if a calibration routine holds the lock
         * for thirty seconds the sampler should idle, not queue up thirty seconds of
         * stale reads to replay afterwards. */
        if (!sensor_lock_take(ctx, 50)) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        const esp_err_t err = sensors_read(ctx->sensors, &s);
        sensor_lock_give(ctx);

        switch (err) {
        case ESP_OK:
            ctx->last       = s;
            ctx->last_valid = true;
            ctx->n_samples++;
            stats_add(&ctx->window, &s);
            /* The gauge sees every accepted sample and nothing else -- a rejected
             * read must never reach the integrator (DESIGN.md §5.2). */
            fg_update(&s);
            break;
        case ESP_ERR_NOT_FINISHED:
            ctx->err_not_finished++;
            break;
        case ESP_ERR_INVALID_STATE:
            /* Either a range discard or unresolved roles. The latter is a standing
             * condition, so warn once rather than every 100 ms. */
            if (sensors_get_role_state(ctx->sensors) != SENSORS_ROLE_RESOLVED) {
                ctx->err_unresolved++;
                if (!warned_unresolved) {
                    warned_unresolved = true;
                    ESP_LOGW(TAG, "roles unresolved -- NOT integrating. Apply a load "
                                  "and run 'detect', or set the mode in menuconfig.");
                }
                vTaskDelay(pdMS_TO_TICKS(500));
            } else {
                ctx->err_range_discard++;
            }
            break;
        default:
            ctx->err_bus++;
            ESP_LOGE(TAG, "sensor read failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(200));
            break;
        }

        const int64_t now = esp_timer_get_time();
        if (ctx->stream_enabled && ctx->last_valid && now >= next_print_us) {
            next_print_us = now + (int64_t)ctx->stream_period_ms * 1000;
            if (ctx->stream_csv) {
                print_sample_csv(ctx, &ctx->last);
            } else {
                print_sample_line(&ctx->last, &ctx->window);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* --- board / bus init --------------------------------------------------------- */

#if CONFIG_BATMON_INIT_ANTENNA_PINS
/*
 * XIAO ESP32-C6: GPIO3 powers the RF switch (active low) and GPIO14 selects the
 * antenna. No radio runs at M1, but leaving these floating is a documented source
 * of poor RF later, and it costs two lines to be correct from the start.
 * Verify the pin numbers against your board revision (DESIGN.md §2.1).
 */
static void init_antenna_pins(void)
{
    const gpio_config_t io = {
        .pin_bit_mask = (1ULL << 3) | (1ULL << 14),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    ESP_ERROR_CHECK(gpio_set_level(3, 0)); /* enable the RF switch */
    ESP_ERROR_CHECK(gpio_set_level(14, BATMON_ANTENNA_EXTERNAL));
    ESP_LOGI(TAG, "antenna: %s",
             BATMON_ANTENNA_EXTERNAL ? "external (u.FL)" : "onboard");
}
#endif

static esp_err_t init_bus_a(app_ctx_t *ctx)
{
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port                     = I2C_NUM_0,
        .sda_io_num                   = CONFIG_BATMON_I2CA_SDA_GPIO,
        .scl_io_num                   = CONFIG_BATMON_I2CA_SCL_GPIO,
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt            = 7,
        .flags.enable_internal_pullup = BATMON_INTERNAL_PULLUPS,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &ctx->bus_a), TAG, "i2c bus");

    ESP_LOGI(TAG, "bus A: SDA=GPIO%d SCL=GPIO%d @ %d Hz%s",
             CONFIG_BATMON_I2CA_SDA_GPIO, CONFIG_BATMON_I2CA_SCL_GPIO,
             CONFIG_BATMON_I2CA_FREQ_HZ,
             BATMON_INTERNAL_PULLUPS ? " (internal pull-ups)" : "");

    /* No warning about GPIO6/7 any more: the board shares one bus on GPIO22/23 and
     * DESIGN.md 2.5 / 9.3 record that as the decision, not as a deviation. LP_I2C
     * cannot reach these pads, so there is no deep-sleep tier to lose -- the floor
     * is T2 light sleep. Warning about a settled trade-off on every boot is noise. */
    return ESP_OK;
}

/* --- entry -------------------------------------------------------------------- */

void app_main(void)
{
    app_ctx_t *ctx = app_ctx();

    ESP_LOGI(TAG, "bat-monitor M1 (bring-up) -- see DESIGN.md");

    /*
     * NVS, before anything that might want it. The radio stores its PHY calibration
     * blob here, and without it every boot pays for a full recalibration and logs an
     * error that looks far more serious than it is. M3 puts the gauge state here too
     * (DESIGN.md 6.1), so this is not just a BLE prerequisite.
     *
     * A truncated or version-mismatched partition is erased and recreated: at M1
     * there is nothing in it worth preserving. That changes once fg_state_t lives
     * here -- 6.5's migration path replaces this erase.
     */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS unusable (%s) -- erasing", esp_err_to_name(nvs_err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    if (nvs_err != ESP_OK) {
        /* Not fatal: the console and the sampler do not need it. */
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(nvs_err));
    }

#if CONFIG_BATMON_INIT_ANTENNA_PINS
    init_antenna_pins();
#endif

    ESP_ERROR_CHECK(init_bus_a(ctx));

    sensors_config_t scfg   = SENSORS_CONFIG_DEFAULT();
    scfg.addr_pos_pole      = CONFIG_BATMON_ADDR_POS_POLE;
    scfg.addr_neg_pole      = CONFIG_BATMON_ADDR_NEG_POLE;
    scfg.mode               = BATMON_INSTALL_MODE;
    scfg.scl_speed_hz       = CONFIG_BATMON_I2CA_FREQ_HZ;
    scfg.r_shunt_uohm       = CONFIG_BATMON_SHUNT_UOHM;
    scfg.pga_max            = BATMON_PGA_MAX;
    scfg.invert_sign        = BATMON_INVERT_SIGN;
    scfg.voltage_divisor    = CONFIG_BATMON_VOLTAGE_DIVISOR;
    scfg.diagnostic_divisor = CONFIG_BATMON_DIAG_DIVISOR;

    const esp_err_t err = sensors_init(ctx->bus_a, &scfg, &ctx->sensors);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sensor bring-up failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "check wiring, then use the 'scan' console command");
        /* Deliberately not fatal: the console still comes up, so 'scan' is
         * available to diagnose exactly this situation. A bring-up build that
         * reboot-loops on a wiring fault is a bring-up build that helps nobody. */
    } else {
        /*
         * Restore calibration before the first sample, so nothing is ever reported
         * through a half-configured conversion. A first boot has nothing stored,
         * which is the normal path, not an error.
         */
        const esp_err_t cerr = cal_store_load(ctx);
        if (cerr == ESP_OK) {
            ESP_LOGI(TAG, "calibration restored from NVS ('cal' to review)");
        } else if (cerr == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGI(TAG, "no stored calibration -- defaults in use ('cal' to set it)");
        }

        /* After calibration is applied: the gauge integrates calibrated current, so
         * seeding it from an uncalibrated reading would anchor it to the wrong
         * number. */
        ESP_ERROR_CHECK(fg_init());

        sensors_report(ctx->sensors);
#if !defined(CONFIG_BATMON_INSTALL_MODE_P)
        ESP_LOGW(TAG,
                 "low-side: keep the full-scale shunt drop under ~100 mV, and make sure "
                 "the shunt is the ONLY connection between battery negative and system "
                 "ground -- a second path (chassis, second charger, or the USB cable to "
                 "this board) silently steals current around it, and the dual-sensor "
                 "cross-checks cannot see it. DESIGN.md 2.9.4 / 2.10.8");
#endif
    }

    ctx->stream_enabled   = BATMON_STREAM_ON_BOOT;
    ctx->stream_period_ms = CONFIG_BATMON_STREAM_PERIOD_MS;
    stats_reset(&ctx->window);

    if (ctx->sensors) {
        ctx->sensor_lock = xSemaphoreCreateMutex();
        if (!ctx->sensor_lock) {
            ESP_LOGE(TAG, "sensor lock alloc failed");
            return;
        }
        xTaskCreate(sampler_task, "sampler", 4096, ctx, 6, NULL);
    }

#if CONFIG_BATMON_DISPLAY_ENABLE
    /* After the sampler, so the first frame has something to show, and never fatal:
     * an absent panel is a configuration, not a fault (DESIGN.md 10). */
    display_debug_start(ctx);
#endif

#if CONFIG_BATMON_BLE_ENABLE
    /* Before console_start(): the BLE worker calls esp_console_run(), so the command
     * table must exist first -- but console_start() ends in the blocking REPL, so
     * "first" means before that call, and registration happens inside it. Starting
     * BLE here is safe because nothing can arrive over the air until the radio syncs,
     * which takes far longer than registering a dozen commands. */
    ble_console_start();
#endif

    console_start(ctx);
}
