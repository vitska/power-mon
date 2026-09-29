/*
 * cli_sense.c -- the commands that measure, range and convert.
 *
 * `read` and `raw` take samples, `sensors`/`detect`/`shunt loc` say which physical
 * device is on which pole, and `gain`/`offset`/`pga`/`profile`/`sense`/`curve` are the
 * conversion from a register to an ampere. They sit together because they are one
 * argument with the hardware: what did it measure, and what does that reading mean.
 *
 * Split out of cli.c, which had grown to a quarter of the firmware in one file.
 * The framing, the transports and the command table stay there; see cli_internal.h
 * for what these files share.
 */

#include "cli.h"

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_ctx.h"
#include "driver/usb_serial_jtag.h"
#include "esp_err.h"
#include "esp_log.h"
#include "fixed_fmt.h"
#include "freertos/FreeRTOS.h"
#include "history_values.h"
#include "ina219.h"
#include "sdkconfig.h"
#include "sensors.h"
#include "cli_internal.h"

/* --- commands ---------------------------------------------------------------- */

int cmd_read(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (cli_no_sensors()) {
        return 1;
    }

    if (!sensor_lock_take(cli_ctx, 2000)) {
        printf("sensor busy -- try again\n");
        return 1;
    }
    power_sample_t s;
    const esp_err_t err = sensors_read_blocking(cli_ctx->sensors, &s);
    sensor_lock_give(cli_ctx);
    if (err != ESP_OK) {
        printf("read failed: %s\n", esp_err_to_name(err));
        if (err == ESP_ERR_INVALID_STATE) {
            printf("(roles unresolved -- see 'detect')\n");
        }
        return 1;
    }

    char bv[24], bi[24], bp[24], bsh[24], bl[24], bo[24];
    printf("  pack voltage %s V\n", FMT_V(bv, s.v_pack_uv));
    printf("  current      %s A\n", FMT_A(bi, s.i_ua));
    printf("  power        %s W\n", FMT_W(bp, s.p_uw));
    printf("  shunt drop   %s mV  (%s%s)\n", FMT_MV(bsh, s.v_shunt_uv),
           ina219_pga_str(s.pga), s.saturated ? ", SATURATED" : "");

    if (sensors_get_mode(cli_ctx->sensors) != SENSORS_MODE_SINGLE) {
        printf("  load voltage %s V   (diagnostic)\n", FMT_V(bl, s.v_load_uv));
        printf("  idle offset  %s A   (other sensor, inputs shorted)\n",
               FMT_A(bo, s.idle_offset_ua));

        /* Pack minus load minus shunt is the drop in the cabling and connectors
         * (DESIGN.md 2.10.5). Only meaningful with real current flowing and both
         * readings taken close together, so it is reported rather than acted on. */
        if (s.v_load_uv > 0 && s.v_pack_uv > s.v_load_uv) {
            const int64_t drop_uv = (int64_t)s.v_pack_uv - (int64_t)s.v_load_uv;
            const int64_t wiring_uv = drop_uv - (s.v_shunt_uv < 0 ? -s.v_shunt_uv
                                                                  : s.v_shunt_uv);
            char bw[24];
            printf("  wiring drop  %s mV  (pack - load - shunt)\n",
                   FMT_MV(bw, wiring_uv));
        }
    }
    return 0;
}

int cmd_sensors(int argc, char **argv)
{
    if (cli_no_sensors()) {
        return 1;
    }

    if (argc >= 3 && strcmp(argv[1], "mode") == 0) {
        sensors_mode_t m;
        if      (strcmp(argv[2], "p")      == 0) m = SENSORS_MODE_P;
        else if (strcmp(argv[2], "n")      == 0) m = SENSORS_MODE_N;
        else if (strcmp(argv[2], "single") == 0) m = SENSORS_MODE_SINGLE;
        else if (strcmp(argv[2], "auto")   == 0) m = SENSORS_MODE_AUTO;
        else { printf("usage: sensors mode <p|n|single|auto>\n"); return 1; }

        const esp_err_t err = sensors_set_mode(cli_ctx->sensors, m);
        stats_reset(history_window());
        if (err != ESP_OK) {
            printf("mode set but roles could not be assigned: %s\n",
                   esp_err_to_name(err));
        }
    } else if (argc >= 2) {
        printf("usage: sensors [mode <p|n|single|auto>]\n");
        return 1;
    }

    printf("mode          %s\n", sensors_mode_str(sensors_get_mode(cli_ctx->sensors)));
    printf("positive pole %s\n",
           sensors_have_pos(cli_ctx->sensors) ? "present" : "absent");
    printf("negative pole %s\n",
           sensors_have_neg(cli_ctx->sensors) ? "present" : "absent");

    if (sensors_get_role_state(cli_ctx->sensors) != SENSORS_ROLE_RESOLVED) {
        printf("roles        UNRESOLVED -- integration is disabled.\n");
        printf("             Apply a load and run 'detect', or set the mode.\n");
        return 0;
    }

    ina219_handle_t cd = sensors_current_dev(cli_ctx->sensors);
    ina219_handle_t vd = sensors_voltage_dev(cli_ctx->sensors);
    printf("current from  %s\n", cd == vd ? "shared device" : "dedicated device");
    printf("bus comp      %s\n",
           ina219_get_vbus_comp(vd) == INA219_VBUS_COMP_NONE ? "off (dedicated "
           "voltage sensor)" : "on (shared device, DESIGN.md 2.9.5)");
    return 0;
}

