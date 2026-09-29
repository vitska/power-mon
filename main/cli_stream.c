/*
 * cli_stream.c -- what the board says about itself over time.
 *
 * The telemetry groups and their rates, the statistics window, the repainting `mon`
 * dashboard, the environmental sensor and the `ver` handshake. All of it is output: no
 * command here changes how anything is measured.
 *
 * Split out of cli.c, which had grown to a quarter of the firmware in one file.
 * The framing, the transports and the command table stay there; see cli_internal.h
 * for what these files share.
 */

#include "cli.h"

#include "config.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_ctx.h"
#include "ble.h"
#include "bme280.h"
#include "driver/usb_serial_jtag.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "fixed_fmt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fuelgauge.h"
#include "history_values.h"
#include "ina219.h"
#include "lcd.h"
#include "sdkconfig.h"
#include "sensors.h"
#include "values.h"
#include "cli_internal.h"


/*
 * Telemetry groups. One flag enables the stream; three periods decide what appears in
 * it and how often. A group set to 0 is off without disturbing the others, which is the
 * point of splitting them -- a client that only wants temperature should not have to
 * receive 10 Hz of current to get it.
 */
static bool stream_set_rate(const char *what, const char *val)
{
    volatile uint32_t *target = NULL;
    uint32_t           maxms  = 60000;

    if (strcmp(what, "fast") == 0) {
        target = &config()->rate_fast_ms;
    } else if (strcmp(what, "calc") == 0) {
        target = &config()->rate_calc_ms;
    } else if (strcmp(what, "diag") == 0) {
        target = &config()->rate_diag_ms;
    } else if (strcmp(what, "env") == 0) {
        target = &config()->rate_env_ms;
        maxms  = 600000; /* a thermal mass may legitimately be reported once a minute */
    } else {
        return false;
    }

    if (strcmp(val, "off") == 0) {
        *target = 0;
        printf("%s group off\n", what);
        return true;
    }

    const long ms = strtol(val, NULL, 10);
    if (ms < 20 || ms > (long)maxms) {
        printf("%s period must be 20..%lu ms, or 'off'\n", what,
               (unsigned long)maxms);
        return true; /* handled, just rejected */
    }
    *target = (uint32_t)ms;

    /* The fast group can be asked for more than the sensor can deliver. Say so rather
     * than let someone conclude the firmware is dropping samples. */
    if (target == &config()->rate_fast_ms && ms < 137 &&
        ina219_get_continuous_adc(sensors_current_dev(cli_ctx->sensors)) ==
            INA219_ADC_128AVG) {
        printf("fast group %ld ms (%s Hz requested)\n", ms,
               ms ? (ms <= 100 ? "10" : "<10") : "0");
        printf("NOTE: the continuous profile converts every ~136 ms (128x hardware\n");
        printf("averaging), so the achieved rate is ~7.3 Hz. 'profile fast' drops to\n");
        printf("64x averaging for ~14.7 Hz, at roughly 40%% more noise per sample.\n");
    } else {
        printf("%s group %ld ms\n", what, ms);
    }
    return true;
}

static void stream_show(void)
{
    printf("stream  %s, format %s\n", config()->stream_enabled ? "on" : "off",
           config()->stream_csv ? "CSV (grouped records)" : "text");
    printf("  fast  %-6lu ms   voltage, current\n",
           (unsigned long)config()->rate_fast_ms);
    printf("  calc  %-6lu ms   power, SoC, charge, state, OCV, Peukert\n",
           (unsigned long)config()->rate_calc_ms);
    printf("  diag  %-6lu ms   shunt drop, range, saturation (plus on change)\n",
           (unsigned long)config()->rate_diag_ms);
    printf("  env   %-6lu ms   temperature, humidity, pressure\n",
           (unsigned long)config()->rate_env_ms);
    printf("0 means that group is off. Records are prefixed f, c, d and e; header\n");
    printf("lines start with '#'. Ignore prefixes you do not know -- new record types\n");
    printf("may appear without a protocol bump.\n");
}

int cmd_stream(int argc, char **argv)
{
    if (argc < 2) {
        stream_show();
        printf("\n");
        printf("  stream on | off\n");
        printf("  stream csv | text\n");
        printf("  stream fast|calc|diag|env <ms|off>\n");
        return 0;
    }

    if (strcmp(argv[1], "on") == 0) {
        config()->stream_enabled = true;
    } else if (strcmp(argv[1], "off") == 0) {
        config()->stream_enabled = false;
    } else if (strcmp(argv[1], "csv") == 0) {
        config()->stream_csv             = true;
        cli_ctx->stream_csv_header_done = false; /* re-emit the headers */
        config()->stream_enabled         = true;
    } else if (strcmp(argv[1], "text") == 0) {
        config()->stream_csv     = false;
        config()->stream_enabled = true;
    } else if (argc >= 3 && stream_set_rate(argv[1], argv[2])) {
        cli_ctx->stream_csv_header_done = false; /* the header set may have changed */
        return 0;
    } else {
        printf("usage: stream <on|off|csv|text|fast|calc|diag|env <ms|off>>\n");
        return 1;
    }

    stream_show();
    return 0;
}

