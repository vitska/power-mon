/*
 * bme280.h — BME280 / BMP280 environmental sensor (DESIGN.md §2.4, §4.4).
 *
 * One driver for both parts, distinguished by chip ID at probe: 0x60 is a BME280 and
 * has humidity, 0x58 is a BMP280 and does not. §2.4 says to prefer the BMP280 when
 * choosing fresh — cheaper, identical for our purposes — so the humidity channel is
 * genuinely optional rather than assumed.
 *
 * Forced mode, not continuous. The design reads temperature at most once a minute
 * (§4.4), so a sensor that sleeps between measurements and costs ~0.1 µA idle is worth
 * far more than one that streams. Each read triggers a conversion, waits for it, and
 * returns.
 *
 * Fixed point throughout, per §4.3's no-floats rule: the compensation formulae are
 * Bosch's own integer variants from the datasheet, not a float port of them.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BME_CHIP_NONE = 0,
    BME_CHIP_BMP280,  /**< 0x58 — temperature and pressure */
    BME_CHIP_BME280,  /**< 0x60 — temperature, pressure and humidity */
} bme280_chip_t;

typedef struct bme280_dev_t *bme280_handle_t;

typedef struct {
    uint8_t  i2c_addr;      /**< 0 probes 0x76 then 0x77 */
    uint32_t scl_speed_hz;
} bme280_config_t;

#define BME280_CONFIG_DEFAULT()   \
    ((bme280_config_t){           \
        .i2c_addr     = 0,        \
        .scl_speed_hz = 400000,   \
    })

typedef struct {
    int32_t  temp_centi_c;  /**< temperature, 0.01 °C — e.g. 2341 is 23.41 °C */
    uint32_t press_pa;      /**< pressure in whole pascals */
    uint32_t humid_centi;   /**< relative humidity, 0.01 % — zero on a BMP280 */
    bool     have_humidity;
} bme280_sample_t;

/** Probes, reads calibration, leaves the part asleep. ESP_ERR_NOT_FOUND when nothing
 *  answers, which §10 treats as a configuration rather than a fault. */
esp_err_t bme280_init(i2c_master_bus_handle_t bus, const bme280_config_t *cfg,
                      bme280_handle_t *out);

/** One forced-mode measurement. Blocks for the conversion (~10 ms at our
 *  oversampling), so call it from a task that can afford that. */
esp_err_t bme280_read(bme280_handle_t h, bme280_sample_t *out);

bme280_chip_t bme280_chip(bme280_handle_t h);
uint8_t       bme280_addr(bme280_handle_t h);
const char   *bme280_chip_str(bme280_chip_t c);

#ifdef __cplusplus
}
#endif