int cmd_detect(int argc, char **argv)
{
    if (cli_no_sensors()) {
        return 1;
    }

    uint32_t n = 32;
    if (argc >= 2) {
        const long v = strtol(argv[1], NULL, 10);
        if (v < 8 || v > 1024) {
            printf("sample count must be 8..1024\n");
            return 1;
        }
        n = (uint32_t)v;
    }

    printf("Shunt-position detection (DESIGN.md 2.10.6).\n");
    printf("APPLY A LOAD FIRST. At zero current the two installations are\n");
    printf("indistinguishable, and guessing would invert the sign of every\n");
    printf("subsequent measurement -- so this refuses rather than guesses.\n");

    const bool was_streaming = config()->stream_enabled;
    config()->stream_enabled    = false;

    int32_t pos_uv = 0, neg_uv = 0;
    esp_err_t err = ESP_ERR_TIMEOUT;
    if (sensor_lock_take(cli_ctx, 2000)) {
        err = sensors_detect_mode(cli_ctx->sensors, n, &pos_uv, &neg_uv);
        sensor_lock_give(cli_ctx);
    } else {
        printf("sensor busy -- try again\n");
    }

    config()->stream_enabled = was_streaming;

    char bp[24], bn[24];
    printf("  positive-pole sensor sees %s mV\n", FMT_MV(bp, pos_uv));
    printf("  negative-pole sensor sees %s mV\n", FMT_MV(bn, neg_uv));

    if (err != ESP_OK) {
        printf("INCONCLUSIVE -- nothing changed. Apply a larger load and retry.\n");
        return 1;
    }

    printf("Resolved: %s\n", sensors_mode_str(sensors_get_mode(cli_ctx->sensors)));
    printf("Check the sign: charging must read positive. If it does not, use\n");
    printf("'sense sign invert' rather than rewiring.\n");
    stats_reset(history_window());
    return 0;
}
int cmd_zero(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (!cli_current_dev_or_complain()) {
        return 1;
    }

    printf("Zero-current calibration of the CURRENT sensor (DESIGN.md 5.5).\n");
    printf("DISCONNECT THE LOAD AND THE CHARGER. The firmware cannot verify this;\n");
    printf("if current is flowing it will be baked into the offset permanently.\n");
    printf("One instant reading -- no averaging, no wait.\n");

    int32_t   offset = 0;
    esp_err_t err     = run_zero_calibration(cli_ctx, &offset);

    char b1[24];
    printf("  measured offset : %s A\n", FMT_A(b1, offset));

    if (err != ESP_OK) {
        printf("FAILED: %s. Offset unchanged.\n", esp_err_to_name(err));
        return 1;
    }

    printf("Applied. Note it down -- M1 does not persist anything (NVS lands in M3).\n");
    stats_reset(history_window());
    return 0;
}

/* Parses p|n|single|auto into a mode. Returns false and prints on a bad word. */
static bool parse_mode(const char *w, sensors_mode_t *out)
{
    if      (strcmp(w, "p") == 0 || strcmp(w, "pos") == 0) *out = SENSORS_MODE_P;
    else if (strcmp(w, "n") == 0 || strcmp(w, "neg") == 0) *out = SENSORS_MODE_N;
    else if (strcmp(w, "single") == 0)                     *out = SENSORS_MODE_SINGLE;
    else if (strcmp(w, "auto") == 0)                       *out = SENSORS_MODE_AUTO;
    else {
        printf("location must be p (positive lead), n (negative lead), single or auto\n");
        return false;
    }
    return true;
}

/*
 * Shunt location lives under `shunt` as well as `sensors mode`, on purpose: the two
 * facts a person adjusts about a shunt are its resistance and which lead it is in,
 * and having to remember that one is `shunt` and the other `sensors` is a trap. Both
 * spellings drive the same sensors_set_mode(); there is no second copy of the state.
 */
static int cmd_shunt_loc(int argc, char **argv)
{
    if (cli_no_sensors()) {
        return 1;
    }

    if (argc >= 3) {
        sensors_mode_t m;
        if (!parse_mode(argv[2], &m)) {
            return 1;
        }
        const esp_err_t err = sensors_set_mode(cli_ctx->sensors, m);
        stats_reset(history_window());
        if (err != ESP_OK) {
            printf("location set, but roles could not be assigned: %s\n",
                   esp_err_to_name(err));
        }
        if (m == SENSORS_MODE_AUTO) {
            printf("AUTO resolves only while current flows -- apply a load and run\n"
                   "'detect'. Until then the gauge refuses to integrate (2.10.6).\n");
        }
    }

    const sensors_mode_t m = sensors_get_mode(cli_ctx->sensors);
    printf("shunt location %s\n", sensors_mode_str(m));
    printf("roles          %s\n",
           sensors_get_role_state(cli_ctx->sensors) == SENSORS_ROLE_RESOLVED
               ? "resolved" : "UNRESOLVED -- not integrating");

    if (m == SENSORS_MODE_P) {
        printf("High-side: no common-mode limit, full PGA range available, and the\n");
        printf("positive-pole sensor supplies both current and voltage (2.10.3).\n");
    } else if (m == SENSORS_MODE_N) {
        printf("Low-side: keep the full-scale drop under ~100 mV (2.9.2), and make\n");
        printf("the shunt the ONLY path between battery negative and system ground\n");
        printf("-- a USB cable to this board can quietly become a second one (2.9.4).\n");
    }
    return 0;
}