int cmd_stats(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "reset") == 0) {
        stats_reset(history_window());
        printf("window reset\n");
        return 0;
    }

    const sample_stats_t *w = history_window();
    if (w->n == 0) {
        printf("no samples yet\n");
        return 0;
    }

    char b1[24], b2[24], b3[24], b4[24];
    printf("current samples   : %lu\n", (unsigned long)w->n);
    printf("current  mean     : %s A\n", FMT_A(b1, stats_mean_ua(w)));
    printf("         stddev   : %s A\n", FMT_A(b2, stats_stddev_ua(w)));
    printf("         min/max  : %s / %s A\n", FMT_A(b3, w->min_ua),
           FMT_A(b4, w->max_ua));
    if (w->n_v) {
        printf("voltage samples   : %lu\n", (unsigned long)w->n_v);
        printf("voltage  mean     : %s V\n", FMT_V(b1, stats_mean_uv(w)));
        printf("         min/max  : %s / %s V\n", FMT_V(b2, w->min_uv),
               FMT_V(b3, w->max_uv));
    }
    printf("\n");
    printf("total samples     : %lu\n", (unsigned long)values()->n_samples);
    printf("polls, not ready  : %lu\n", (unsigned long)values()->err_not_finished);
    printf("range discards    : %lu\n", (unsigned long)values()->err_range_discard);
    printf("unresolved skips  : %lu\n", (unsigned long)values()->err_unresolved);
    printf("bus errors        : %lu\n", (unsigned long)values()->err_bus);
    return 0;
}

/* --- environmental sensor (DESIGN.md 2.4, 4.4) -------------------------------- */

/*
 * A fresh forced-mode read rather than the sampler's cached value. The cache exists so
 * the stream and the display cost nothing; someone typing `env` is asking what the
 * sensor says NOW, and waiting 12 ms for the truth beats being handed a value up to a
 * minute old with no way to tell.
 */
