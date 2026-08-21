/*
 * app_ctx.h — the small amount of shared state M1 needs.
 *
 * Kept deliberately minimal. The real architecture (DESIGN.md §3.3: sampler ->
 * gauge -> {ble, persist, ui}) arrives with M3; inventing its plumbing now, before
 * there is anything to plumb, would only mean rewriting it.
 *
 * Note what this file does NOT contain: any reference to individual INA219 devices.
 * The dual-sensor arrangement of §2.10 is entirely behind sensors.h, and the
 * application deals in power_sample_t. That is the whole point of that layer.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sensors.h"

/** Running statistics over a window of samples — the basis of the zero-current
 *  calibration (DESIGN.md §5.5) and of judging noise during bring-up. */
typedef struct {
    uint32_t n;
    int64_t  sum_ua;
    int64_t  sum_sq_ua;   /* in (uA)^2; int64 holds ~9e18, so |i| up to ~3 A over
                           * 1e6 samples stays in range with room to spare */
    int32_t  min_ua;
    int32_t  max_ua;
    uint32_t n_v;         /* fresh voltage samples; voltage is read on its own
                           * divisor (DESIGN.md 4.5) so it has its own count */
    int64_t  sum_uv;
    uint32_t min_uv;
    uint32_t max_uv;
} sample_stats_t;

void    stats_reset(sample_stats_t *s);
void    stats_add(sample_stats_t *s, const power_sample_t *smp);
int32_t stats_mean_ua(const sample_stats_t *s);
int32_t stats_stddev_ua(const sample_stats_t *s);
int32_t stats_mean_uv(const sample_stats_t *s);

typedef struct {
    i2c_master_bus_handle_t bus_a;
    sensors_handle_t        sensors;

    /*
     * Guards ALL sensor acquisition. The INA219 in triggered mode is a
     * trigger-then-collect pair, and two readers interleaving those steps steal each
     * other's conversions: the loser sees NOT_FINISHED until it gives up. That is
     * exactly what a blocking calibration read hit while the 100 ms sampler kept
     * running underneath it -- the symptom was ESP_ERR_TIMEOUT from a healthy sensor.
     *
     * The sampler takes it for one read; a calibration routine takes it for its whole
     * run, which is the point -- calibration owns the sensor while it works.
     */
    SemaphoreHandle_t sensor_lock;

    volatile bool     stream_enabled;
    volatile uint32_t stream_period_ms;
    /* CSV instead of the human-readable line: same data, straight into a spreadsheet
     * or a plot. A monitoring tool that cannot hand its numbers to something else is
     * only half a tool. */
    volatile bool     stream_csv;
    volatile bool     stream_csv_header_done;

    /* Guarded by nothing: M1 is single-writer (the sampler task) and the console
     * only reads. Promoted to the seqlock of DESIGN.md §3.3 in M3. */
    sample_stats_t window;
    power_sample_t last;
    bool           last_valid;

    uint32_t err_not_finished;
    uint32_t err_range_discard;
    uint32_t err_unresolved;
    uint32_t err_bus;
    uint32_t n_samples;
} app_ctx_t;

app_ctx_t *app_ctx(void);

/** Sensor-acquisition lock. Returns false on timeout, in which case the caller must
 *  NOT touch the sensors. Never call the blocking sensor helpers without it. */
bool sensor_lock_take(app_ctx_t *ctx, uint32_t timeout_ms);
void sensor_lock_give(app_ctx_t *ctx);

/** Registers the M1 bring-up console commands. */
void console_start(app_ctx_t *ctx);

/** Blocking zero-current calibration of the CURRENT sensor (DESIGN.md §5.5).
 *  Returns ESP_OK and writes the new offset on success; ESP_ERR_INVALID_STATE if the
 *  current was not stable enough to trust, in which case the offset is unchanged. */
esp_err_t run_zero_calibration(app_ctx_t *ctx, uint32_t n_samples,
                               int32_t *out_offset_ua, int32_t *out_stddev_ua);

/** Zero-VOLTAGE calibration of the VOLTAGE sensor: the bus-channel counterpart of
 *  run_zero_calibration(). Requires 0 V on VBUS -- pack disconnected. Returns
 *  ESP_ERR_INVALID_STATE if a real voltage is present, in which case nothing changes.
 *
 *  Unlike the current channel there is no noise floor to compare against: the bus LSB
 *  is 4 mV, so a genuine offset is either several counts or invisible. The guard is
 *  therefore on magnitude, not on spread. */
esp_err_t run_zero_voltage_calibration(app_ctx_t *ctx, uint32_t n_samples,
                                       int32_t *out_offset_uv, uint32_t *out_spread_uv);
