/*
 * cli_cal.c -- the guided two-point calibration.
 *
 * Its own file because of what it costs to get wrong: `cal zero i` with a load
 * connected poisons the offset permanently and the firmware cannot tell. Every step
 * here states its physical precondition before it acts, and every accepted point writes
 * itself to flash. See CALIBRATION.md for the procedure these implement.
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

/* --- guided two-point calibration (cal) --------------------------------------- */

/*
 * `curve` exposes the individual terms; `cal` walks the two points that produce them
 * and does the arithmetic. Same state underneath -- there is exactly one offset and
 * one gain per channel, and both commands read and write those. Nothing here caches
 * a "calibration session".
 *
 * The two points are named for what you do at the bench, not for what the maths
 * calls them:
 *
 *   cal zero  -- nothing applied (no current / no voltage). Fixes the OFFSET.
 *   cal top   -- a known value applied, read from your meter. Fixes the GAIN.
 *
 * Order matters and is enforced only by advice, not by refusal: gain is solved
 * assuming the offset is already right, so `zero` before `top`. Running them the
 * other way round is not an error, it is just a worse fit, and refusing would get in
 * the way of someone re-trimming one term deliberately.
 */
/*
 * Every successful cal point writes itself to flash. The alternative -- measure, then
 * remember to type `cal save` -- means the one time it is forgotten is the time the
 * bench session has to be repeated, and these values cost a meter and a load to
 * obtain. Flash wear is a non-issue at one write per human action.
 */
void cli_cal_autosave(void)
{
    const esp_err_t err = config_capture_and_commit();
    if (err == ESP_OK) {
        printf("Saved to flash -- restored automatically at every boot.\n");
    } else {
        printf("WARNING: could not save (%s). This value is RAM only and will be\n",
               esp_err_to_name(err));
        printf("lost on reboot; retry with 'cal save'.\n");
    }
}

static void cal_status(ina219_handle_t cd, ina219_handle_t vd)
{
    char b1[24], b2[24];

    const int32_t  ioff = ina219_get_offset_ua(cd);
    const uint32_t igain = ina219_get_gain_ppm(cd);
    const int32_t  voff = ina219_get_vbus_offset_uv(vd);
    const uint32_t vgain = ina219_get_vbus_gain_ppm(vd);

    printf("stored     %s\n", config_exists()
               ? "yes -- restored at every boot"
               : "NO -- these values are RAM only until 'cal save'");
    printf("           zero point (offset)        top point (gain)\n");
    printf("current    %-10s %-12s  %lu ppm %s\n",
           FMT_A(b1, ioff), ioff ? "set" : "not set",
           (unsigned long)igain, igain != 1000000 ? "set" : "not set");
    printf("voltage    %-10s %-12s  %lu ppm %s\n",
           FMT_V(b2, voff), voff ? "set" : "not set",
           (unsigned long)vgain, vgain != 1000000 ? "set" : "not set");
}

/*
 * `cal top i <uA>` (alias `cal shunt`): everything about the current channel from one
 * known current, one instant reading -- no averaging, no wait. Nothing that was
 * configured before is trusted -- not the shunt resistance, not which pole's sensor
 * carries the current, not the sign -- because a known current and the raw shunt
 * voltages are enough to determine all three:
 *
 *   1. Read the raw shunt voltage on BOTH INA219s, right now. That is what each chip
 *      actually sees across its inputs, before any setting is applied (10 uV/count).
 *   2. The chip that sees the current is the one with the larger voltage: make it the
 *      current sensor, on whichever pole it is.
 *   3. Resistance = that voltage / the known current.
 *   4. Direction: the reference's sign is the truth (positive = charging); the board's
 *      sign is set so its reading agrees.
 *
 * Gain goes back to 1.0. A zero point taken earlier on the same chip stays valid: its
 * offset is kept as a voltage and rescaled to the new resistance.
 */
