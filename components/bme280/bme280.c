/*
 * bme280.c — see bme280.h.
 *
 * The compensation functions are Bosch's integer reference implementations from the
 * BME280 datasheet (BST-BME280-DS002 §4.2.3 and §8.2), transcribed rather than
 * reinvented. They look like nothing else in this firmware on purpose: the magic
 * constants and shift counts are part of the published algorithm, and rewriting them
 * into something more readable is how people introduce errors that only show up at the
 * edges of the temperature range.
 */

#include "bme280.h"

#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bme280";

#define REG_CALIB_00  0x88 /* dig_T1 .. dig_H1, 26 bytes */
#define REG_CHIP_ID   0xD0
#define REG_RESET     0xE0
#define REG_CALIB_26  0xE1 /* dig_H2 .. dig_H6, 7 bytes */
#define REG_CTRL_HUM  0xF2
#define REG_STATUS    0xF3
#define REG_CTRL_MEAS 0xF4
#define REG_CONFIG    0xF5
#define REG_DATA      0xF7 /* press(3) temp(3) hum(2) */

#define CHIP_ID_BMP280 0x58
#define CHIP_ID_BME280 0x60

struct bme280_dev_t {
    i2c_master_dev_handle_t dev;
    bme280_chip_t           chip;
    uint8_t                 addr;

    /* Calibration, names as the datasheet uses them. */
    uint16_t T1;
    int16_t  T2, T3;
    uint16_t P1;
    int16_t  P2, P3, P4, P5, P6, P7, P8, P9;
    uint8_t  H1, H3;
    int16_t  H2, H4, H5;
    int8_t   H6;

    int32_t t_fine; /* carries temperature into the pressure and humidity maths */
};

static esp_err_t rd(bme280_handle_t h, uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(h->dev, &reg, 1, buf, n, 100);
}

static esp_err_t wr(bme280_handle_t h, uint8_t reg, uint8_t val)
{
    const uint8_t b[2] = {reg, val};
    return i2c_master_transmit(h->dev, b, 2, 100);
}

static esp_err_t read_calibration(bme280_handle_t h)
{
    uint8_t c[26];
    ESP_RETURN_ON_ERROR(rd(h, REG_CALIB_00, c, sizeof(c)), TAG, "calib0");

    h->T1 = (uint16_t)(c[1] << 8 | c[0]);
    h->T2 = (int16_t)(c[3] << 8 | c[2]);
    h->T3 = (int16_t)(c[5] << 8 | c[4]);
    h->P1 = (uint16_t)(c[7] << 8 | c[6]);
    h->P2 = (int16_t)(c[9] << 8 | c[8]);
    h->P3 = (int16_t)(c[11] << 8 | c[10]);
    h->P4 = (int16_t)(c[13] << 8 | c[12]);
    h->P5 = (int16_t)(c[15] << 8 | c[14]);
    h->P6 = (int16_t)(c[17] << 8 | c[16]);
    h->P7 = (int16_t)(c[19] << 8 | c[18]);
    h->P8 = (int16_t)(c[21] << 8 | c[20]);
    h->P9 = (int16_t)(c[23] << 8 | c[22]);
    /* c[24] is unused padding; H1 sits at 0xA1. */
    h->H1 = c[25];

    if (h->chip != BME_CHIP_BME280) {
        return ESP_OK; /* a BMP280 has no humidity block to read */
    }

    uint8_t d[7];
    ESP_RETURN_ON_ERROR(rd(h, REG_CALIB_26, d, sizeof(d)), TAG, "calib26");
    h->H2 = (int16_t)(d[1] << 8 | d[0]);
    h->H3 = d[2];
    /* H4 and H5 are 12-bit values sharing the nibbles of d[4]. */
    h->H4 = (int16_t)((int8_t)d[3] * 16 | (d[4] & 0x0F));
    h->H5 = (int16_t)((int8_t)d[5] * 16 | (d[4] >> 4));
    h->H6 = (int8_t)d[6];
    return ESP_OK;
}

/* --- Bosch reference compensation, integer variants --------------------------- */