int cmd_shunt(int argc, char **argv)
{
    /* `shunt loc ...` is handled before the device check: the location can be set
     * before roles resolve, which is exactly when it is most often needed. */
    if (argc >= 2 && (strcmp(argv[1], "loc") == 0 ||
                      strcmp(argv[1], "location") == 0)) {
        return cmd_shunt_loc(argc, argv);
    }

    ina219_handle_t dev = cli_current_dev_or_complain();
    if (!dev) {
        return 1;
    }

    if (argc >= 2) {
        const long v = strtol(argv[1], NULL, 10);
        if (v < 150 || v > 1000000) {
            printf("shunt must be 150..1000000 micro-ohms (below 150 uOhm the\n"
                   "full-scale current no longer fits the int32 reading). If the\n"
                   "value is unknown, 'cal shunt <uA>' solves it from a known current.\n");
            return 1;
        }
        ESP_ERROR_CHECK(ina219_set_shunt_uohm(dev, (uint32_t)v));
        stats_reset(history_window());
    }

    const uint32_t r = ina219_get_shunt_uohm(dev);
    char bm[24], bfs[24], blsb[24];

    /* Full scale is taken from the PGA *ceiling*, not from /8: on a low-side build
     * the ceiling is what the hardware can actually survive (DESIGN.md 2.9.2), so
     * quoting /8 would advertise a range this board must never reach. */
    const ina219_pga_t ceil_pga = ina219_get_pga_max(dev);
    const int64_t fs_uv  = ina219_pga_fullscale_uv(ceil_pga);
    const int64_t fs_ua  = (fs_uv * 1000000LL) / (int64_t)r;
    const int64_t lsb_ua = (10LL * 1000000LL) / (int64_t)r;     /* 10 uV / R */

    printf("shunt      %lu uOhm (%s mOhm)\n", (unsigned long)r,
           fixed_fmt(bm, sizeof(bm), r, 1000, 3));
    printf("full scale +/-%s A at the %s ceiling\n", FMT_A(bfs, fs_ua),
           ina219_pga_str(ceil_pga));
    printf("resolution %s A per count\n", FMT_A(blsb, lsb_ua));

    if (fs_uv > 100000 && sensors_get_mode(cli_ctx->sensors) != SENSORS_MODE_P) {
        char bd[24];
        printf("WARNING: %s mV full-scale drop exceeds the ~100 mV low-side budget.\n",
               FMT_MV(bd, fs_uv));
        printf("         Use a smaller shunt or lower the ceiling: sense pgamax 2\n");
    }
    printf("location   %s   ('shunt loc <p|n|single|auto>' to change)\n",
           sensors_mode_str(sensors_get_mode(cli_ctx->sensors)));
    return 0;
}

int cmd_gain(int argc, char **argv)
{
    ina219_handle_t dev = cli_current_dev_or_complain();
    if (!dev) {
        return 1;
    }

    if (argc >= 2) {
        const long v = strtol(argv[1], NULL, 10);
        const esp_err_t err = ina219_set_gain_ppm(dev, (uint32_t)v);
        if (err != ESP_OK) {
            printf("rejected: gain must be 900000..1100000 ppm. A correction larger\n"
                   "than that means the shunt value is wrong, not the gain.\n");
            return 1;
        }
        stats_reset(history_window());
    }
    printf("gain %lu ppm\n", (unsigned long)ina219_get_gain_ppm(dev));
    return 0;
}

int cmd_offset(int argc, char **argv)
{
    ina219_handle_t dev = cli_current_dev_or_complain();
    if (!dev) {
        return 1;
    }

    if (argc >= 2) {
        ESP_ERROR_CHECK(ina219_set_offset_ua(dev,
                                             (int32_t)strtol(argv[1], NULL, 10)));
        stats_reset(history_window());
    }
    char b[24];
    printf("offset %ld uA (%s A)\n", (long)ina219_get_offset_ua(dev),
           FMT_A(b, ina219_get_offset_ua(dev)));
    return 0;
}

int cmd_pga(int argc, char **argv)
{
    ina219_handle_t dev = cli_current_dev_or_complain();
    if (!dev) {
        return 1;
    }

    if (argc >= 2) {
        if (strcmp(argv[1], "auto") == 0) {
            ESP_ERROR_CHECK(ina219_set_autorange(dev, true));
        } else {
            const long v = strtol(argv[1], NULL, 10);
            ina219_pga_t p;
            switch (v) {
            case 1: p = INA219_PGA_1; break;
            case 2: p = INA219_PGA_2; break;
            case 4: p = INA219_PGA_4; break;
            case 8: p = INA219_PGA_8; break;
            default:
                printf("usage: pga <auto|1|2|4|8>\n");
                return 1;
            }
            ESP_ERROR_CHECK(ina219_set_autorange(dev, false));
            ESP_ERROR_CHECK(ina219_set_pga(dev, p));
        }
    }
    printf("pga %s, autorange %s\n", ina219_pga_str(ina219_get_pga(dev)),
           ina219_get_autorange(dev) ? "on" : "off");
    return 0;
}

