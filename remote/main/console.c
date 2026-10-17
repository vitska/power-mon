/*
 * console.c — see console.h.
 *
 * The serial side reads the console UART through its driver, byte by byte, and echoes
 * what is typed: the remote's USB port is a USB-serial bridge on UART0, and a terminal
 * on it shows nothing of the typing otherwise. Output goes out through stdout, the
 * same path as the log, so a reply and a log line never tear each other mid-line any
 * worse than two log lines would.
 */

#include "console.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "board.h"
#include "link.h"
#include "ota.h"
#include "rcon.h"
#include "ui.h"

#define CON_LINE_MAX  160
#define CON_ARGS_MAX  12

/* --- ver, ota, reboot: the commands the phone app speaks (CLI.md §6) ---------------- */

static bool parse_sha256(const char *hex, uint8_t out_sha[32])
{
    if (strlen(hex) != 64) return false;
    for (int i = 0; i < 32; i++) {
        unsigned v;
        if (sscanf(hex + 2 * i, "%2x", &v) != 1) return false;
        out_sha[i] = (uint8_t)v;
    }
    return true;
}

static int cmd_ver(console_out_t out)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    char build[17];
    esp_app_get_elf_sha256(build, sizeof(build));
    /* protocol 3: the same framing and OTA commands as the monitor, which is all the
     * app needs from us. `device remote` says which of the two it is talking to, and
     * `board` which image fits it: the app picks the release asset by it. */
    out("protocol 3\n");
    out("firmware %s\n", esp_app_get_description()->version);
    out("build %s\n", build);
    out("device remote\n");
    out("board %s\n", BOARD_NAME);
    out("chip %s\n", CONFIG_IDF_TARGET);
    out("mac %02X:%02X:%02X:%02X:%02X:%02X\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return 0;
}

static int cmd_ota(int argc, char **argv, console_out_t out)
{
    if (argc < 2 || strcmp(argv[1], "status") == 0) {
        ota_status_t st;
        ota_get_status(&st);
        out("running=%s\n", st.running);
        out("version=%s\n", st.version);
        out("build=%s\n", st.build);
        out("state=%s\n", st.pending ? "probation" : "valid");
        if (st.pending && st.probation_left_s >= 0) out("probation_s=%d\n", st.probation_left_s);
        out("rollback=%d\n", st.can_rollback ? 1 : 0);
        out("boot=%s\n", st.boot);
        out("spare=%s\n", st.spare);
        out("spare.version=%s\n", st.spare_version[0] ? st.spare_version : "none");
        out("session=%s\n", st.phase == OTA_RECEIVING ? "receiving" : "idle");
        if (st.phase == OTA_RECEIVING) {
            out("session.received=%lu\n", (unsigned long)st.received);
            out("session.size=%lu\n", (unsigned long)st.size);
        }
        return 0;
    }
    if (strcmp(argv[1], "begin") == 0) {
        uint8_t       sha[32];
        char         *end  = NULL;
        unsigned long size = argc >= 4 ? strtoul(argv[2], &end, 10) : 0;
        if (argc < 4 || !end || *end || !size || !parse_sha256(argv[3], sha)) {
            out("usage: ota begin <bytes> <sha256 as 64 hex digits>\n");
            return 1;
        }
        const esp_err_t err = ota_begin((uint32_t)size, sha);
        if (err == ESP_OK) {
            out("spare slot erased; send %lu bytes to the OTA characteristic\n", size);
            return 0;
        }
        if (err == ESP_ERR_OTA_ROLLBACK_INVALID_STATE) {
            out("the running image is on probation -- 'ota confirm' or 'ota rollback' first\n");
        } else if (err == ESP_ERR_NOT_FOUND) {
            out("no spare app slot -- flash this remote once over USB (flash.ps1 -Remote)\n");
        } else if (err == ESP_ERR_INVALID_SIZE) {
            out("%lu bytes does not fit the spare slot\n", size);
        } else {
            out("cannot start: %s\n", esp_err_to_name(err));
        }
        return 1;
    }
    if (strcmp(argv[1], "end") == 0) {
        char why[96];
        if (ota_finish(why, sizeof(why)) != ESP_OK) {
            out("%s\n", why);
            return 1;
        }
        out("firmware %s written; 'reboot' to start it\n", why);
        return 0;
    }
    if (strcmp(argv[1], "abort") == 0) {
        ota_abort();
        out("abandoned; the running image is untouched\n");
        return 0;
    }
    if (strcmp(argv[1], "confirm") == 0) {
        ota_status_t st;
        ota_get_status(&st);
        if (!st.pending) {
            out("not on probation; nothing to confirm\n");
            return 0;
        }
        if (ota_confirm() != ESP_OK) {
            out("confirm failed\n");
            return 1;
        }
        out("firmware %s confirmed; it stays\n", st.version);
        return 0;
    }
    if (strcmp(argv[1], "rollback") == 0) {
        if (ota_rollback() != ESP_OK) {
            out("nothing to roll back to -- the other slot has no valid image\n");
            return 1;
        }
        out("rolling back to the previous image; the link will drop\n");
        return 0;
    }
    out("usage: ota [status | begin <bytes> <sha256> | end | abort | confirm | rollback]\n");
    return 1;
}