static int32_t compensate_T(bme280_handle_t h, int32_t adc_T)
{
    int32_t var1, var2;
    var1 = ((((adc_T >> 3) - ((int32_t)h->T1 << 1))) * ((int32_t)h->T2)) >> 11;
    var2 = (((((adc_T >> 4) - ((int32_t)h->T1)) * ((adc_T >> 4) - ((int32_t)h->T1))) >>
             12) *
            ((int32_t)h->T3)) >>
           14;
    h->t_fine = var1 + var2;
    return (h->t_fine * 5 + 128) >> 8; /* 0.01 degC */
}

static uint32_t compensate_P(bme280_handle_t h, int32_t adc_P)
{
    int64_t var1, var2, p;
    var1 = (int64_t)h->t_fine - 128000;
    var2 = var1 * var1 * (int64_t)h->P6;
    var2 = var2 + ((var1 * (int64_t)h->P5) << 17);
    var2 = var2 + (((int64_t)h->P4) << 35);
    var1 = ((var1 * var1 * (int64_t)h->P3) >> 8) + ((var1 * (int64_t)h->P2) << 12);
    var1 = (((((int64_t)1) << 47) + var1)) * ((int64_t)h->P1) >> 33;
    if (var1 == 0) {
        return 0; /* the datasheet's own divide-by-zero guard */
    }
    p    = 1048576 - adc_P;
    p    = (((p << 31) - var2) * 3125) / var1;
    var1 = (((int64_t)h->P9) * (p >> 13) * (p >> 13)) >> 25;
    var2 = (((int64_t)h->P8) * p) >> 19;
    p    = ((p + var1 + var2) >> 8) + (((int64_t)h->P7) << 4);
    return (uint32_t)(p >> 8); /* Q24.8 Pa -> whole Pa */
}

static uint32_t compensate_H(bme280_handle_t h, int32_t adc_H)
{
    int32_t v = h->t_fine - ((int32_t)76800);
    v = (((((adc_H << 14) - (((int32_t)h->H4) << 20) - (((int32_t)h->H5) * v)) +
           ((int32_t)16384)) >>
          15) *
         (((((((v * ((int32_t)h->H6)) >> 10) *
              (((v * ((int32_t)h->H3)) >> 11) + ((int32_t)32768))) >>
             10) +
            ((int32_t)2097152)) *
               ((int32_t)h->H2) +
           8192) >>
          14));
    v = (v - (((((v >> 15) * (v >> 15)) >> 7) * ((int32_t)h->H1)) >> 4));
    if (v < 0) {
        v = 0;
    } else if (v > 419430400) {
        v = 419430400;
    }
    /* Q22.10 %RH. Scaled to 0.01 % here so callers never see a Q format. */
    return (uint32_t)(((uint64_t)(v >> 12) * 100) / 1024);
}

/* --- public -------------------------------------------------------------------- */

