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
#include "esp_app_desc.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "bme280.h"
#include "sensors.h"



/*
 * The hardware this build is wired to, plus the lock that serialises access to it.
 * Measured values live in values.h, accumulated ones in history_values.h and
 * settings in config.h -- this struct is now only the devices themselves.
 */
typedef struct app_ctx_s {
    i2c_master_bus_handle_t bus_a;
    sensors_handle_t        sensors;
    bme280_handle_t         bme;

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

    /* Runtime, not configuration: whether the CSV header has been emitted on THIS
     * stream since it was enabled. Settings live in config.h. */
    volatile bool     stream_csv_header_done;



} app_ctx_t;

/*
 * CLI contract, for anything parsing this console programmatically.
 *
 * BATMON_CLI_PROTOCOL is bumped whenever an existing command's OUTPUT changes shape,
 * an argument's meaning changes, or the framing changes. Adding a new command does not
 * bump it: a client that does not know the command simply never sends it.
 *
 * BATMON_EOT terminates every response on the BLE transport. Without it a client can
 * only guess when a reply has finished -- a quiet-period heuristic, which is wrong
 * exactly when the device is slow, which is exactly when a command like `cal top` is
 * doing something interesting. 0x04 is invisible in a terminal, so the same stream
 * stays comfortable for a human.
 */
/*
 * 3: telemetry is split into three groups with independent rates, each emitted as its
 *    own prefixed record -- `f` fast, `c` calculated, `e` environmental. The single
 *    wide CSV row of protocol 2 is gone. Different quantities change at genuinely
 *    different speeds, and sending temperature at the current-sampling rate wastes
 *    airtime while sending current at the temperature rate loses the event you were
 *    watching for.
 * 2: the CSV stream gained temp_c, humid_pct and press_hpa columns.
 */
#define BATMON_CLI_PROTOCOL 3
/*
 * The firmware version has exactly one source: version.txt at the project root. ESP-IDF
 * reads it into the app descriptor, which is embedded in the image at a fixed offset --
 * so `ver`, `ota status`, and anything inspecting a .bin before flashing it (the phone
 * app does) all see the same string, and none can drift from the others. MAJOR.MINOR.PATCH;
 * see README.md "Versioning".
 */
#define BATMON_FW_VERSION   (esp_app_get_description()->version)
#define BATMON_EOT          '\x04'

app_ctx_t *app_ctx(void);

/** Sensor-acquisition lock. Returns false on timeout, in which case the caller must
 *  NOT touch the sensors. Never call the blocking sensor helpers without it. */
bool sensor_lock_take(app_ctx_t *ctx, uint32_t timeout_ms);
void sensor_lock_give(app_ctx_t *ctx);


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