/*
 * Three profiles, which really are three sample rates with different costs:
 *
 *   continuous  128x averaging, 7.3 Hz pair rate -- the quietest, and the default
 *   fast         64x averaging, 14.7 Hz          -- what a 10 Hz telemetry group needs
 *   triggered    one conversion on demand        -- the low-power tier of §9.4
 *
 * `fast` exists because asking the fast telemetry group for 10 Hz against a 7.3 Hz
 * sensor cannot work: records would either duplicate or arrive late. Buying the rate
 * costs noise, so it is opt-in rather than the default.
 */
int cmd_profile(int argc, char **argv)
{
    ina219_handle_t dev = cli_current_dev_or_complain();
    if (!dev) {
        return 1;
    }
    ina219_handle_t vd = sensors_voltage_dev(cli_ctx->sensors);

    if (argc >= 2) {
        if (strcmp(argv[1], "continuous") == 0) {
            ESP_ERROR_CHECK(ina219_set_continuous_adc(dev, INA219_ADC_128AVG));
            if (vd && vd != dev) ina219_set_continuous_adc(vd, INA219_ADC_128AVG);
            ESP_ERROR_CHECK(ina219_set_profile(dev, INA219_PROFILE_CONTINUOUS));
            if (vd && vd != dev) ina219_set_profile(vd, INA219_PROFILE_CONTINUOUS);
        } else if (strcmp(argv[1], "fast") == 0) {
            ESP_ERROR_CHECK(ina219_set_continuous_adc(dev, INA219_ADC_64AVG));
            if (vd && vd != dev) ina219_set_continuous_adc(vd, INA219_ADC_64AVG);
            ESP_ERROR_CHECK(ina219_set_profile(dev, INA219_PROFILE_CONTINUOUS));
            if (vd && vd != dev) ina219_set_profile(vd, INA219_PROFILE_CONTINUOUS);
            printf("64x averaging: ~14.6 Hz, and about 40%% more noise per sample.\n");
            printf("Re-check 'stats' sigma against the 3 mA deadband (DESIGN.md 5.2).\n");
        } else if (strcmp(argv[1], "triggered") == 0) {
            ESP_ERROR_CHECK(ina219_set_profile(dev, INA219_PROFILE_TRIGGERED));
            if (vd && vd != dev) ina219_set_profile(vd, INA219_PROFILE_TRIGGERED);
        } else {
            printf("usage: profile <continuous|fast|triggered>\n");
            return 1;
        }
        stats_reset(history_window());

        /* Persist, on the same terms as `sense`: only if a store already exists, so a
         * never-calibrated board is not given one by a profile change. */
        if (config_exists()) {
            cli_cal_autosave();
        }
    }

    /* ina219_conversion_time_us() already covers BOTH channels -- the shunt and bus
     * conversions are sequential and it sums them. Multiplying by two here was double
     * counting, and reported 7.3 Hz for a profile genuinely running at 14.7. */
    const uint32_t pair = ina219_conversion_time_us(dev);
    char           b1[24];
    printf("profile     %s\n",
           ina219_get_profile(dev) == INA219_PROFILE_TRIGGERED ? "triggered"
           : (ina219_get_continuous_adc(dev) == INA219_ADC_128AVG ? "continuous (128x)"
                                                                 : "fast (64x)"));
    printf("conversion  %lu us for a shunt+bus pair\n", (unsigned long)pair);
    printf("=> up to %s samples/s at the ADC\n",
           fixed_fmt(b1, sizeof(b1), pair ? 10000000 / (int64_t)pair : 0, 10, 1));
    printf("The stream's fast group is separately capped by 'stream fast <ms>'; the\n");
    printf("rate you observe is the lower of the two, and records are never repeated.\n");
    return 0;
}

