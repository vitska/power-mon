/*
 * cli_report.c -- the board's settings, its radio, its panel and its firmware.
 *
 * `config` and `options` are the same state twice over: one for programs, one for
 * people, and they must not disagree. Alongside them the things that are configured
 * rather than measured -- the OLED, BLE pairing, and the OTA slots.
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
#include "ble.h"
#include "bme280.h"
#include "driver/usb_serial_jtag.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "fixed_fmt.h"
#include "freertos/FreeRTOS.h"
#include "fuelgauge.h"
#include "history_values.h"
#include "ina219.h"
#include "lcd.h"
#include "ota.h"
#include "sdkconfig.h"
#include "sensors.h"
#include "values.h"
#include "cli_internal.h"

/* --- display (DESIGN.md 9.11) ------------------------------------------------- */

int cmd_disp(int argc, char **argv)
{
    if (!lcd_present()) {
        printf("no display fitted (nothing answered at 0x%02X). 'scan' will confirm.\n",
               CONFIG_BATMON_DISPLAY_ADDR);
        return 0;
    }

    if (argc < 2) {
        const int pinned = lcd_get_screen();
        printf("display: %s, ", lcd_enabled() ? "on" : "off");
        if (pinned < 0) {
            printf("auto-cycling %d screens\n", lcd_n_screens());
        } else {
            printf("pinned to screen %d of %d\n", pinned, lcd_n_screens());
        }
        printf("  disp <on|off>              blank or restore the panel\n");
        printf("  disp screen <n|auto>       pin a screen, or resume cycling\n");
        printf("  disp contrast <1-255>      0x40 is the default and is deliberate\n");
        return 0;
    }

    if (strcmp(argv[1], "on") == 0 || strcmp(argv[1], "off") == 0) {
        const bool on = (argv[1][1] == 'n');
        lcd_enable(on);
        printf("display %s\n", on ? "on" : "off");
        return 0;
    }

    if (strcmp(argv[1], "screen") == 0) {
        if (argc < 3) {
            printf("usage: disp screen <n|auto>\n");
            return 1;
        }
        if (strcmp(argv[2], "auto") == 0) {
            lcd_set_screen(-1);
            printf("auto-cycling\n");
            return 0;
        }
        const int n = atoi(argv[2]);
        if (n < 0 || n >= lcd_n_screens()) {
            printf("screen must be 0..%d, or 'auto'\n",
                   lcd_n_screens() - 1);
            return 1;
        }
        lcd_set_screen(n);
        printf("pinned to screen %d\n", n);
        return 0;
    }

    if (strcmp(argv[1], "contrast") == 0) {
        if (argc < 3) {
            printf("usage: disp contrast <1-255>\n");
            return 1;
        }
        const int c = atoi(argv[2]);
        if (c < 1 || c > 255) {
            printf("contrast must be 1..255\n");
            return 1;
        }
        const esp_err_t err = lcd_set_contrast((uint8_t)c);
        if (err != ESP_OK) {
            printf("failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        printf("contrast 0x%02X\n", c);
        if (c > 0x80) {
            printf("  note: high contrast costs 2-3x the current and is usually\n");
            printf("  harder to read indoors (DESIGN.md 9.10).\n");
        }
        return 0;
    }

    printf("unknown: %s\n", argv[1]);
    return 1;
}

/* --- BLE console (DESIGN.md 7.3) ---------------------------------------------- */

#if CONFIG_BATMON_BLE_ENABLE
static void ble_show(void)
{
    ble_stats_t st;
    ble_get_stats(&st);

    printf("name        %s\n", ble_name());
    printf("state       %d of %d connected, %d subscribed%s\n", st.connections,
           BLE_MAX_CONNS, st.subscribers,
           st.advertising ? ", advertising" : "");
    if (st.connections > 0) {
        printf("MTU         %u  (%u bytes per notification, smallest link)\n", st.mtu,
               st.mtu > 3 ? st.mtu - 3 : 0);
        /* The security flags describe the WEAKEST link, so one unencrypted client
         * cannot hide behind two encrypted ones. */
        printf("security    %s, %s  (weakest link)\n",
               st.encrypted ? "all encrypted" : "NOT all encrypted",
               st.authenticated ? "all authenticated" : "not all authenticated");
    }

    printf("pairing     %s\n",
           ble_get_sec_mode() == BLE_SEC_BONDED
               ? "bonded -- pairing required"
               : "OPEN -- no pairing, anyone in range can run commands");
    const uint32_t pk = ble_get_passkey();
    printf("passkey     %s\n", pk == 0xFFFFFFFFu ? "random each pairing"
                                                 : "fixed");
    if (pk != 0xFFFFFFFFu) {
        printf("            %06lu -- a fixed passkey is not a secret\n",
               (unsigned long)pk);
    }
    printf("bonds       %d stored\n", st.bonds);
    printf("traffic     rx %lu B, tx %lu B, %lu lines\n",
           (unsigned long)st.rx_bytes, (unsigned long)st.tx_bytes,
           (unsigned long)st.lines);
    if (st.dropped) {
        printf("dropped     %lu B of output with no subscriber\n",
               (unsigned long)st.dropped);
    }
    if (st.rejected) {
        printf("rejected    %lu writes for insufficient security\n",
               (unsigned long)st.rejected);
    }
    if (st.connections > st.subscribers) {
        printf("%d connected but NOT subscribed: their command output goes nowhere\n",
               st.connections - st.subscribers);
        printf("until they enable notifications on TX (6E400003-...).\n");
    }
    printf("service     Nordic UART, 6E400001-B5A3-F393-E0A9-E50E24DCCA9E\n");
}

int cmd_ble(int argc, char **argv)
{
    if (argc < 2) {
        ble_show();
        printf("\n");
        printf("  ble pair <open|bonded>     require pairing, or not\n");
        printf("  ble passkey <random|NNNNNN> six digits, or a fresh one each time\n");
        printf("  ble bonds                  list bonded peers\n");
        printf("  ble unpair [all]           forget bonds\n");
        printf("  ble disconnect             drop every current link\n");
        return 0;
    }

    if (strcmp(argv[1], "pair") == 0) {
        if (argc < 3) {
            printf("usage: ble pair <open|bonded>\n");
            return 1;
        }
        ble_sec_mode_t m;
        if (strcmp(argv[2], "open") == 0) {
            m = BLE_SEC_OPEN;
        } else if (strcmp(argv[2], "bonded") == 0) {
            m = BLE_SEC_BONDED;
        } else {
            printf("mode must be 'open' or 'bonded'\n");
            return 1;
        }

        const esp_err_t err = ble_set_sec_mode(m);
        if (err != ESP_OK) {
            printf("failed: %s\n", esp_err_to_name(err));
            return 1;
        }

        if (m == BLE_SEC_BONDED) {
            printf("pairing required. Any open link was dropped so the peer comes\n");
            printf("back through the new rules. The passkey appears on the OLED and\n");
            printf("in this log when a phone asks to pair.\n");
        } else {
            printf("PAIRING DISABLED. Anything in radio range can now run every\n");
            printf("command, including 'zero' and 'curve' -- which can silently\n");
            printf("corrupt the gauge. Bench only (DESIGN.md 8.5).\n");
        }
        printf("Stored, and it survives a reboot.\n");
        return 0;
    }

    if (strcmp(argv[1], "passkey") == 0) {
        if (argc < 3) {
            printf("usage: ble passkey <random|NNNNNN>\n");
            return 1;
        }
        uint32_t pk;
        if (strcmp(argv[2], "random") == 0) {
            pk = 0xFFFFFFFFu;
        } else {
            const long v = strtol(argv[2], NULL, 10);
            if (v < 0 || v > 999999) {
                printf("passkey must be 0..999999, or 'random'\n");
                return 1;
            }
            pk = (uint32_t)v;
        }
        const esp_err_t err = ble_set_passkey(pk);
        if (err != ESP_OK) {
            printf("failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        if (pk == 0xFFFFFFFFu) {
            printf("a fresh random passkey per pairing -- the safer choice\n");
        } else {
            printf("fixed passkey %06lu. Note this is written to flash and is\n",
                   (unsigned long)pk);
            printf("recoverable from the device; treat it as a convenience, not a\n");
            printf("secret. 'ble passkey random' is better wherever it is usable.\n");
        }
        return 0;
    }

    if (strcmp(argv[1], "bonds") == 0) {
        char list[8][24];
        const int n = ble_list_bonds(list, 8);
        if (n < 0) {
            printf("could not read the bond store\n");
            return 1;
        }
        if (n == 0) {
            printf("no bonds. In bonded mode the next phone to pair creates one.\n");
            return 0;
        }
        printf("%d bonded peer%s:\n", n, n == 1 ? "" : "s");
        for (int i = 0; i < n; i++) {
            printf("  %s\n", list[i]);
        }
        return 0;
    }

    if (strcmp(argv[1], "unpair") == 0) {
        /* No magic constant here, unlike the destructive commands of DESIGN.md 8.4:
         * forgetting a bond costs one re-pairing, not a year of accumulated charge.
         * Guarding it would be security theatre. */
        const esp_err_t err = ble_clear_bonds();
        if (err != ESP_OK) {
            printf("failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        printf("all bonds forgotten; the link was dropped. Peers must pair again.\n");
        return 0;
    }

    if (strcmp(argv[1], "disconnect") == 0) {
        const esp_err_t err = ble_disconnect();
        if (err == ESP_ERR_INVALID_STATE) {
            printf("nothing connected\n");
            return 0;
        }
        if (err != ESP_OK) {
            printf("failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        printf("all links dropped; advertising resumes\n");
        return 0;
    }

    printf("unknown: %s\n", argv[1]);
    return 1;
}

/* --- options overview --------------------------------------------------------- */

/*
 * One place to see everything that is set, with the command that owns each group.
 * Deliberately a VIEW, not a config store: nothing here holds state of its own, so
 * it cannot disagree with the thing it reports. The persistent configuration layer
 * is M3's job (DESIGN.md 6.1, 8.3), and until then the closing line says so rather
 * than letting anyone assume these survive a reboot.
 */
/*
 * `config` -- every setting as key=value, machine first.
 *
 * `options` says the same things in prose for a person to read. This says them for a
 * program: one setting per line, no prose, no alignment to match on. Values are in the
 * SAME units the corresponding setter takes, so what this prints is what you would type
 * to reproduce it -- `soc.cap_uah=44000000` came from `soc cap 44000000`, and enums are
 * the exact keyword the setter accepts. That round-trip property is the whole point: a
 * client can show current values and write new ones without a table mapping one
 * spelling to the other.
 *
 * Keys are namespaced by the command that owns them. A client should skip keys it does
 * not recognise, exactly as it skips unknown telemetry records -- new settings will
 * appear here without a protocol bump, and only a change to an existing key's meaning
 * is a breaking change (CLI.md 8.1).
 */
int cmd_config(int argc, char **argv)
{
    (void)argc; (void)argv;

    printf("protocol=%d\n", BATMON_CLI_PROTOCOL);
    printf("firmware=%s\n", BATMON_FW_VERSION);

    printf("stream.on=%d\n", config()->stream_enabled ? 1 : 0);
    printf("stream.csv=%d\n", config()->stream_csv ? 1 : 0);
    printf("stream.fast_ms=%lu\n", (unsigned long)config()->rate_fast_ms);
    printf("stream.calc_ms=%lu\n", (unsigned long)config()->rate_calc_ms);
    printf("stream.diag_ms=%lu\n", (unsigned long)config()->rate_diag_ms);
    printf("stream.env_ms=%lu\n", (unsigned long)config()->rate_env_ms);

    ina219_handle_t cd = cli_ctx->sensors ? sensors_current_dev(cli_ctx->sensors) : NULL;
    ina219_handle_t vd = cli_ctx->sensors ? sensors_voltage_dev(cli_ctx->sensors) : NULL;

    if (cli_ctx->sensors) {
        /* sensors_mode_str() is prose for a person ("N: shunt in negative lead").
         * This command's contract is that a value is what the setter takes, so
         * emit the keyword `shunt loc` accepts and nothing else. */
        const sensors_mode_t m = sensors_get_mode(cli_ctx->sensors);
        printf("shunt.loc=%s\n",
               m == SENSORS_MODE_P      ? "p" :
               m == SENSORS_MODE_N      ? "n" :
               m == SENSORS_MODE_SINGLE ? "single" : "auto");
        printf("shunt.roles=%s\n",
               sensors_get_role_state(cli_ctx->sensors) == SENSORS_ROLE_RESOLVED
                   ? "resolved" : "unresolved");
        printf("sensors.pos=%d\n", sensors_have_pos(cli_ctx->sensors) ? 1 : 0);
        printf("sensors.neg=%d\n", sensors_have_neg(cli_ctx->sensors) ? 1 : 0);
    }

    if (cd && vd) {
        printf("profile=%s\n",
               ina219_get_profile(cd) == INA219_PROFILE_TRIGGERED ? "triggered"
               : (ina219_get_continuous_adc(cd) == INA219_ADC_128AVG ? "continuous"
                                                                     : "fast"));
        printf("profile.pair_us=%lu\n", (unsigned long)ina219_conversion_time_us(cd));
        printf("shunt.uohm=%lu\n", (unsigned long)ina219_get_shunt_uohm(cd));
        printf("cal.i_offset_ua=%ld\n", (long)ina219_get_offset_ua(cd));
        printf("cal.i_gain_ppm=%lu\n", (unsigned long)ina219_get_gain_ppm(cd));
        printf("cal.v_offset_uv=%ld\n", (long)ina219_get_vbus_offset_uv(vd));
        printf("cal.v_gain_ppm=%lu\n", (unsigned long)ina219_get_vbus_gain_ppm(vd));
        printf("cal.v_divider_q16=%lu\n",
               (unsigned long)ina219_get_vbus_divider_q16(vd));
        printf("cal.stored=%d\n", config_exists() ? 1 : 0);
        printf("sense.sign=%s\n", ina219_get_invert_sign(cd) ? "invert" : "normal");
        printf("sense.vbuscomp=%s\n",
               ina219_get_vbus_comp(vd) == INA219_VBUS_COMP_NONE ? "none" :
               ina219_get_vbus_comp(vd) == INA219_VBUS_COMP_ADD_SHUNT ? "add" : "sub");
        /* The enum is an index, the setter takes the divisor: print the divisor. */
        printf("sense.pgamax=%d\n", 1 << (int)ina219_get_pga_max(cd));
        printf("sense.pga=%d\n", 1 << (int)ina219_get_pga(cd));
        printf("sense.autorange=%d\n", ina219_get_autorange(cd) ? 1 : 0);
    }

    {
        fg_config_t c = fg_get_config();
        fg_status_t st;
        fg_get(&st);
        const fg_chem_profile_t *chem = fg_chem_profile((fg_chem_t)c.chemistry);
        printf("battery.chem=%s\n", chem ? chem->key : "unknown");
        printf("battery.cells=%u\n", (unsigned)c.cells);
        printf("soc.cap_uah=%lu\n", (unsigned long)c.design_capacity_uah);
        printf("soc.learned_uah=%lu\n", (unsigned long)st.full_capacity_uah);
        /* 0 means the learned figure above is just the nameplate copied, not a
         * measurement. Without this a client has to guess from the two being equal,
         * which is also what a pack that measured exactly its nameplate looks like. */
        printf("soc.learn_count=%lu\n", (unsigned long)st.learn_count);
        printf("soc.last_learn_uah=%lu\n", (unsigned long)st.last_learn_uah);
        /* The OPEN span, so a client can say when the next measurement will happen
         * rather than only that none has. have_ref 0 means no span is open at all:
         * the next full, empty or settled rest starts one. */
        printf("soc.have_ref=%d\n", st.have_ref ? 1 : 0);
        printf("soc.ref_permille=%lu\n", (unsigned long)st.ref_soc_permille);
        /*
         * Charge drawn since the reference, in uAh: positive means out of the pack,
         * which is the direction that can be learned from.
         *
         * 32-bit, NOT %lld. CONFIG_NEWLIB_NANO_FORMAT drops %ll support, so that
         * conversion printed nothing usable -- which is how this reported a span of
         * 0 Ah on a pack that had drawn several. Micro-amp-hours fit an int32 to
         * 2147 Ah. The console's own `soc` was right throughout, because fixed_fmt()
         * takes the int64 and formats it itself rather than handing it to printf.
         */
        printf("soc.span_uah=%ld\n", (long)(-st.q_since_ref_uas / 3600));
        printf("soc.v0_uv=%lu\n", (unsigned long)c.v_0pct_uv);
        printf("soc.v100_uv=%lu\n", (unsigned long)c.v_100pct_uv);
        printf("soc.vfull_uv=%lu\n", (unsigned long)c.v_full_uv);
        printf("soc.rint_uohm=%lu\n", (unsigned long)c.r_int_uohm);
        printf("soc.taper_ua=%lu\n", (unsigned long)c.i_taper_ua);
        printf("soc.taper_eff_ua=%lu\n", (unsigned long)st.taper_current_ua);
        printf("soc.rest_s=%lu\n", (unsigned long)c.t_rest_s);
        printf("soc.irest_permille=%u\n", (unsigned)c.i_rest_permille);
        printf("soc.rest_ua=%lu\n", (unsigned long)st.rest_current_ua);
        printf("soc.peukert_q8=%u\n", (unsigned)c.peukert_q8);
        printf("soc.irated_ua=%lu\n", (unsigned long)c.i_rated_ua);
        printf("soc.depth_permille=%u\n", (unsigned)c.learn_min_depth_permille);
        printf("soc.deadband_ua=%lu\n", (unsigned long)c.i_deadband_ua);
        printf("soc.permille=%lu\n", (unsigned long)st.soc_permille);
        printf("soc.state=%s\n", fg_state_str(st.state));
        printf("soc.voltage_only=%d\n", st.voltage_only ? 1 : 0);
        printf("hist.period_s=%lu\n", (unsigned long)soc_history_period_s());
        printf("hist.points=%d\n", SOC_HIST_POINTS);
    }

#if CONFIG_BATMON_DISPLAY_ENABLE
    printf("disp.present=%d\n", lcd_present() ? 1 : 0);
    if (lcd_present()) {
        const int pinned = lcd_get_screen();
        printf("disp.on=%d\n", lcd_enabled() ? 1 : 0);
        if (pinned < 0) {
            printf("disp.screen=auto\n");
        } else {
            printf("disp.screen=%d\n", pinned);
        }
        printf("disp.screens=%d\n", lcd_n_screens());
        printf("disp.contrast=%u\n", (unsigned)lcd_get_contrast());
    }
#else
    printf("disp.present=0\n");
#endif

#if CONFIG_BATMON_BLE_ENABLE
    {
        ble_stats_t bst;
        ble_get_stats(&bst);
        const uint32_t pk = ble_get_passkey();
        printf("ble.name=%s\n", ble_name());
        printf("ble.pair=%s\n", bst.mode == BLE_SEC_BONDED ? "bonded" : "open");
        if (pk == 0xFFFFFFFFu) {
            printf("ble.passkey=random\n");
        } else {
            printf("ble.passkey=%06lu\n", (unsigned long)pk);
        }
        printf("ble.conns=%d\n", bst.connections);
        printf("ble.subs=%d\n", bst.subscribers);
        printf("ble.bonds=%d\n", bst.bonds);
    }
#endif

    if (cli_ctx->bme) {
        printf("env.sensor=%s\n", bme280_chip_str(bme280_chip(cli_ctx->bme)));
    } else {
        printf("env.sensor=none\n");
    }
    return 0;
}

/* --- firmware update ----------------------------------------------------------- */

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_sha256(const char *hex, uint8_t out[32])
{
    if (strlen(hex) != 64) {
        return false;
    }
    for (int i = 0; i < 32; i++) {
        const int hi = hexval(hex[2 * i]), lo = hexval(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return true;
}

/* key=value, like `config`: this is what the phone app reads to decide what to offer. */
static void ota_print_status(void)
{
    ota_status_t st;
    ota_get_status(&st);
    printf("running=%s\n", st.running);
    printf("version=%s\n", st.version);
    printf("build=%s\n", st.build);
    printf("state=%s\n", st.pending ? "probation" : "valid");
    if (st.pending && st.probation_left_s >= 0) {
        printf("probation_s=%d\n", st.probation_left_s);
    }
    printf("rollback=%d\n", st.can_rollback ? 1 : 0);
    printf("boot=%s\n", st.boot);
    printf("spare=%s\n", st.spare);
    printf("spare.version=%s\n", st.spare_version[0] ? st.spare_version : "none");
    printf("session=%s\n", st.phase == OTA_RECEIVING ? "receiving" : "idle");
    if (st.phase == OTA_RECEIVING) {
        printf("session.received=%lu\n", (unsigned long)st.received);
        printf("session.size=%lu\n", (unsigned long)st.size);
    }
}

int cmd_ota(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "status") == 0) {
        ota_print_status();
        return 0;
    }

    if (strcmp(argv[1], "begin") == 0) {
        uint8_t sha[32];
        char   *end  = NULL;
        const unsigned long size = argc >= 4 ? strtoul(argv[2], &end, 10) : 0;
        if (argc < 4 || !end || *end != '\0' || size == 0 || !parse_sha256(argv[3], sha)) {
            printf("usage: ota begin <bytes> <sha256 as 64 hex digits>\n");
            return 1;
        }
        const esp_err_t err = ota_begin((uint32_t)size, sha);
        switch (err) {
        case ESP_OK: {
            ota_status_t st;
            ota_get_status(&st);
            printf("%s erased; send %lu bytes to the OTA characteristic\n", st.spare, size);
            return 0;
        }
        case ESP_ERR_OTA_ROLLBACK_INVALID_STATE:
            printf("the running image is on probation -- 'ota confirm' or 'ota rollback'\n"
                   "first; overwriting the only known-good image is what probation prevents\n");
            break;
        case ESP_ERR_NOT_FOUND:
            printf("no spare app slot -- this board still has the single-slot partition\n"
                   "table. Flash once over USB (tools/flash.ps1) to get BLE updates.\n");
            break;
        case ESP_ERR_INVALID_SIZE:
            printf("%lu bytes does not fit the spare slot\n", size);
            break;
        default:
            printf("cannot start: %s\n", esp_err_to_name(err));
            break;
        }
        return 1;
    }

    if (strcmp(argv[1], "end") == 0) {
        char why[96];
        if (ota_finish(why, sizeof(why)) != ESP_OK) {
            printf("%s\n", why);
            return 1;
        }
        printf("firmware %s written; 'reboot' to start it\n", why);
        return 0;
    }

    if (strcmp(argv[1], "abort") == 0) {
        ota_abort();
        printf("abandoned; the running image is untouched\n");
        return 0;
    }

    if (strcmp(argv[1], "confirm") == 0) {
        ota_status_t st;
        ota_get_status(&st);
        if (!st.pending) {
            printf("not on probation; nothing to confirm\n");
            return 0;
        }
        const esp_err_t err = ota_confirm();
        if (err != ESP_OK) {
            printf("confirm failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        printf("firmware %s confirmed; it stays\n", st.version);
        return 0;
    }

    if (strcmp(argv[1], "rollback") == 0) {
        if (ota_rollback() != ESP_OK) {
            printf("nothing to roll back to -- the other slot has no valid image\n");
            return 1;
        }
        printf("rolling back to the previous image; the link will drop\n");
        return 0;
    }

    printf("usage: ota [status | begin <bytes> <sha256> | end | abort | confirm | rollback]\n");
    return 1;
}

int cmd_reboot(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("rebooting; the link will drop\n");
    ota_reboot_after(500);
    return 0;
}

int cmd_options(int argc, char **argv)
{
    (void)argc; (void)argv;

    char b1[24], b2[24];

    printf("== monitoring ============================ (stream, stats, profile)\n");
    printf("stream        %s, %s   fast %lu / calc %lu / diag %lu / env %lu ms\n",
           config()->stream_enabled ? "on" : "off", config()->stream_csv ? "CSV" : "text",
           (unsigned long)config()->rate_fast_ms, (unsigned long)config()->rate_calc_ms,
           (unsigned long)config()->rate_diag_ms, (unsigned long)config()->rate_env_ms);
    printf("samples       %lu taken, window n=%lu\n",
           (unsigned long)values()->n_samples, (unsigned long)history_window()->n);
    printf("history       a point every %lu s, %d points = %lu h span   (hist every <s>)\n",
           (unsigned long)soc_history_period_s(), SOC_HIST_POINTS,
           (unsigned long)soc_history_period_s() * SOC_HIST_POINTS / 3600);
    printf("errors        bus %lu, not-ready %lu, range %lu, unresolved %lu\n",
           (unsigned long)values()->err_bus, (unsigned long)values()->err_not_finished,
           (unsigned long)values()->err_range_discard,
           (unsigned long)values()->err_unresolved);

    printf("== shunt and sensors ===================== (shunt, sensors, detect)\n");
    if (cli_ctx->sensors) {
        printf("location      %s\n",
               sensors_mode_str(sensors_get_mode(cli_ctx->sensors)));
        printf("roles         %s\n",
               sensors_get_role_state(cli_ctx->sensors) == SENSORS_ROLE_RESOLVED
                   ? "resolved" : "UNRESOLVED -- not integrating");
        printf("sensors       0x%02X %s, 0x%02X %s\n", CONFIG_BATMON_ADDR_POS_POLE,
               sensors_have_pos(cli_ctx->sensors) ? "present" : "absent",
               CONFIG_BATMON_ADDR_NEG_POLE,
               sensors_have_neg(cli_ctx->sensors) ? "present" : "absent");
    } else {
        printf("sensors       NONE -- bring-up failed; try 'scan'\n");
    }

    ina219_handle_t cd = cli_ctx->sensors ? sensors_current_dev(cli_ctx->sensors) : NULL;
    ina219_handle_t vd = cli_ctx->sensors ? sensors_voltage_dev(cli_ctx->sensors) : NULL;

    printf("== calibration =========================== (zero, curve, sense)\n");
    if (cd && vd) {
        printf("shunt         %lu uOhm\n", (unsigned long)ina219_get_shunt_uohm(cd));
        printf("current       offset %s A, gain %lu ppm, sign %s\n",
               FMT_A(b1, ina219_get_offset_ua(cd)),
               (unsigned long)ina219_get_gain_ppm(cd),
               ina219_get_invert_sign(cd) ? "inverted" : "normal");
        printf("voltage       offset %s V, gain %lu ppm, divider x%s\n",
               FMT_V(b2, ina219_get_vbus_offset_uv(vd)),
               (unsigned long)ina219_get_vbus_gain_ppm(vd),
               fixed_fmt(b1, sizeof(b1), ina219_get_vbus_divider_q16(vd), 65536, 4));
        printf("range         %s ceiling, autorange %s\n",
               ina219_pga_str(ina219_get_pga_max(cd)),
               ina219_get_autorange(cd) ? "on" : "off");
        printf("buscomp       %s\n",
               ina219_get_vbus_comp(vd) == INA219_VBUS_COMP_NONE ? "off" :
               ina219_get_vbus_comp(vd) == INA219_VBUS_COMP_ADD_SHUNT
                   ? "add shunt" : "subtract shunt");
    } else {
        printf("unavailable   roles unresolved; set 'shunt loc <p|n|single>'\n");
    }

#if CONFIG_BATMON_DISPLAY_ENABLE
    printf("== display =============================== (disp)\n");
    if (lcd_present()) {
        const int pinned = lcd_get_screen();
        printf("panel         0x%02X, %s\n", CONFIG_BATMON_DISPLAY_ADDR,
               lcd_enabled() ? "on" : "blanked");
        if (pinned < 0) {
            printf("screens       auto-cycling %d\n", lcd_n_screens());
        } else {
            printf("screens       pinned to %d\n", pinned);
        }
    } else {
        printf("panel         none fitted\n");
    }
#endif

    printf("== environment =========================== (env)\n");
    if (cli_ctx->bme) {
        char be[24];
        printf("sensor        %s at 0x%02X\n",
               bme280_chip_str(bme280_chip(cli_ctx->bme)), bme280_addr(cli_ctx->bme));
        if (values()->env_valid) {
            printf("last reading  %s C (cached, refreshed every 60 s)\n",
                   fixed_fmt(be, sizeof(be), values()->env.temp_centi_c, 100, 2));
        } else {
            printf("last reading  none yet\n");
        }
    } else {
        printf("sensor        none fitted\n");
    }

    printf("== fuel gauge ============================ (soc)\n");
    {
        fg_status_t st;
        fg_config_t c = fg_get_config();
        char        b1[24], b2[24];
        fg_get(&st);
        printf("SoC           %s %%%s, %s\n",
               fixed_fmt(b1, sizeof(b1), st.soc_permille, 10, 1),
               st.voltage_only ? " (voltage only)" : "", fg_state_str(st.state));
        printf("capacity      %s Ah, window %s",
               fixed_fmt(b1, sizeof(b1), st.full_capacity_uah, 1000000, 1),
               FMT_V(b2, c.v_0pct_uv));
        printf(" - %s V\n", FMT_V(b1, c.v_100pct_uv));
    }

#if CONFIG_BATMON_BLE_ENABLE
    printf("== BLE =================================== (ble)\n");
    ble_stats_t bst;
    ble_get_stats(&bst);
    printf("name          %s\n", ble_name());
    printf("state         %d/%d connected, %d subscribed%s\n", bst.connections,
           BLE_MAX_CONNS, bst.subscribers,
           bst.advertising ? ", advertising" : "");
    printf("pairing       %s, %d bond%s\n",
           bst.mode == BLE_SEC_BONDED ? "required" : "OPEN -- anyone can connect",
           bst.bonds, bst.bonds == 1 ? "" : "s");
#endif

    printf("==========================================================\n");
    printf("calibration   %s\n", config_exists() ? "stored in flash"
                                                    : "NOT stored -- 'cal save'");
    printf("Calibration and BLE pairing mode persist. Everything else here is RAM\n");
    printf("only until the full config layer lands in M3 (DESIGN.md 6.1, 8.3).\n");
    return 0;
}

#endif
