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

#include "config.h"
#include "app_ctx.h"
#include "cli.h"
#include "ble.h"
#include "bme280.h"
#include "history_values.h"
#include "lcd.h"
#include "ota.h"
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
#include "values.h"

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

/*
 * The stream goes to BOTH transports.
 *
 * This is not symmetry for its own sake. the BLE bridge in cli.c captures stdout only for
 * the worker task, and only while a command is running -- so anything the sampler task
 * prints reaches USB and nothing else. That left a BLE client with no way to obtain
 * live telemetry at all, which is the one thing a phone app most needs. Emitting here
 * covers both, and ble_write() is a no-op when nobody is subscribed.
 *
 * CRLF for the radio, bare LF for the terminal: the BLE side matches the framing that
 * command responses use, so one client-side line splitter handles everything.
 */
static void stream_emit(const char *line)
{
    /* CRLF on both transports. The framing is defined in CRLF, so a stream line using
     * bare LF locally would leave the two wires subtly different after all. */
    printf("%s\r\n", line);
#if CONFIG_BATMON_BLE_ENABLE
    /* One notification, not two: a command reply landing between the record and its
     * CRLF would splice them in the client's byte stream. */
    char buf[192];
    const int n = snprintf(buf, sizeof(buf), "%s\r\n", line);
    if (n > 0 && (size_t)n < sizeof(buf)) {
        ble_write(buf, (size_t)n);
    } else {
        ble_write(line, 0);
        ble_write("\r\n", 2);
    }
#endif
}

/*
 * Three record types, one per group, each prefixed so a client can demultiplex a single
 * stream without tracking which command produced what:
 *
 *   f,<ms>,<volts>,<amps>
 *   c,<ms>,<watts>,<soc_pct>,<charge_ah>,<state>,<ocv_v>,<peukert>
 *   e,<ms>,<temp_c>,<humid_pct>,<press_hpa>
 *
 * Header lines are emitted once per group when the stream is enabled and start with
 * '#', so a parser can either use them or skip them by that single character.
 *
 * Splitting them is not cosmetic. A load step is an event you either catch at 10 Hz or
 * miss; SoC cannot meaningfully change faster than the gauge integrates; and a thermal
 * mass reported ten times a second is ten times the airtime for the same number.
 */
static void emit_headers(app_ctx_t *ctx)
{
    if (ctx->stream_csv_header_done) {
        return;
    }
    ctx->stream_csv_header_done = true;
    if (config()->rate_fast_ms) stream_emit("#f,ms,volts,amps");
    if (config()->rate_calc_ms) stream_emit("#c,ms,watts,soc_pct,charge_ah,state,ocv_v,peukert");
    if (config()->rate_diag_ms) stream_emit("#d,ms,shunt_mv,pga,sat");
    if (config()->rate_env_ms)  stream_emit("#e,ms,temp_c,humid_pct,press_hpa");
}

static void emit_fast(const power_sample_t *s)
{
    char line[96], bv[24], bi[24];
    snprintf(line, sizeof(line), "f,%lu,%s,%s",
             (unsigned long)(s->t_us / 1000), FMT_V(bv, s->v_pack_uv),
             FMT_A(bi, s->i_ua));
    stream_emit(line);
}

static void emit_calc(const power_sample_t *s)
{
    fg_status_t fg;
    fg_get(&fg);

    char line[160], bp[24], bsoc[24], bq[24], bo[24], bk[24];
    snprintf(line, sizeof(line), "c,%lu,%s,%s,%s,%s,%s,%s",
             (unsigned long)(s->t_us / 1000),
             FMT_W(bp, s->p_uw),
             fixed_fmt(bsoc, sizeof(bsoc), fg.soc_permille, 10, 1),
             fixed_fmt(bq, sizeof(bq), fg.charge_uas / 3600, 1000000, 3),
             fg_state_str(fg.state),
             FMT_V(bo, fg.ocv_uv),
             fixed_fmt(bk, sizeof(bk), fg.peukert_factor_q16, 65536, 3));
    stream_emit(line);
}

static void emit_diag(const power_sample_t *s)
{
    char line[112], bsh[24];
    snprintf(line, sizeof(line), "d,%lu,%s,%s,%d",
             (unsigned long)(s->t_us / 1000), FMT_MV(bsh, s->v_shunt_uv),
             ina219_pga_str(s->pga), s->saturated ? 1 : 0);
    stream_emit(line);
}

static void emit_env(app_ctx_t *ctx, int64_t now_us)
{
    /* Empty fields, not zeros, when there is no sensor: 0.00 C is a plausible reading
     * and would be indistinguishable from a real one. */
    char line[112], bt[16] = "", bh[16] = "", bpr[16] = "";
    if (values()->env_valid) {
        fixed_fmt(bt, sizeof(bt), values()->env.temp_centi_c, 100, 2);
        fixed_fmt(bpr, sizeof(bpr), (int64_t)values()->env.press_pa, 100, 2);
        if (values()->env.have_humidity) {
            fixed_fmt(bh, sizeof(bh), values()->env.humid_centi, 100, 1);
        }
    }
    snprintf(line, sizeof(line), "e,%lu,%s,%s,%s",
             (unsigned long)(now_us / 1000), bt, bh, bpr);
    stream_emit(line);
}