static int cal_shunt(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: cal top i <uA>\n");
        printf("Let a steady, known current flow and give the meter's reading now --\n");
        printf("this uses one instant reading of each sensor, taken the moment the\n");
        printf("command arrives. Positive is charging.\n");
        return 1;
    }
    if (cli_no_sensors()) {
        return 1;
    }
    char      *end = NULL;
    const long ref = strtol(argv[2], &end, 10);
    if (!end || *end != '\0') {
        printf("the current is micro-amps, a whole number: 5800000 for 5.8 A\n");
        return 1;
    }
    if (!sensor_lock_take(cli_ctx, 2000)) {
        printf("sensor busy -- try again\n");
        return 1;
    }
    sensors_shunt_cal_t cal;
    const esp_err_t err = sensors_calibrate_shunt(cli_ctx->sensors, ref, &cal);
    sensor_lock_give(cli_ctx);

    if (err == ESP_ERR_INVALID_ARG) {
        printf("the known current is exactly zero -- nothing to divide by.\n");
        return 1;
    }

    printf("  positive-pole sensor: %ld uV%s\n", (long)cal.v_pos_uv,
           cal.got_pos ? (cal.sat_pos ? " SATURATED" : "") : " (absent)");
    printf("  negative-pole sensor: %ld uV%s\n", (long)cal.v_neg_uv,
           cal.got_neg ? (cal.sat_neg ? " SATURATED" : "") : " (absent)");

    if (err == ESP_ERR_NOT_FOUND) {
        printf("no sensor answered -- nothing to calibrate from.\n");
        return 1;
    }
    if (err == ESP_ERR_INVALID_SIZE) {
        /* Not a crash: cal.shunt_uohm is what was computed and rejected, so this
         * prints it without ever dividing by it -- that division is exactly what
         * used to happen here, and a value this low is now reachable with no
         * validation upstream to catch it first. */
        char br[24];
        printf("computed %lu uOhm (%s mOhm), but the driver refused it: too low to\n"
               "trust. Nothing changed -- mode, sign, offset and gain are unchanged.\n",
               (unsigned long)cal.shunt_uohm, fixed_fmt(br, sizeof(br), cal.shunt_uohm, 1000, 3));
        return 1;
    }
    if (err != ESP_OK) {
        printf("failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    if (cal.mode_changed) {
        printf("  current is on the %s-pole sensor: install mode set to %s\n",
               cal.mode == SENSORS_MODE_N ? "negative" : "positive",
               sensors_mode_str(cal.mode));
    }
    stats_reset(history_window());

    char b1[24], b2[24];
    const int64_t fs_ua = ((int64_t)ina219_pga_fullscale_uv(ina219_get_pga_max(cal.dev)) *
                           1000000LL) / (int64_t)cal.shunt_uohm;
    printf("shunt %lu uOhm (%s mOhm), sign %s, gain 1.000000 -> reads %s A\n",
           (unsigned long)cal.shunt_uohm, fixed_fmt(b1, sizeof(b1), cal.shunt_uohm, 1000, 3),
           cal.inverted ? "inverted" : "normal", FMT_A(b2, ref));
    printf("full scale about +/-%s A\n", FMT_A(b1, fs_ua));
    cli_cal_autosave();
    return 0;
}