/* --- the display's own settings: what the touch screens do ---------------------------- */

static const char *state_name(link_state_t st)
{
    switch (st) {
    case LINK_SCANNING:   return "searching";
    case LINK_CONNECTING: return "connecting";
    case LINK_SETUP:      return "setting up";
    case LINK_PAIRING:    return "pairing";
    case LINK_READY:      return "connected";
    }
    return "?";
}

/* NimBLE keeps an address least significant byte first; people read it the other way. */
static void fmt_addr(char *b, size_t n, const uint8_t a[6])
{
    snprintf(b, n, "%02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
}

static int cmd_status(console_out_t out)
{
    static link_model_t m;
    link_get(&m);
    out("board    %s (%s)%s\n", m.name[0] ? m.name : "none chosen", state_name(m.state),
        m.state == LINK_READY ? (m.secure ? ", paired" : ", not encrypted") : "");
    if (m.firmware[0]) out("monitor  firmware %s\n", m.firmware);
    if (m.have_rssi)   out("signal   %d dBm\n", m.rssi_dbm);
    if (m.have_fast) {
        out("reading  %.3f V  %.4f A  %.2f W\n", m.volts, m.amps, m.watts);
    }
    if (m.have_calc) out("charge   %.1f %%  %s\n", m.soc_pct, m.mode);
    if (m.have_est && m.t_empty_s >= 0) out("empty in %ld min\n", (long)(m.t_empty_s / 60));
    if (m.have_est && m.t_full_s >= 0)  out("full in  %ld min\n", (long)(m.t_full_s / 60));
    if (m.note[0]) out("note     %s\n", m.note);
    rcon_status_t r;
    rcon_status(&r);
    out("update   %s\n", r.connected ? "a phone is connected" : r.advertising ? "visible to the app" : "off");
    return 0;
}

static int cmd_battery(console_out_t out)
{
    static link_model_t m;
    link_get(&m);
    if (m.state != LINK_READY || !m.capacity_design_uah) {
        out("not known: no monitor connected yet, or its config not read\n");
        return 1;
    }
    out("pack     %s %dS\n", m.chem[0] ? m.chem : "?", m.cells);
    out("rated    %.2f Ah\n", m.capacity_design_uah / 1e6);
    if (m.learn_count == 0) {
        out("measured not yet -- the gauge is still learning\n");
    } else if (m.capacity_learned_uah) {
        out("measured %.2f Ah, %.0f %% of rated\n", m.capacity_learned_uah / 1e6,
            100.0 * m.capacity_learned_uah / m.capacity_design_uah);
    }
    out("cycles   %lu.%03lu equivalent full\n", (unsigned long)m.cycles,
        (unsigned long)m.cycle_permille);
    out("in       %.2f Ah lifetime\n", m.in_uah / 1e6);
    out("out      %.2f Ah lifetime\n", m.out_uah / 1e6);
    return 0;
}

static int cmd_scan(int argc, char **argv, console_out_t out)
{
    int secs = argc >= 2 ? atoi(argv[1]) : 5;
    if (secs < 1) secs = 1;
    if (secs > 30) secs = 30;
    /* Browsing keeps the scan going while connected, which a link otherwise stops. */
    link_browse(true);
    vTaskDelay(pdMS_TO_TICKS(secs * 1000));
    link_browse(false);

    link_found_t f[8];
    const int    n = link_found(f, 8);
    if (n == 0) {
        out("no batmon boards in range\n");
        return 0;
    }
    for (int i = 0; i < n; i++) {
        char a[18];
        fmt_addr(a, sizeof(a), f[i].addr);
        out("%d  %-16s %s %4d dBm%s%s\n", i + 1, f[i].name, a, f[i].rssi,
            f[i].saved ? "  saved" : "", f[i].connected ? "  connected" : "");
    }
    out("use one with: connect <number | name | address>\n");
    return 0;
}

static int cmd_connect(int argc, char **argv, console_out_t out)
{
    if (argc < 2) {
        out("usage: connect <number | name | address>  (numbers from 'scan')\n");
        return 1;
    }
    link_found_t f[8];
    const int    n    = link_found(f, 8);
    const int    pick = atoi(argv[1]);
    for (int i = 0; i < n; i++) {
        char a[18];
        fmt_addr(a, sizeof(a), f[i].addr);
        if (pick == i + 1 || strcasecmp(argv[1], f[i].name) == 0 || strcasecmp(argv[1], a) == 0) {
            link_select(&f[i]);
            out("connecting to %s; remembered\n", f[i].name);
            return 0;
        }
    }
    out("no board '%s' in range -- 'scan' lists them\n", argv[1]);
    return 1;
}

static int cmd_passkey(int argc, char **argv, console_out_t out)
{
    char who[24];
    if (!link_passkey_wanted(who, sizeof(who))) {
        out("no pairing request open\n");
        return 1;
    }
    if (argc >= 2 && strcmp(argv[1], "cancel") == 0) {
        link_passkey_cancel();
        out("pairing with %s refused\n", who);
        return 0;
    }
    const char *d = argc >= 2 ? argv[1] : "";
    bool        ok = strlen(d) == 6;
    for (const char *p = d; ok && *p; p++) ok = isdigit((unsigned char)*p);
    if (!ok) {
        out("usage: passkey <the six digits %s shows> | passkey cancel\n", who);
        return 1;
    }
    link_passkey_submit((uint32_t)strtoul(d, NULL, 10));
    out("passkey sent to %s\n", who);
    return 0;
}

/*
 * A console command on the MONITOR, through the link's one-at-a-time command slot --
 * the same one the Settings screen's calibrations use. Calibration takes up to about
 * 70 s, so the wait is long; the monitor answers only once a command has finished.
 */
static int cmd_monitor(int argc, char **argv, console_out_t out)
{
    if (argc < 2) {
        out("usage: monitor <command ...>   e.g. monitor cal zero i\n");
        return 1;
    }
    char cmd[48] = "";
    for (int i = 1; i < argc; i++) {
        if (i > 1) strlcat(cmd, " ", sizeof(cmd));
        strlcat(cmd, argv[i], sizeof(cmd));
    }
    const int timeout_ms = strncmp(cmd, "cal", 3) == 0 ? 120000 : 15000;
    if (!link_command(cmd, timeout_ms)) {
        out("cannot: no monitor connected, or a command is already running\n");
        return 1;
    }
    out("running '%s' on the monitor...\n", cmd);
    link_cmd_t c;
    for (int waited = 0;; waited += 200) {
        vTaskDelay(pdMS_TO_TICKS(200));
        link_command_status(&c);
        if (c.done && !c.busy && strcmp(c.cmd, cmd) == 0) break;
        if (waited > timeout_ms + 5000) {
            out("no answer\n");
            return 1;
        }
    }
    if (!c.answered) {
        out("no answer: timed out, or the link dropped\n");
        return 1;
    }
    for (int i = 0; i < 3; i++) {
        if (c.reply[i][0]) out("%s\n", c.reply[i]);
    }
    out("monitor: exit %d\n", c.exit);
    return c.exit == 0 ? 0 : 1;
}

static int cmd_update(int argc, char **argv, console_out_t out)
{
    if (argc < 2 || (strcmp(argv[1], "on") != 0 && strcmp(argv[1], "off") != 0)) {
        out("usage: update on | off\n");
        return 1;
    }
    const bool on = strcmp(argv[1], "on") == 0;
    ui_update_mode(on);
    if (on) out("update mode: visible to the power-mon app; 'update off' to leave\n");
    else    out("update mode off\n");
    return 0;
}

/*
 * Drives a GPIO, for finding out what a board without a pin list has where -- the
 * JC1732W319's backlight, to start with. Refused on the pins that would take the
 * remote down with them: the flash (6-11) and the console UART (1, 3).
 */
static int cmd_pin(int argc, char **argv, console_out_t out)
{
    if (argc < 3) {
        out("usage: pin <gpio> <0|1>\n");
        return 1;
    }
    const int n = atoi(argv[1]), v = atoi(argv[2]);
    if (n < 0 || n > 33 || (n >= 6 && n <= 11) || n == 1 || n == 3) {
        out("GPIO %d is not one to drive\n", n);
        return 1;
    }
    gpio_reset_pin(n);
    gpio_set_direction(n, GPIO_MODE_OUTPUT);
    gpio_set_level(n, v ? 1 : 0);
    out("GPIO %d = %d\n", n, v ? 1 : 0);
    return 0;
}

static void help(console_out_t out)
{
    out("batmon remote display (%s). Commands:\n", BOARD_NAME);
    out("  status                     the link and the latest reading\n");
    out("  battery                    the pack's black box, as the monitor keeps it\n");
    out("  scan [seconds]             list the boards in range\n");
    out("  connect <n|name|address>   use that board, and remember it\n");
    out("  forget                     drop the remembered board and every bond\n");
    out("  passkey <digits>|cancel    answer the monitor's pairing request\n");
    out("  monitor <command ...>      run a command on the monitor, e.g. cal zero i\n");
    out("  update on|off              firmware update mode, for the phone app\n");
    out("  pin <gpio> <0|1>           drive a GPIO (finding a board's wiring)\n");
    out("  ver, ota [...], reboot, help\n");
}

int console_exec(int argc, char **argv, console_out_t out)
{
    if (argc == 0) {
        out("empty command\n");
        return -3;
    }
    const char *c = argv[0];
    if (strcmp(c, "ver") == 0)     return cmd_ver(out);
    if (strcmp(c, "ota") == 0)     return cmd_ota(argc, argv, out);
    if (strcmp(c, "status") == 0)  return cmd_status(out);
    if (strcmp(c, "battery") == 0) return cmd_battery(out);
    if (strcmp(c, "scan") == 0)    return cmd_scan(argc, argv, out);
    if (strcmp(c, "connect") == 0) return cmd_connect(argc, argv, out);
    if (strcmp(c, "passkey") == 0) return cmd_passkey(argc, argv, out);
    if (strcmp(c, "monitor") == 0) return cmd_monitor(argc, argv, out);
    if (strcmp(c, "update") == 0)  return cmd_update(argc, argv, out);
    if (strcmp(c, "pin") == 0)     return cmd_pin(argc, argv, out);
    if (strcmp(c, "forget") == 0) {
        link_forget();
        out("forgotten: no board, no bonds. 'scan' and 'connect' to pick one\n");
        return 0;
    }
    if (strcmp(c, "reboot") == 0) {
        out("rebooting\n");
        ota_reboot_after(500);
        return 0;
    }
    if (strcmp(c, "help") == 0) {
        help(out);
        return 0;
    }
    out("unknown command -- try 'help'\n");
    return -2;
}

/* --- the serial console ------------------------------------------------------------ */

static void serial_out(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

static void console_task(void *arg)
{
    const uart_port_t port = CONFIG_ESP_CONSOLE_UART_NUM;
    char              line[CON_LINE_MAX];
    size_t            len = 0;
    for (;;) {
        uint8_t ch;
        if (uart_read_bytes(port, &ch, 1, portMAX_DELAY) != 1) continue;
        if (ch == '\r' || ch == '\n') {
            if (len == 0) continue;
            line[len] = '\0';
            len       = 0;
            printf("\n");
            char *argv[CON_ARGS_MAX];
            int   argc = 0;
            for (char *t = strtok(line, " "); t && argc < CON_ARGS_MAX; t = strtok(NULL, " ")) {
                argv[argc++] = t;
            }
            const int st = console_exec(argc, argv, serial_out);
            if (st != 0) printf("(exit %d)\n", st);
            fflush(stdout);
        } else if (ch == 0x08 || ch == 0x7F) {
            if (len) {
                len--;
                printf("\b \b");
                fflush(stdout);
            }
        } else if (ch >= ' ' && len < CON_LINE_MAX - 1) {
            line[len++] = (char)ch;
            putchar(ch);
            fflush(stdout);
        }
    }
}

void console_start(void)
{
    /* Receive through the driver; transmit stays on the console's own path (stdout),
     * which the log uses too, so the driver gets no TX buffer. */
    const uart_port_t port = CONFIG_ESP_CONSOLE_UART_NUM;
    if (!uart_is_driver_installed(port)) {
        ESP_ERROR_CHECK(uart_driver_install(port, 256, 0, 0, NULL, 0));
    }
    xTaskCreate(console_task, "console", 4096, NULL, 2, NULL);
    ESP_LOGI("console", "serial console ready: type 'help'");
}