esp_err_t bme280_init(i2c_master_bus_handle_t bus, const bme280_config_t *cfg,
                      bme280_handle_t *out)
{
    ESP_RETURN_ON_FALSE(bus && cfg && out, ESP_ERR_INVALID_ARG, TAG, "args");

    /* Probe both addresses when none is pinned. SDO strapping decides between them and
     * breakouts disagree about which they use, so trying both beats making the user
     * find out from a datasheet. */
    const uint8_t candidates[2] = {cfg->i2c_addr ? cfg->i2c_addr : 0x76, 0x77};
    const int     n_cand        = cfg->i2c_addr ? 1 : 2;

    for (int i = 0; i < n_cand; i++) {
        const uint8_t addr = candidates[i];
        if (i2c_master_probe(bus, addr, 100) != ESP_OK) {
            continue;
        }

        bme280_handle_t h = calloc(1, sizeof(*h));
        if (!h) {
            return ESP_ERR_NO_MEM;
        }
        const i2c_device_config_t dc = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address  = addr,
            .scl_speed_hz    = cfg->scl_speed_hz,
        };
        if (i2c_master_bus_add_device(bus, &dc, &h->dev) != ESP_OK) {
            free(h);
            continue;
        }
        h->addr = addr;

        uint8_t id = 0;
        if (rd(h, REG_CHIP_ID, &id, 1) != ESP_OK) {
            i2c_master_bus_rm_device(h->dev);
            free(h);
            continue;
        }

        /* The chip ID is the only trustworthy way to tell the parts apart, and it is
         * also how a device that merely happens to live at 0x76 is rejected. */
        if (id == CHIP_ID_BME280) {
            h->chip = BME_CHIP_BME280;
        } else if (id == CHIP_ID_BMP280) {
            h->chip = BME_CHIP_BMP280;
        } else {
            ESP_LOGW(TAG, "0x%02X answered with chip id 0x%02X -- not a BME/BMP280",
                     addr, id);
            i2c_master_bus_rm_device(h->dev);
            free(h);
            continue;
        }

        if (read_calibration(h) != ESP_OK) {
            i2c_master_bus_rm_device(h->dev);
            free(h);
            return ESP_ERR_INVALID_RESPONSE;
        }

        /* IIR filter off: at one reading a minute there is nothing to filter, and the
         * filter would only make the first forced measurement after a wake unreliable. */
        (void)wr(h, REG_CONFIG, 0x00);
        if (h->chip == BME_CHIP_BME280) {
            (void)wr(h, REG_CTRL_HUM, 0x01); /* humidity oversampling x1 */
        }
        (void)wr(h, REG_CTRL_MEAS, 0x00); /* sleep until asked */

        ESP_LOGI(TAG, "%s at 0x%02X", bme280_chip_str(h->chip), addr);
        *out = h;
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t bme280_read(bme280_handle_t h, bme280_sample_t *out)
{
    ESP_RETURN_ON_FALSE(h && out, ESP_ERR_INVALID_ARG, TAG, "args");

    /* osrs_t = x1, osrs_p = x1, mode = forced. */
    ESP_RETURN_ON_ERROR(wr(h, REG_CTRL_MEAS, (1 << 5) | (1 << 2) | 0x01), TAG,
                        "trigger");

    /* Datasheet worst case at x1 oversampling is ~9.3 ms with humidity. Wait that,
     * then poll: a fixed delay alone would be a guess, and polling alone would spin. */
    vTaskDelay(pdMS_TO_TICKS(10));
    for (int i = 0; i < 10; i++) {
        uint8_t st = 0;
        ESP_RETURN_ON_ERROR(rd(h, REG_STATUS, &st, 1), TAG, "status");
        if ((st & 0x08) == 0) { /* measuring cleared */
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    uint8_t d[8] = {0};
    const size_t n = (h->chip == BME_CHIP_BME280) ? 8 : 6;
    ESP_RETURN_ON_ERROR(rd(h, REG_DATA, d, n), TAG, "data");

    const int32_t adc_P = ((int32_t)d[0] << 12) | ((int32_t)d[1] << 4) | (d[2] >> 4);
    const int32_t adc_T = ((int32_t)d[3] << 12) | ((int32_t)d[4] << 4) | (d[5] >> 4);

    /* Temperature first, always: t_fine feeds both other channels. */
    out->temp_centi_c = compensate_T(h, adc_T);
    out->press_pa     = compensate_P(h, adc_P);

    if (h->chip == BME_CHIP_BME280) {
        const int32_t adc_H = ((int32_t)d[6] << 8) | d[7];
        out->humid_centi   = compensate_H(h, adc_H);
        out->have_humidity = true;
    } else {
        out->humid_centi   = 0;
        out->have_humidity = false;
    }
    return ESP_OK;
}

bme280_chip_t bme280_chip(bme280_handle_t h)
{
    return h ? h->chip : BME_CHIP_NONE;
}

uint8_t bme280_addr(bme280_handle_t h)
{
    return h ? h->addr : 0;
}

const char *bme280_chip_str(bme280_chip_t c)
{
    switch (c) {
    case BME_CHIP_BME280: return "BME280";
    case BME_CHIP_BMP280: return "BMP280";
    default:              return "none";
    }
}