int cmd_sense(int argc, char **argv)
{
    ina219_handle_t dev = cli_current_dev_or_complain();
    if (!dev) {
        return 1;
    }

    if (argc >= 3 && strcmp(argv[1], "sign") == 0) {
        const bool inv = (strcmp(argv[2], "invert") == 0 || strcmp(argv[2], "1") == 0);
        ESP_ERROR_CHECK(ina219_set_invert_sign(dev, inv));
        stats_reset(history_window());
    } else if (argc >= 3 && strcmp(argv[1], "vbuscomp") == 0) {
        ina219_vbus_comp_t c;
        if      (strcmp(argv[2], "none") == 0) c = INA219_VBUS_COMP_NONE;
        else if (strcmp(argv[2], "add")  == 0) c = INA219_VBUS_COMP_ADD_SHUNT;
        else if (strcmp(argv[2], "sub")  == 0) c = INA219_VBUS_COMP_SUB_SHUNT;
        else { printf("usage: sense vbuscomp <none|add|sub>\n"); return 1; }
        ESP_ERROR_CHECK(ina219_set_vbus_comp(sensors_voltage_dev(cli_ctx->sensors), c));
    } else if (argc >= 3 && strcmp(argv[1], "pgamax") == 0) {
        ina219_pga_t p;
        switch (strtol(argv[2], NULL, 10)) {
        case 1: p = INA219_PGA_1; break;
        case 2: p = INA219_PGA_2; break;
        case 4: p = INA219_PGA_4; break;
        case 8: p = INA219_PGA_8; break;
        default: printf("usage: sense pgamax <1|2|4|8>\n"); return 1;
        }
        ESP_ERROR_CHECK(ina219_set_pga_max(dev, p));
    } else if (argc >= 2) {
        printf("usage: sense [sign <normal|invert>] [vbuscomp <none|add|sub>] "
               "[pgamax <1|2|4|8>]\n");
        return 1;
    }

    /* Any of the three settings above changes the conversion, so persist them the
     * same way a cal point is persisted -- but only if a store already exists, so a
     * board that has never been calibrated is not given one by a `sense` poke. */
    if (argc >= 3 && config_exists()) {
        cli_cal_autosave();
    }

    static const char *vc[] = {"none", "add shunt", "subtract shunt"};
    printf("sign        %s\n",
           ina219_get_invert_sign(dev) ? "inverted" : "normal (charge = +)");
    printf("vbus comp   %s\n",
           vc[ina219_get_vbus_comp(sensors_voltage_dev(cli_ctx->sensors))]);
    printf("pga ceiling %s\n", ina219_pga_str(ina219_get_pga_max(dev)));

    const uint32_t r = ina219_get_shunt_uohm(dev);
    const int64_t fs_uv = ina219_pga_fullscale_uv(ina219_get_pga_max(dev));
    char bfs[24], bdrop[24];
    printf("=> max current at the ceiling: +/-%s A  (drop %s mV)\n",
           FMT_A(bfs, (fs_uv * 1000000LL) / (int64_t)r), FMT_MV(bdrop, fs_uv));

    if (sensors_get_mode(cli_ctx->sensors) != SENSORS_MODE_P) {
        printf("Low-side: keep that drop under ~100 mV (DESIGN.md 2.9.2), and make\n");
        printf("sure the shunt is the only path between battery negative and system\n");
        printf("ground -- a USB cable to this board can quietly become a second one,\n");
        printf("and the dual-sensor cross-checks cannot detect it (2.9.4 / 2.10.8).\n");
    }
    return 0;
}

int cmd_scan(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("scanning I2C (SDA=GPIO%d SCL=GPIO%d)...\n",
           CONFIG_BATMON_I2CA_SDA_GPIO, CONFIG_BATMON_I2CA_SCL_GPIO);

    /* DESIGN.md 2.5: one shared bus, so the OLED and the temperature sensor are
     * expected company here, not a wiring mistake. Count the gauge sensors
     * separately -- a total device count cannot tell "both INA219s present" from
     * "one INA219 and a display", which is exactly the confusion worth avoiding
     * during bring-up. */
    int found = 0, ina = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(cli_ctx->bus_a, addr, 50) == ESP_OK) {
            const char *hint = "";
            if (addr == CONFIG_BATMON_ADDR_POS_POLE)      { hint = "  (positive-pole INA219)"; ina++; }
            else if (addr == CONFIG_BATMON_ADDR_NEG_POLE) { hint = "  (negative-pole INA219)"; ina++; }
            else if (addr >= 0x40 && addr <= 0x4F) hint = "  (INA219 range, unexpected address -- check A0/A1)";
            else if (addr == 0x3C || addr == 0x3D) hint = "  (SSD1306 OLED -- shares this bus)";
            else if (addr == 0x76 || addr == 0x77) hint = "  (BME280/BMP280 -- shares this bus, see 'env')";
            printf("  0x%02X%s\n", addr, hint);
            found++;
        }
    }
    if (!found) {
        printf("  nothing found. Check power, SDA/SCL not swapped, and pull-ups.\n");
    } else if (ina == 0) {
        printf("  no INA219 at 0x%02X or 0x%02X -- the gauge has no sensor. Check A0/A1\n",
               CONFIG_BATMON_ADDR_POS_POLE, CONFIG_BATMON_ADDR_NEG_POLE);
        printf("  strapping against DESIGN.md 2.10.2.\n");
    } else if (ina == 1) {
        printf("  only one INA219 -- the pair's cross-checks are unavailable and the\n");
        printf("  firmware falls back to single-sensor mode (DESIGN.md 2.10.7).\n");
    }
    return 0;
}

/* --- conversion curve (DESIGN.md 4.3, 8.3 tags 0x0002-0x0004) ----------------- */

/*
 * Both channels are `raw -> scale -> offset -> gain`, and this command exposes the
 * two trims per channel plus a one-point solve against a reference meter.
 *
 * Why a one-point solve and not two: the offset is already measured properly by
 * `zero` (DESIGN.md 5.5), which averages hundreds of samples with the load
 * disconnected. Asking for a second reference point here would invite people to
 * solve gain and offset simultaneously from two noisy readings, which fits the noise
 * as happily as the curve. So: `zero` sets offset at the origin, then `curve i ref`
 * sets gain at one well-chosen working point. That is a genuine two-point fit with
 * the first point taken where it can be taken accurately.
 */