static void print_sample_line(const power_sample_t *s, const sample_stats_t *w)
{
    char bv[24], bi[24], bp[24], bsh[24], bmean[24], bsd[24];
    char line[224];

    snprintf(line, sizeof(line),
           "V=%9s V  I=%11s A  P=%10s W  | shunt %8s mV  pga %-14s%s | "
           "win n=%-5lu mean %9s A  sd %9s A",
           FMT_V(bv, s->v_pack_uv),
           FMT_A(bi, s->i_ua),
           FMT_W(bp, s->p_uw),
           FMT_MV(bsh, s->v_shunt_uv),
           ina219_pga_str(s->pga),
           s->saturated ? " SAT" : "",
           (unsigned long)w->n,
           FMT_A(bmean, stats_mean_ua(w)),
           FMT_A(bsd, stats_stddev_ua(w)));
    stream_emit(line);
}

/*
 * The sampler ticks faster than the sensor converts and polls CNVR, per
 * DESIGN.md §4.1. In M1 it also owns the console stream; from M3 it will push
 * onto a queue and own nothing else.
 */
static void sampler_task(void *arg)
{
    app_ctx_t *ctx = arg;

    int64_t next_fast_us = esp_timer_get_time();
    int64_t next_calc_us = next_fast_us;
    int64_t next_env_us  = next_fast_us;
    /* Last sample actually emitted, so a group never sends the same measurement twice.
     * Without this, asking for 10 Hz from a 7.3 Hz sensor produces duplicate records
     * that a client would count as real samples. */
    int64_t last_fast_t  = 0;
    int64_t last_calc_t  = 0;
    int64_t next_diag_us = next_fast_us;
    /* Previous range and saturation, so a change can be reported the instant it
     * happens rather than at the next periodic slot. */
    ina219_pga_t last_pga = (ina219_pga_t)0xFF;
    bool         last_sat = false;
    bool         diag_primed = false;
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
            values()->last       = s;
            values()->last_valid = true;
            values()->n_samples++;
            stats_add(history_window(), &s);
            /* The gauge sees every accepted sample and nothing else -- a rejected
             * read must never reach the integrator (DESIGN.md §5.2). */
            fg_update(&s);
            break;
        case ESP_ERR_NOT_FINISHED:
            values()->err_not_finished++;
            break;
        case ESP_ERR_INVALID_STATE:
            /* Either a range discard or unresolved roles. The latter is a standing
             * condition, so warn once rather than every 100 ms. */
            if (sensors_get_role_state(ctx->sensors) != SENSORS_ROLE_RESOLVED) {
                values()->err_unresolved++;
                if (!warned_unresolved) {
                    warned_unresolved = true;
                    ESP_LOGW(TAG, "roles unresolved -- NOT integrating. Apply a load "
                                  "and run 'detect', or set the mode in menuconfig.");
                }
                vTaskDelay(pdMS_TO_TICKS(500));
            } else {
                values()->err_range_discard++;
            }
            break;
        default:
            values()->err_bus++;
            ESP_LOGE(TAG, "sensor read failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(200));
            break;
        }

        const int64_t now = esp_timer_get_time();

        /*
         * Read the sensor on the same cadence it is reported at -- there is no point
         * sampling a thermal mass faster than anything consumes it. One forced read
         * costs ~12 ms, affordable every ten seconds and not at 10 Hz.
         */
        if (ctx->bme && now >= values()->env_next_us) {
            const uint32_t ems = config()->rate_env_ms ? config()->rate_env_ms : 10000;
            values()->env_next_us   = now + (int64_t)ems * 1000;
            bme280_sample_t e;
            if (bme280_read(ctx->bme, &e) == ESP_OK) {
                values()->env       = e;
                values()->env_valid = true;
            } else {
                /* §10: a temperature fault is not a gauge fault. Keep the last good
                 * value, keep counting, and retry on the next cadence. */
                ESP_LOGW(TAG, "environmental read failed; keeping last value");
            }
        }

        if (config()->stream_enabled && values()->last_valid) {
            if (config()->stream_csv) {
                emit_headers(ctx);

                /* Each group keeps its own deadline. A slow group cannot delay a fast
                 * one, and a group disabled with 0 simply never becomes due. */
                if (config()->rate_fast_ms && now >= next_fast_us &&
                    values()->last.t_us != last_fast_t) {
                    next_fast_us = now + (int64_t)config()->rate_fast_ms * 1000;
                    last_fast_t  = values()->last.t_us;
                    emit_fast(&values()->last);
                }
                if (config()->rate_calc_ms && now >= next_calc_us &&
                    values()->last.t_us != last_calc_t) {
                    next_calc_us = now + (int64_t)config()->rate_calc_ms * 1000;
                    last_calc_t  = values()->last.t_us;
                    emit_calc(&values()->last);
                }
                if (config()->rate_diag_ms) {
                    /* Change-or-deadline, whichever comes first. The change test is
                     * what makes a 1 s period acceptable for something a client needs
                     * to know about immediately. */
                    const bool changed = diag_primed &&
                                         (values()->last.pga != last_pga ||
                                          values()->last.saturated != last_sat);
                    if (changed || now >= next_diag_us) {
                        next_diag_us = now + (int64_t)config()->rate_diag_ms * 1000;
                        last_pga     = values()->last.pga;
                        last_sat     = values()->last.saturated;
                        diag_primed  = true;
                        emit_diag(&values()->last);
                    } else if (!diag_primed) {
                        last_pga    = values()->last.pga;
                        last_sat    = values()->last.saturated;
                        diag_primed = true;
                    }
                }
                if (config()->rate_env_ms && now >= next_env_us) {
                    next_env_us = now + (int64_t)config()->rate_env_ms * 1000;
                    emit_env(ctx, now);
                }
            } else if (config()->rate_calc_ms && now >= next_calc_us) {
                /* Text mode stays one line per tick, at the calculated group's rate:
                 * it is for a person reading, and 10 Hz of scrolling is unreadable. */
                next_calc_us = now + (int64_t)config()->rate_calc_ms * 1000;
                print_sample_line(&values()->last, history_window());
            }
        }

        /* 20 ms, not 100: a 10 Hz group needs the loop to come round considerably
         * faster than the deadline it is servicing, or emission jitters by a whole
         * tick. The INA219 read itself is what paces this loop in practice. */
        vTaskDelay(pdMS_TO_TICKS(20));
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

    /* Early, so a new image's probation clock starts at boot rather than after sensor
     * bring-up -- which is exactly the part most likely to hang in a bad build. */
    ota_init();

#if CONFIG_BATMON_INIT_ANTENNA_PINS
    init_antenna_pins();
#endif

    /* Settings before hardware: config_load() below applies them to whatever came
     * up, and a driver must never be configured from a struct that is still zeroed. */
    config_defaults();
    config_bind(ctx);

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
        /*
         * The gauge comes up first because config_load() pushes settings INTO it, and
         * it must exist to receive them. It restores its own accumulated charge here
         * -- history, not configuration -- and does not integrate anything until the
         * first sample, which is still several steps away.
         */
        ESP_ERROR_CHECK(fg_init());

        /*
         * Then the settings, before the first sample, so nothing is ever reported
         * through a half-configured conversion. A first boot has nothing stored, which
         * is the normal path and not an error.
         */
        const esp_err_t cerr = config_load();
        if (cerr == ESP_OK) {
            ESP_LOGI(TAG, "settings restored from NVS ('config' to review)");
        } else if (cerr == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGI(TAG, "no stored settings -- defaults in use ('cal' to calibrate)");
        }

#if CONFIG_BATMON_BME_ENABLE
        /*
         * Absence is a configuration, not a failure (§10, TEMP_UNAVAILABLE): the gauge
         * runs without it and the §5.6 corrections simply stay disabled rather than
         * being guessed from the die sensor.
         */
        bme280_config_t bcfg = BME280_CONFIG_DEFAULT();
        bcfg.i2c_addr        = CONFIG_BATMON_BME_ADDR;
        bcfg.scl_speed_hz    = CONFIG_BATMON_I2CA_FREQ_HZ;
        if (bme280_init(ctx->bus_a, &bcfg, &ctx->bme) != ESP_OK) {
            ESP_LOGI(TAG, "no BME/BMP280 fitted -- temperature unavailable");
        }
#endif

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

    /* No stream defaults here any more: config_defaults() set them from Kconfig and
     * config_load() may have replaced them with something the user chose. Assigning
     * them at this point would quietly undo every persisted telemetry rate. */
    stats_reset(history_window());

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
    lcd_start(ctx);
#endif

#if CONFIG_BATMON_BLE_ENABLE
    /* Before cli_start(): the BLE worker calls esp_console_run(), so the command
     * table must exist first -- but cli_start() ends in the blocking REPL, so
     * "first" means before that call, and registration happens inside it. Starting
     * BLE here is safe because nothing can arrive over the air until the radio syncs,
     * which takes far longer than registering a dozen commands. */
    cli_ble_start();
#endif

    /*
     * One more pass now that the panel and the radio exist: config_apply() skips them
     * when they are not up yet, and they are started after the settings are loaded.
     * Idempotent by construction -- it only ever writes what the struct already says.
     */
    config_apply();

    cli_start(ctx);
}