int cmd_cal(int argc, char **argv)
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
        cal_status(cd, vd);
        printf("\n");
        printf("  cal zero i            no current flowing -> current offset\n");
        printf("  cal zero v            no voltage on VBUS -> voltage offset\n");
        printf("  cal top i <uA>        known current, from your meter -> gain\n");
        printf("  cal top v <uV>        known voltage, from your meter -> gain\n");
        printf("  cal reset [i|v]       back to zero offset, unity gain\n");
        printf("  cal save              write to flash (zero/top do this for you)\n");
        printf("  cal forget            erase the stored calibration\n");
        printf("\n");
        printf("Values are MICRO-units: 2.0134 A is 2013400, 12.6543 V is 12654300.\n");
        printf("Do zero before top on each channel; gain is solved assuming the\n");
        printf("offset is already correct.\n");
        return 0;
    }

    /* --- cal save / forget ---------------------------------------------------- */
    if (strcmp(argv[1], "save") == 0) {
        const esp_err_t err = config_capture_and_commit();
        if (err == ESP_ERR_INVALID_STATE) {
            printf("roles unresolved -- nothing coherent to store yet.\n");
            return 1;
        }
        if (err != ESP_OK) {
            printf("save failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        printf("saved to flash. Restored automatically at every boot.\n");
        cal_status(cd, vd);
        return 0;
    }

    if (strcmp(argv[1], "forget") == 0) {
        const esp_err_t err = config_forget();
        if (err != ESP_OK) {
            printf("failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        printf("stored calibration erased. The live values are unchanged until the\n");
        printf("next reboot; 'cal reset' clears those too.\n");
        return 0;
    }

    /* --- cal reset ------------------------------------------------------------ */
    if (strcmp(argv[1], "reset") == 0) {
        const bool all = (argc < 3);
        const bool doi = all || strcmp(argv[2], "i") == 0;
        const bool dov = all || strcmp(argv[2], "v") == 0;
        if (!doi && !dov) {
            printf("usage: cal reset [i|v]\n");
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
        /* Persist the cleared state rather than leaving the old values on flash to
         * come back at the next boot -- a reset that undoes itself overnight is
         * worse than no reset at all. */
        if (config_exists()) {
            cli_cal_autosave();
        }
        printf("cleared, on flash too. The shunt resistance and divider ratio\n");
        printf("describe the hardware and are left alone.\n");
        cal_status(cd, vd);
        return 0;
    }

    const bool is_zero = (strcmp(argv[1], "zero") == 0);
    const bool is_top  = (strcmp(argv[1], "top") == 0);
    if (strcmp(argv[1], "shunt") == 0) {
        return cal_shunt(argc, argv);
    }
    if ((!is_zero && !is_top) || argc < 3) {
        printf("usage: cal <zero|top> <i|v> [value]\n");
        return 1;
    }

    const bool chan_i = (strcmp(argv[2], "i") == 0);
    const bool chan_v = (strcmp(argv[2], "v") == 0);
    if (!chan_i && !chan_v) {
        printf("channel must be 'i' (current) or 'v' (voltage)\n");
        return 1;
    }

    /* --- cal zero ------------------------------------------------------------- */
    if (is_zero) {
        char b1[24];

        if (chan_i) {
            printf("ZERO POINT, current channel.\n");
            printf("DISCONNECT THE LOAD AND THE CHARGER. The firmware cannot check\n");
            printf("this; any current flowing now becomes part of the offset.\n");
            printf("One instant reading -- no averaging, no wait.\n");

            int32_t off = 0;
            const esp_err_t err = run_zero_calibration(cli_ctx, &off);

            printf("  offset  %s A\n", FMT_A(b1, off));
            if (err != ESP_OK) {
                printf("FAILED: %s. Nothing changed.\n", esp_err_to_name(err));
                return 1;
            }
            printf("Applied. Now: cal top i <uA> with a known current flowing.\n");
            cli_cal_autosave();
        } else {
            printf("ZERO POINT, voltage channel.\n");
            printf("VBUS must be AT GROUND, not merely disconnected. It is measured\n");
            printf("against system ground, so an ungrounded input floats up to near\n");
            printf("the 3.3 V rail -- which looks exactly like a connected pack.\n");
            printf("Tie the VBUS node to GND. A voltage present now would be\n");
            printf("subtracted from every future reading.\n");
            printf("One instant reading -- no averaging, no wait.\n");

            int32_t off = 0;
            const esp_err_t err = run_zero_voltage_calibration(cli_ctx, &off);

            printf("  offset  %s V\n", FMT_V(b1, off));
            if (err != ESP_OK) {
                printf("FAILED: %s. Nothing changed.\n", esp_err_to_name(err));
                return 1;
            }
            if (off == 0) {
                printf("Zero counts -- there is no offset to correct. The bus LSB is\n");
                printf("4 mV, so anything smaller is invisible to the hardware and\n");
                printf("nothing is lost by leaving it at zero.\n");
            } else {
                printf("Applied. Now: cal top v <uV> with a known voltage applied.\n");
            }
            cli_cal_autosave();
        }
        stats_reset(history_window());
        return 0;
    }

    /* --- cal top -------------------------------------------------------------- */

    /* The current channel's known-value point always solves the shunt resistance from
     * the measured current (cal_shunt), whatever resistance is configured: the shunt's
     * real value is what the meter reading determines, and a +/-10 % gain trim around
     * a guessed resistance refused exactly the case it is needed for -- a shunt of
     * unknown value. `cal top i <uA>` is `cal shunt <uA>`. */
    if (chan_i) {
        char *sargv[3] = {argv[0], (char *)"shunt", argc >= 4 ? argv[3] : NULL};
        return cal_shunt(argc - 1, sargv);
    }

    /*
     * One instant reading, not an average: chan_i never reaches here (redirected to
     * cal_shunt() above), so this is the voltage channel only. cli_solve_gain()
     * scales linearly -- old_gain * ref / measured -- so the reading this solves
     * from is, by construction, exactly `ref` immediately afterward. Averaging a
     * changing input across many seconds is what used to make that not visibly
     * true; a single instant sample removes the gap between "what this solved
     * from" and "what the next reading shows" entirely, and removes the wait.
     */
    if (argc < 4) {
        printf("usage: cal top v <uV>\n");
        printf("Read the meter and send that value now -- this uses one instant\n");
        printf("reading, taken the moment the command arrives.\n");
        return 1;
    }
    const long ref = strtol(argv[3], NULL, 10);

    if (!sensor_lock_take(cli_ctx, 2000)) {
        printf("sensor busy -- try again\n");
        return 1;
    }
    power_sample_t s;
    const esp_err_t err = sensors_read_blocking(cli_ctx->sensors, &s);
    sensor_lock_give(cli_ctx);

    if (err != ESP_OK || !s.v_valid) {
        printf("read failed: %s\n", err == ESP_OK ? "no voltage reading" : esp_err_to_name(err));
        return 1;
    }

    uint32_t want = 0;
    if (!cli_solve_gain((int64_t)s.v_pack_uv, ref, ina219_get_vbus_gain_ppm(vd), &want)) {
        printf("nothing changed.\n");
        return 1;
    }
    /* Not ESP_ERROR_CHECK: the solve itself no longer refuses an implausible ratio,
     * so a far-off reference now reaches the driver's own 900000..1100000 ppm floor,
     * and that must not abort the board -- it must just refuse the write. */
    const esp_err_t apply = ina219_set_vbus_gain_ppm(vd, want);
    if (apply != ESP_OK) {
        printf("solved %lu ppm, but the driver refused it: %s (limited to\n"
               "900000..1100000 ppm). Nothing changed.\n",
               (unsigned long)want, esp_err_to_name(apply));
        return 1;
    }
    stats_reset(history_window());
    cli_cal_autosave();
    cal_status(cd, vd);
    return 0;
}