static void curve_show(ina219_handle_t cd, ina219_handle_t vd)
{
    char b1[24], b2[24];

    printf("current channel (device at the %s pole)\n",
           sensors_get_mode(cli_ctx->sensors) == SENSORS_MODE_P ? "positive" : "negative");
    printf("  shunt    %lu uOhm\n", (unsigned long)ina219_get_shunt_uohm(cd));
    printf("  offset   %s A  (%ld uA)\n", FMT_A(b1, ina219_get_offset_ua(cd)),
           (long)ina219_get_offset_ua(cd));
    printf("  gain     %lu ppm  (%s%s%%)\n", (unsigned long)ina219_get_gain_ppm(cd),
           ina219_get_gain_ppm(cd) >= 1000000 ? "+" : "",
           fixed_fmt(b2, sizeof(b2),
                     (int64_t)ina219_get_gain_ppm(cd) - 1000000, 10000, 3));
    printf("  sign     %s\n", ina219_get_invert_sign(cd) ? "inverted" : "normal");

    printf("voltage channel (device at the %s pole)\n",
           vd == cd ? "same" : "other");
    printf("  divider  %lu q16  (x%s)\n",
           (unsigned long)ina219_get_vbus_divider_q16(vd),
           fixed_fmt(b1, sizeof(b1), ina219_get_vbus_divider_q16(vd), 65536, 4));
    printf("  offset   %s V  (%ld uV)\n", FMT_V(b2, ina219_get_vbus_offset_uv(vd)),
           (long)ina219_get_vbus_offset_uv(vd));
    printf("  gain     %lu ppm  (%s%s%%)\n",
           (unsigned long)ina219_get_vbus_gain_ppm(vd),
           ina219_get_vbus_gain_ppm(vd) >= 1000000 ? "+" : "",
           fixed_fmt(b1, sizeof(b1),
                     (int64_t)ina219_get_vbus_gain_ppm(vd) - 1000000, 10000, 3));
    printf("  buscomp  %s\n",
           ina219_get_vbus_comp(vd) == INA219_VBUS_COMP_NONE   ? "off" :
           ina219_get_vbus_comp(vd) == INA219_VBUS_COMP_ADD_SHUNT ? "add shunt"
                                                                 : "subtract shunt");
}

/* Progress dot for an averaging loop, at the cadence `curve_average` used to print
 * inline before the averaging moved into sensors.c. */
static void average_progress_dot(uint32_t index, void *ctx)
{
    (void)ctx;
    if ((index % 4) == 0) {
        printf(".");
        fflush(stdout);
    }
}

/* Averages n samples so a one-point gain solve is not decided by a single reading.
 * Thin wrapper: takes the sensor lock and prints progress/failure, sensors_average()
 * does the actual reading. */
static esp_err_t curve_average(uint32_t n, int64_t *sum_i_ua, int64_t *sum_v_uv,
                               uint32_t *got, bool *saturated)
{
    if (!sensor_lock_take(cli_ctx, 2000)) {
        printf("sensor busy -- try again\n");
        return ESP_ERR_TIMEOUT;
    }
    const esp_err_t err = sensors_average(cli_ctx->sensors, n, sum_i_ua, sum_v_uv, got,
                                          saturated, average_progress_dot, NULL);
    printf("\n");
    sensor_lock_give(cli_ctx);
    return err;
}

/*
 * Solves gain so that `measured` reads as `reference`, and prints what it found.
 * sensors_solve_gain_ppm() does the math; this is the console-facing wording around
 * it, including the one case it refuses: a literal zero measurement, where a ratio
 * against it is not a number.
 */
bool cli_solve_gain(int64_t measured, int64_t reference, uint32_t old_ppm,
                             uint32_t *new_ppm)
{
    if (!sensors_solve_gain_ppm(measured, reference, old_ppm, new_ppm)) {
        printf("measured value is exactly zero -- no ratio to solve a gain from.\n");
        return false;
    }
    char b1[24], b2[24];
    printf("measured %s, reference %s -> gain %lu ppm\n",
           fixed_fmt(b1, sizeof(b1), measured, 1000000, 4),
           fixed_fmt(b2, sizeof(b2), reference, 1000000, 4),
           (unsigned long)*new_ppm);
    return true;
}