int cmd_env(int argc, char **argv)
{
    (void)argc; (void)argv;

    if (!cli_ctx->bme) {
        printf("no BME280/BMP280 fitted.\n");
        printf("Absence is a configuration, not a fault: the gauge runs without it and\n");
        printf("the temperature corrections of DESIGN.md 5.6 stay disabled rather than\n");
        printf("being guessed. 'scan' shows whether anything answers at 0x76 or 0x77.\n");
        return 1;
    }

    bme280_sample_t e;
    const esp_err_t err = bme280_read(cli_ctx->bme, &e);
    if (err != ESP_OK) {
        printf("read failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    char b1[24];
    printf("chip        %s at 0x%02X\n", bme280_chip_str(bme280_chip(cli_ctx->bme)),
           bme280_addr(cli_ctx->bme));
    printf("temperature %s C\n", fixed_fmt(b1, sizeof(b1), e.temp_centi_c, 100, 2));
    printf("pressure    %s hPa\n",
           fixed_fmt(b1, sizeof(b1), (int64_t)e.press_pa, 100, 2));
    if (e.have_humidity) {
        printf("humidity    %s %%RH\n",
               fixed_fmt(b1, sizeof(b1), e.humid_centi, 100, 1));
    } else {
        printf("humidity    not available on a BMP280\n");
    }
    printf("\n");
    printf("This measures the BOARD, not the cells (DESIGN.md 2.7). Every correction\n");
    printf("in 5.6 inherits that error, which is why each one is switchable.\n");
    return 0;
}

/* --- version / handshake ------------------------------------------------------ */

/*
 * The first thing a programmatic client should send. Everything here is stable, fixed
 * order, one `key value` pair per line -- so a client can parse it without knowing any
 * of the prose formatting the other commands use.
 */
int cmd_ver(int argc, char **argv)
{
    (void)argc; (void)argv;

    esp_chip_info_t chip;
    esp_chip_info(&chip);

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);

    printf("protocol %d\n", BATMON_CLI_PROTOCOL);
    printf("firmware %s\n", BATMON_FW_VERSION);
    /* The version names a release; the build names the exact binary. Two local builds
     * of one version differ here, which is what tells them apart on the bench. */
    char build[17];
    esp_app_get_elf_sha256(build, sizeof(build));
    printf("build %s\n", build);
    printf("idf %s\n", esp_get_idf_version());
    printf("chip esp32c6 rev%d cores%d\n", chip.revision, chip.cores);
    printf("mac %02X:%02X:%02X:%02X:%02X:%02X\n", mac[0], mac[1], mac[2], mac[3],
           mac[4], mac[5]);
    printf("built %s %s\n", __DATE__, __TIME__);
    printf("units micro\n"); /* every numeric argument is an integer micro-unit */
    return 0;
}

/* --- live dashboard ----------------------------------------------------------- */

/*
 * `stream` scrolls; this repaints. For watching a value settle -- a load being
 * applied, a charge tapering, an SoC re-anchoring -- a fixed block that updates in
 * place is far easier to read than a river of lines, because the eye can park on one
 * number and see only it change.
 *
 * ANSI cursor-home plus erase-to-end-of-line per row, rather than clear-screen each
 * frame: clearing the whole screen every refresh flickers badly at 2 Hz over a
 * 115200 link. The screen is cleared exactly once, on entry.
 *
 * Everything shown comes from the shared snapshot and the gauge -- no sensor access,
 * so the dashboard never contends with the sampler for the I2C bus.
 */
#define MON_MAX_SECONDS 600

static void mon_bar(char *out, size_t n, uint32_t permille)
{
    /* Ten cells, so each is 10 %. Coarse on purpose: a 40-cell bar invites reading
     * precision out of it that the gauge does not have. */
    const uint32_t filled = (permille + 50) / 100;
    size_t i = 0;
    if (n < 13) {
        if (n) out[0] = '\0';
        return;
    }
    out[i++] = '[';
    for (uint32_t c = 0; c < 10; c++) {
        out[i++] = (c < filled) ? '#' : '-';
    }
    out[i++] = ']';
    out[i]   = '\0';
}

int cmd_mon(int argc, char **argv)
{
    /*
     * This one command genuinely cannot be transport-agnostic: it repaints until a
     * keypress arrives on stdin, and a remote caller's keystrokes are not on stdin.
     * Left unguarded it would repaint into the BLE link for its whole 600 s timeout.
     * Refusing with the alternative named is better than either hanging or silently
     * doing something different depending on the wire.
     */
    if (cli_is_remote()) {
        printf("'mon' is a local-terminal dashboard: it repaints until a key is\n");
        printf("pressed on the console it was started from, and a remote caller has\n");
        printf("no way to send that. Use 'stream csv' for live data over BLE -- it\n");
        printf("carries the same values and needs no terminal.\n");
        return 1;
    }

    uint32_t period_ms = 500;
    if (argc >= 2) {
        const long v = strtol(argv[1], NULL, 10);
        if (v < 100 || v > 5000) {
            printf("refresh period must be 100..5000 ms\n");
            return 1;
        }
        period_ms = (uint32_t)v;
    }

    /* The scrolling stream would fight the repaint for the same screen. */
    const bool was_streaming = config()->stream_enabled;
    config()->stream_enabled    = false;

    /* Non-blocking stdin so a keypress can end the loop without stalling a frame on
     * a read. Restored on the way out -- leaving the console non-blocking would break
     * every command typed afterwards. */
    const int fd    = fileno(stdin);
    const int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    printf("\033[2J\033[?25l"); /* clear once, hide the cursor */

    const int64_t started   = esp_timer_get_time();
    uint32_t      last_n    = values()->n_samples;
    int64_t       last_rate = started;

    char rate_s[16] = "--";

    for (;;) {
        const int64_t now = esp_timer_get_time();

        /* Sample rate over the last frame: the single best indicator that the
         * acquisition path is healthy, and it belongs on a monitoring screen. */
        if (now - last_rate >= 1000000) {
            const uint32_t d = values()->n_samples - last_n;
            const int64_t  us = now - last_rate;
            char rb[16];
            snprintf(rate_s, sizeof(rate_s), "%s",
                     fixed_fmt(rb, sizeof(rb), (int64_t)d * 1000000 * 10 / us, 10, 1));
            last_n    = values()->n_samples;
            last_rate = now;
        }

        fg_status_t fg;
        fg_get(&fg);
        const power_sample_t sm = values()->last;
        const bool valid        = values()->last_valid;

        char b1[24], b2[24], b3[24], bar[16];
        mon_bar(bar, sizeof(bar), fg.soc_permille);

        const uint32_t up = (uint32_t)(now / 1000000);

        printf("\033[H");
        printf("bat-monitor live      up %lu:%02lu:%02lu   %s sa/s\033[K\n",
               (unsigned long)(up / 3600), (unsigned long)((up / 60) % 60),
               (unsigned long)(up % 60), rate_s);
        printf("--------------------------------------------------------------\033[K\n");

        if (!valid) {
            printf("  no valid sample -- see 'scan' and 'sensors'\033[K\n");
        } else {
            printf("  %9s V   %10s A   %9s W\033[K\n",
                   FMT_V(b1, sm.v_pack_uv), FMT_A(b2, sm.i_ua), FMT_W(b3, sm.p_uw));
            printf("  %s %s %%   %s / %s Ah   %s\033[K\n", bar,
                   fixed_fmt(b1, sizeof(b1), fg.soc_permille, 10, 1),
                   fixed_fmt(b2, sizeof(b2), fg.charge_uas / 3600, 1000000, 2),
                   fixed_fmt(b3, sizeof(b3), fg.full_capacity_uah, 1000000, 1),
                   fg_state_str(fg.state));
            printf("  shunt %9s mV  pga %-16s%s\033[K\n",
                   FMT_MV(b1, sm.v_shunt_uv), ina219_pga_str(sm.pga),
                   sm.saturated ? " SATURATED" : "");
            printf("  OCV   %9s V   I*R %7s mV   Peukert x%s\033[K\n",
                   FMT_V(b1, fg.ocv_uv), FMT_MV(b2, fg.ir_drop_uv),
                   fixed_fmt(b3, sizeof(b3), fg.peukert_factor_q16, 65536, 3));
            if (values()->env_valid) {
                printf("  env   %8s C   %8s hPa%s%s\033[K\n",
                       fixed_fmt(b1, sizeof(b1), values()->env.temp_centi_c, 100, 2),
                       fixed_fmt(b2, sizeof(b2), (int64_t)values()->env.press_pa, 100, 2),
                       values()->env.have_humidity ? "   " : "",
                       values()->env.have_humidity
                           ? fixed_fmt(b3, sizeof(b3), values()->env.humid_centi, 100, 1)
                           : "");
            } else {
                printf("  env   no sensor\033[K\n");
            }
        }

        printf("--------------------------------------------------------------\033[K\n");
        printf("  win n=%-6lu mean %10s A  sd %9s A\033[K\n",
               (unsigned long)history_window()->n,
               FMT_A(b1, stats_mean_ua(history_window())),
               FMT_A(b2, stats_stddev_ua(history_window())));
        printf("  err   bus %-4lu notready %-5lu range %-4lu unresolved %lu\033[K\n",
               (unsigned long)values()->err_bus,
               (unsigned long)values()->err_not_finished,
               (unsigned long)values()->err_range_discard,
               (unsigned long)values()->err_unresolved);
        printf("  gauge in %8s Ah  out %8s Ah  anchor %s\033[K\n",
               fixed_fmt(b1, sizeof(b1), fg.cum_in_uas / 3600, 1000000, 3),
               fixed_fmt(b2, sizeof(b2), fg.cum_out_uas / 3600, 1000000, 3),
               fg.s_since_anchor == UINT32_MAX
                   ? "never"
                   : fixed_fmt(b3, sizeof(b3), fg.s_since_anchor, 1, 0));
        printf("  sens  0x%02X %-3s 0x%02X %-3s mode %-8s%s\033[K\n",
               CONFIG_BATMON_ADDR_POS_POLE,
               (cli_ctx->sensors && sensors_have_pos(cli_ctx->sensors)) ? "ok" : "--",
               CONFIG_BATMON_ADDR_NEG_POLE,
               (cli_ctx->sensors && sensors_have_neg(cli_ctx->sensors)) ? "ok" : "--",
               cli_ctx->sensors ? sensors_mode_str(sensors_get_mode(cli_ctx->sensors))
                              : "none",
               fg.voltage_only ? "  SoC from voltage only" : "");

#if CONFIG_BATMON_BLE_ENABLE
        {
            ble_stats_t bs;
            ble_get_stats(&bs);
            printf("  link  BLE %d/%d conn %d sub  pair %-8s disp %s\033[K\n",
                   bs.connections, BLE_MAX_CONNS, bs.subscribers,
                   bs.mode == BLE_SEC_BONDED ? "required" : "OPEN",
                   lcd_present()
                       ? (lcd_enabled() ? "on" : "off") : "none");
        }
#else
        printf("\033[K\n");
#endif
        printf("--------------------------------------------------------------\033[K\n");
        printf("  any key to exit\033[K\n");
        fflush(stdout);

        /* Exit on any input. EOF simply means nothing was typed this frame. */
        const int c = fgetc(stdin);
        if (c != EOF) {
            break;
        }
        if ((now - started) > (int64_t)MON_MAX_SECONDS * 1000000) {
            printf("\n(monitor timed out after %d s)\n", MON_MAX_SECONDS);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(period_ms));
    }

    printf("\033[?25h\n"); /* cursor back, and leave the frame on screen */
    fcntl(fd, F_SETFL, flags);
    config()->stream_enabled = was_streaming;
    return 0;
}