int cmd_curve(int argc, char **argv)
{
    if (cli_no_sensors()) {
        return 1;
    }
    ina219_handle_t cd = sensors_current_dev(cli_ctx->sensors);
    ina219_handle_t vd = sensors_voltage_dev(cli_ctx->sensors);
    if (!cd || !vd) {
        printf("roles unresolved -- set the location first: shunt loc <p|n|single>\n");
        return 1;
    }

    if (argc < 2) {
        curve_show(cd, vd);
        printf("\n");
        printf("  curve i offset <uA> | gain <ppm> | ref <uA> [n]\n");
        printf("  curve v offset <uV> | gain <ppm> | ref <uV> [n] | divider <q16>\n");
        printf("  curve reset [i|v]\n");
        printf("'ref' solves gain from a bench-meter reading at the present load.\n");
        return 0;
    }

    if (strcmp(argv[1], "reset") == 0) {
        const bool all = (argc < 3);
        const bool doi = all || strcmp(argv[2], "i") == 0;
        const bool dov = all || strcmp(argv[2], "v") == 0;
        if (!doi && !dov) {
            printf("usage: curve reset [i|v]\n");
            return 1;
        }
        if (doi) {
            ESP_ERROR_CHECK(ina219_set_offset_ua(cd, 0));
            ESP_ERROR_CHECK(ina219_set_gain_ppm(cd, 1000000));
        }
        if (dov) {
            ESP_ERROR_CHECK(ina219_set_vbus_offset_uv(vd, 0));
            ESP_ERROR_CHECK(ina219_set_vbus_gain_ppm(vd, 1000000));
        }
        stats_reset(history_window());
        printf("reset%s%s. The divider ratio is hardware and is left alone.\n",
               doi ? " current" : "", dov ? " voltage" : "");
        curve_show(cd, vd);
        return 0;
    }

    const bool is_i = (strcmp(argv[1], "i") == 0);
    const bool is_v = (strcmp(argv[1], "v") == 0);
    if ((!is_i && !is_v) || argc < 3) {
        printf("usage: curve [i|v] <offset|gain|ref|divider> <value> [n]\n");
        return 1;
    }

    /* --- direct sets ---------------------------------------------------------- */
    if (strcmp(argv[2], "offset") == 0) {
        if (argc < 4) { printf("usage: curve %s offset <%s>\n", argv[1],
                               is_i ? "uA" : "uV"); return 1; }
        const long v = strtol(argv[3], NULL, 10);
        const esp_err_t err = is_i ? ina219_set_offset_ua(cd, (int32_t)v)
                                   : ina219_set_vbus_offset_uv(vd, (int32_t)v);
        if (err != ESP_OK) { printf("failed: %s\n", esp_err_to_name(err)); return 1; }
        stats_reset(history_window());
        cli_cal_autosave(); /* a direct set is as deliberate as `cal top`; save the same way */
        curve_show(cd, vd);
        return 0;
    }

    if (strcmp(argv[2], "gain") == 0) {
        if (argc < 4) { printf("usage: curve %s gain <ppm>\n", argv[1]); return 1; }
        const long v = strtol(argv[3], NULL, 10);
        const esp_err_t err = is_i ? ina219_set_gain_ppm(cd, (uint32_t)v)
                                   : ina219_set_vbus_gain_ppm(vd, (uint32_t)v);
        if (err != ESP_OK) {
            printf("failed: %s (gain is limited to 900000..1100000 ppm)\n",
                   esp_err_to_name(err));
            return 1;
        }
        stats_reset(history_window());
        cli_cal_autosave();
        curve_show(cd, vd);
        return 0;
    }

    if (is_v && strcmp(argv[2], "divider") == 0) {
        if (argc < 4) { printf("usage: curve v divider <q16, 65536 = 1.0>\n"); return 1; }
        const long v = strtol(argv[3], NULL, 10);
        const esp_err_t err = ina219_set_vbus_divider_q16(vd, (uint32_t)v);
        if (err != ESP_OK) {
            printf("failed: %s (ratio must be >= 65536, i.e. >= 1.0)\n",
                   esp_err_to_name(err));
            return 1;
        }
        stats_reset(history_window());
        cli_cal_autosave();
        curve_show(cd, vd);
        return 0;
    }

    /* --- one-point gain solve against a reference meter ----------------------- */
    if (strcmp(argv[2], "ref") == 0) {
        if (argc < 4) {
            printf("usage: curve %s ref <%s> [samples]\n", argv[1],
                   is_i ? "uA" : "uV");
            printf("Apply a steady, known %s first and read it on the bench meter.\n",
                   is_i ? "load" : "supply");
            return 1;
        }
        const long ref = strtol(argv[3], NULL, 10);
        uint32_t   n   = 64;
        if (argc >= 5) {
            const long ns = strtol(argv[4], NULL, 10);
            if (ns < 8 || ns > 1024) { printf("samples must be 8..1024\n"); return 1; }
            n = (uint32_t)ns;
        }

        const bool was_streaming = config()->stream_enabled;
        config()->stream_enabled    = false;

        int64_t  si = 0, sv = 0;
        uint32_t got = 0;
        bool     sat = false;
        const esp_err_t err = curve_average(n, &si, &sv, &got, &sat);

        config()->stream_enabled = was_streaming;

        if (err != ESP_OK || got == 0) {
            printf("read failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        (void)sat;

        uint32_t  want = 0;
        bool      ok;
        esp_err_t apply = ESP_OK;
        if (is_i) {
            ok = cli_solve_gain(si / (int64_t)got, ref, ina219_get_gain_ppm(cd), &want);
            if (ok) {
                apply = ina219_set_gain_ppm(cd, want);
            }
        } else {
            ok = cli_solve_gain(sv / (int64_t)got, ref, ina219_get_vbus_gain_ppm(vd), &want);
            if (ok) {
                apply = ina219_set_vbus_gain_ppm(vd, want);
            }
        }
        if (!ok) {
            printf("nothing changed.\n");
            return 1;
        }
        if (apply != ESP_OK) {
            /* Not ESP_ERROR_CHECK: with the solve's own +/-10%% band removed, a gain
             * this far from 1.0 is now reachable, and the driver's hardware-sanity
             * floor rejecting it must not crash the board -- it must just say no. */
            printf("solved %lu ppm, but the driver refused it: %s (limited to\n"
                   "900000..1100000 ppm). Nothing changed.\n",
                   (unsigned long)want, esp_err_to_name(apply));
            return 1;
        }
        stats_reset(history_window());
        curve_show(cd, vd);
        return 0;
    }

    printf("unknown: %s\n", argv[2]);
    return 1;
}

/* --- raw per-sensor readings --------------------------------------------------- */

/*
 * Each INA219 read directly, whatever role it has been given -- the view that answers
 * "is the current actually going through the shunt this board measures?" without a
 * meter. Machine-readable, like `config`, for the phone app's Sensors screen.
 *
 * Also a wiring check that needs no load: each chip's own bus voltage says which pole
 * it sits on. The positive-pole sensor sees the pack voltage; the negative-pole one
 * sits at ground and reads close to zero. The firmware assumes 0x40 is positive and
 * 0x41 negative, and `poles=` says whether the hardware agrees.
 */
#define RAW_SIDE_POS_UV 1000000 /* above 1 V: at the pack's positive side */
#define RAW_SIDE_GND_UV 300000  /* below 0.3 V: at ground */

static const char *raw_side(uint32_t bus_uv)
{
    return bus_uv > RAW_SIDE_POS_UV ? "positive" : bus_uv < RAW_SIDE_GND_UV ? "ground" : "unclear";
}

static bool raw_one(const char *key, ina219_handle_t d, uint32_t *bus_uv)
{
    if (!d) {
        printf("%s.present=0\n", key);
        return false;
    }
    ina219_sample_t smp;
    const esp_err_t err = ina219_read_blocking(d, &smp);
    printf("%s.present=1\n", key);
    const bool cur  = d == sensors_current_dev(cli_ctx->sensors);
    const bool volt = d == sensors_voltage_dev(cli_ctx->sensors);
    printf("%s.role=%s\n", key, cur && volt ? "current+voltage" : cur ? "current"
                                 : volt ? "voltage" : "idle");
    if (err != ESP_OK) {
        printf("%s.error=%s\n", key, esp_err_to_name(err));
        return false;
    }
    printf("%s.bus_uv=%lu\n", key, (unsigned long)smp.v_uv);
    printf("%s.side=%s\n", key, raw_side(smp.v_uv));
    printf("%s.shunt_uv=%ld\n", key, (long)smp.v_shunt_uv);
    /* Through this chip's own conversion: its shunt value, trims and sign. For the
     * idle chip that is nominal scaling -- the point is whether it sees current. */
    printf("%s.current_ua=%ld\n", key, (long)smp.i_ua);
    printf("%s.pga=%d\n", key, 1 << (int)smp.pga);
    printf("%s.range_uv=%ld\n", key, (long)ina219_pga_fullscale_uv(smp.pga));
    printf("%s.sat=%d\n", key, smp.saturated ? 1 : 0);
    /* This chip's OWN conversion settings, independent of which one is presently
     * assigned the current/voltage role -- the point of `raw` is to see both sides
     * well enough to decide the role for yourself. */
    printf("%s.shunt_uohm=%lu\n", key, (unsigned long)ina219_get_shunt_uohm(d));
    printf("%s.gain_ppm=%lu\n", key, (unsigned long)ina219_get_gain_ppm(d));
    printf("%s.offset_ua=%ld\n", key, (long)ina219_get_offset_ua(d));
    printf("%s.sign=%s\n", key, ina219_get_invert_sign(d) ? "invert" : "normal");
    *bus_uv = smp.v_uv;
    return true;
}

int cmd_raw(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (cli_no_sensors()) {
        return 1;
    }
    if (!sensor_lock_take(cli_ctx, 2000)) {
        printf("sensor busy -- try again\n");
        return 1;
    }
    uint32_t  pos_bus = 0, neg_bus = 0;
    const bool pos_ok = raw_one("pos", sensors_pos_dev(cli_ctx->sensors), &pos_bus);
    const bool neg_ok = raw_one("neg", sensors_neg_dev(cli_ctx->sensors), &neg_bus);
    sensor_lock_give(cli_ctx);

    printf("shunt.loc=%s\n", sensors_mode_str(sensors_get_mode(cli_ctx->sensors)));
    printf("shunt.uohm=%lu\n", (unsigned long)(sensors_current_dev(cli_ctx->sensors)
        ? ina219_get_shunt_uohm(sensors_current_dev(cli_ctx->sensors)) : 0));
    if (pos_ok && neg_ok) {
        const bool pos_pos = pos_bus > RAW_SIDE_POS_UV, neg_gnd = neg_bus < RAW_SIDE_GND_UV;
        const bool pos_gnd = pos_bus < RAW_SIDE_GND_UV, neg_pos = neg_bus > RAW_SIDE_POS_UV;
        printf("poles=%s\n", pos_pos && neg_gnd ? "ok" : pos_gnd && neg_pos ? "swapped" : "unclear");
    } else {
        printf("poles=single\n");
    }
    return 0;
}
