/*
 * cli.c — the textual command interface, on whichever transport it arrived.
 *
 * This file is the framing and the transports: running one line against the console,
 * the USB task that feeds it, the BLE bridge that feeds it, and the table of what can
 * be typed. The commands themselves are in cli_sense.c, cli_stream.c, cli_gauge.c,
 * cli_cal.c and cli_report.c, which share only cli_internal.h.
 *
 * They were all one file until it reached a quarter of the firmware, which is past the
 * point where "it is all one concern" stops being an argument and starts being an
 * excuse. What genuinely is one concern is the framing contract in CLI.md, and that is
 * what stayed here -- a command that behaves differently over BLE than over USB is the
 * bug this file exists to prevent, and it prevents it for all five.
 *
 * TRANSPORT NEUTRALITY. cli_exec_line() takes a sink, so the same echo, the same
 * CRLF output, the same `exit <n>` line and the same 0x04 terminator reach a USB
 * terminal and a phone. The one documented exception is `mon`, which reads stdin and
 * so refuses when cli_is_remote() says the caller is not local.
 */

#include "cli.h"

#include "config.h"
#include "cli_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_ctx.h"
#include "ble.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_console.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ina219.h"
#include "lcd.h"
#include "ota.h"
#include "linenoise/linenoise.h"
#include "sdkconfig.h"
#include "sensors.h"

/* --- framing, the local console, and the remote entry point ------------------ */


static const char *TAG = "cli";

/*
 * One command's captured output. 4 KB rather than the 2.5 KB the BLE bridge used to
 * carry on its own: `help` is the largest realistic output at ~1.5 KB and headroom is
 * cheap, and the cap must now be the same on both transports or the framing would not
 * be identical after all.
 */
#define CAP_BYTES 4096

static char s_cap[CAP_BYTES];

/*
 * Which task, if any, is running a remote command. A plain bool would be racy: the USB
 * read loop and the BLE worker are separate tasks and can execute concurrently, so the
 * flag has to be per-task. Comparing task handles is enough and needs no TLS slot.
 */
static volatile TaskHandle_t s_remote_task;

bool cli_is_remote(void)
{
    return s_remote_task == xTaskGetCurrentTaskHandle();
}

static void emit(cli_sink_t sink, void *user, const char *s)
{
    const size_t n = strlen(s);
    if (n) {
        sink(user, s, n);
    }
}

/*
 * printf writes '\n'; the framing is defined in CRLF. Translating on the way out means
 * every command keeps its natural format strings and neither transport has to care.
 */
static void emit_crlf(cli_sink_t sink, void *user, const char *s, size_t n)
{
    size_t start = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\n') {
            if (i > start) {
                sink(user, &s[start], i - start);
            }
            sink(user, "\r\n", 2);
            start = i + 1;
        }
    }
    if (n > start) {
        sink(user, &s[start], n - start);
    }
}

int cli_exec_line(const char *line, cli_sink_t sink, void *user, bool remote)
{
    if (!line || !sink) {
        return -3;
    }

    emit(sink, user, "> ");
    emit(sink, user, line);
    emit(sink, user, "\r\n");

    /*
     * Redirect this task's stdout for the duration. ESP-IDF gives every task its own
     * stdio, so a command's printf cannot leak into another transport's capture or into
     * the log. Bounded and static: the buffer is filled by remote input, and an
     * unbounded allocation driven by that is a bad idea on 400 KB of heap.
     */
    FILE *saved = stdout;
    FILE *mem   = fmemopen(s_cap, sizeof(s_cap), "w");
    if (mem) {
        setvbuf(mem, NULL, _IONBF, 0);
        stdout = mem;
    }

    if (remote) {
        s_remote_task = xTaskGetCurrentTaskHandle();
    }

    int             ret = 0;
    const esp_err_t err = esp_console_run(line, &ret);

    s_remote_task = NULL;
    stdout        = saved; /* restore before touching the sink: it may log */

    size_t n = 0;
    if (mem) {
        fflush(mem);
        const long pos = ftell(mem);
        n = (pos > 0) ? (size_t)pos : 0;
        if (n > sizeof(s_cap)) {
            n = sizeof(s_cap);
        }
        fclose(mem);
    }

    if (n) {
        emit_crlf(sink, user, s_cap, n);
    }
    if (n >= sizeof(s_cap) - 1) {
        /* fmemopen simply stops writing, so a long output is silently short. Say so:
         * a missing tail that looks like a complete answer is the worst failure a
         * diagnostic console can have. */
        emit(sink, user, "\r\n[output truncated]\r\n");
    }

    int status = ret;
    switch (err) {
    case ESP_OK:
        break;
    case ESP_ERR_NOT_FOUND:
        emit(sink, user, "unknown command -- try 'help'\r\n");
        status = -2;
        break;
    case ESP_ERR_INVALID_ARG:
        emit(sink, user, "empty command\r\n");
        status = -3;
        break;
    default:
        emit(sink, user, "command failed to run\r\n");
        status = -1;
        break;
    }

    char tail[24];
    snprintf(tail, sizeof(tail), "exit %d\r\n%c", status, BATMON_EOT);
    emit(sink, user, tail);
    return status;
}

/* --- the local console -------------------------------------------------------- */

static void usb_sink(void *user, const char *data, size_t len)
{
    (void)user;
    fwrite(data, 1, len, stdout);
}

static void console_task(void *arg)
{
    (void)arg;

    /* Escape sequences are how line editing works at all. When the far end cannot do
     * them -- a raw pipe, a script -- linenoise still functions, just without editing,
     * and saying so beats leaving someone to wonder why the arrow keys emit garbage. */
    if (linenoiseProbe() != 0) {
        linenoiseSetDumbMode(1);
        printf("Terminal does not support escape sequences; line editing disabled.\n");
    }

    for (;;) {
        char *line = linenoise("batmon> ");
        if (line == NULL) {
            /* EOF or a transient read error. Not fatal, and not a reason to spin. */
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (line[0] != '\0') {
            linenoiseHistoryAdd(line);
            cli_exec_line(line, usb_sink, NULL, false);
            fflush(stdout);
        }
        linenoiseFree(line);
    }
}

void cli_usb_start(void)
{
#if defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
    usb_serial_jtag_vfs_use_driver();

    /* CR from the terminal, and no translation outbound -- cli_exec_line() emits
     * CRLF itself, and a second translation here would produce CR CR LF. */
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_CR);
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_LF);
#endif

    setvbuf(stdin, NULL, _IONBF, 0);

    linenoiseSetMultiLine(1);
    linenoiseHistorySetMaxLen(20);
    linenoiseSetMaxLineLen(160);
    linenoiseSetCompletionCallback(&esp_console_get_completion);
    linenoiseSetHintsCallback((linenoiseHintsCallback *)&esp_console_get_hint);

    if (xTaskCreate(console_task, "console", 6144, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "console task create failed");
    }
}

/* --- the BLE bridge --------------------------------------------------------- */

/*
 * The BLE bridge (was ble_console.c): runs console commands arriving over BLE and sends the output back.
 *
 * The bridge is deliberately thin, and the interesting part is how output is
 * captured. Every console command in this firmware prints with plain printf, which
 * is right -- they were written for a serial console and should not have to know
 * about transports. So instead of rewriting them around a callback, this redirects
 * the worker task's stdout into a fixed buffer for the duration of one command.
 *
 * That works because ESP-IDF gives every task its own stdio: reassigning stdout here
 * cannot disturb the USB console, the sampler's stream, or a log line from any other
 * task. It is also why the buffer is static and bounded rather than growing -- `help`
 * is the largest realistic output, a truncation marker is honest, and an unbounded
 * allocation driven by remote input is a bad idea on a device with 400 KB of heap.
 */


static const char *BLE_TAG = "cli.ble";

/* LF->CRLF translation, framing and the exit status live in cli_exec_line() above,
 * shared with the local console. What is left here is the sink. */

/*
 * Unicast: the reply belongs to the client that asked. With several centrals attached, a
 * broadcast reply would drop one client's `help` output into another's data feed.
 *
 * COALESCED, AND CUT AT LINE ENDS. cli_exec_line() hands the sink every line and every
 * CRLF as separate calls. Passed straight through, each became its own notification:
 * `config` is ~65 lines, so ~130 notifications, which exhausts NimBLE's buffer pool
 * several times over -- the tail, terminator included, was dropped, and the app
 * reported the command as missing. Packing into MTU-sized notifications makes the same
 * reply four or five of them.
 *
 * Cutting at a line end matters too. The sampler task broadcasts telemetry at the same
 * time, and the client parses one byte stream, so a record landing between a line and
 * its CRLF splices the two. A notification that carries only whole lines cannot be
 * split by one. (Below an MTU that fits a line there is no avoiding a split.)
 */
#define REPLY_BUF 509 /* ATT_MTU 512, NimBLE's preferred MTU, less the 3-byte header */

typedef struct {
    uint16_t conn;
    size_t   cap; /* this connection's payload limit, at most REPLY_BUF */
    size_t   len;
    char     buf[REPLY_BUF];
} reply_t;

/* Sends everything up to and including the last line end, keeping any partial line;
 * with no line end in the buffer at all, or with `all`, sends everything. */
static void reply_flush(reply_t *r, bool all)
{
    size_t cut = r->len;
    if (!all) {
        while (cut > 0 && r->buf[cut - 1] != '\n') {
            cut--;
        }
        if (cut == 0) {
            cut = r->len;
        }
    }
    if (cut == 0) {
        return;
    }
    ble_reply_conn(r->conn, r->buf, cut);
    memmove(r->buf, r->buf + cut, r->len - cut);
    r->len -= cut;
}

static void ble_sink(void *user, const char *data, size_t len)
{
    reply_t *r = user;
    while (len > 0) {
        if (r->len + len > r->cap) {
            reply_flush(r, false);
        }
        size_t take = r->cap - r->len;
        if (take > len) {
            take = len;
        }
        memcpy(r->buf + r->len, data, take);
        r->len += take;
        data   += take;
        len    -= take;
        if (r->len == r->cap) {
            reply_flush(r, false);
        }
    }
}

static void on_line(const char *line, uint16_t conn, void *user)
{
    (void)user;
    /* Static rather than on the stack: 0.5 KB, and only the one worker task runs
     * commands, one at a time. */
    static reply_t r;
    r.conn = conn;
    r.len  = 0;
    r.cap  = ble_payload_max(conn);
    if (r.cap > sizeof(r.buf)) {
        r.cap = sizeof(r.buf);
    }
    /* remote = true: `mon` refuses rather than repainting into a link that cannot
     * carry the keypress that would stop it. */
    cli_exec_line(line, ble_sink, &r, true);
    reply_flush(&r, true);
}

/*
 * Firmware-update data from the OTA characteristic, on the host task. The refusal codes
 * are the protocol a client sees as the write's status (CLI.md §6, "Firmware update"),
 * so each ota_write() outcome gets its own.
 */
#define OTA_ATT_NO_SESSION 0x80 /* no `ota begin`, or the session was abandoned */
#define OTA_ATT_BAD_OFFSET 0x81 /* offset is not the byte count received so far */
#define OTA_ATT_TOO_LONG   0x82 /* runs past the size `ota begin` announced */
#define OTA_ATT_BUSY       0x83 /* erasing or finishing; retry shortly */
#define OTA_ATT_FLASH      0x84 /* flash refused it, or it does not start like an image */

static int on_ota(const uint8_t *data, size_t len, uint16_t conn, void *user)
{
    (void)conn; (void)user;
    switch (ota_write(data, len)) {
    case ESP_OK:                return 0;
    case ESP_ERR_INVALID_STATE: return OTA_ATT_NO_SESSION;
    case ESP_ERR_INVALID_ARG:   return OTA_ATT_BAD_OFFSET;
    case ESP_ERR_INVALID_SIZE:  return OTA_ATT_TOO_LONG;
    case ESP_ERR_TIMEOUT:       return OTA_ATT_BUSY;
    default:                    return OTA_ATT_FLASH;
    }
}

/*
 * Show the pairing passkey wherever it can be seen. The OLED is the primary place --
 * that is what makes DISPLAY_ONLY pairing honest -- but a board with no panel fitted
 * is a normal configuration here, and ble.c already logs the passkey at WARN so
 * the USB console shows it either way. A zero clears the screen after pairing.
 */
static void on_passkey(uint32_t passkey, void *user)
{
    (void)user;
    if (lcd_present()) {
        lcd_show_passkey(passkey);
    }
}

esp_err_t cli_ble_start(void)
{
    const ble_config_t cfg = {
        .device_name = CONFIG_BATMON_BLE_NAME,
#ifdef CONFIG_BATMON_BLE_APPEND_MAC
        .append_mac = true,
#endif
        .on_line    = on_line,
        .on_ota     = on_ota,
        .on_passkey = on_passkey,
        .user       = NULL,
    };

    const esp_err_t err = ble_start(&cfg);
    if (err != ESP_OK) {
        /* Not fatal, on purpose: the USB console is unaffected and the gauge does not
         * depend on the radio (DESIGN.md §10). */
        ESP_LOGW(BLE_TAG, "BLE console unavailable: %s", esp_err_to_name(err));
    }
    return err;
}

/* --- what the command files share ------------------------------------------- */

/*
 * The context and the two questions every command that touches hardware has to ask
 * first. Declared in cli_internal.h, defined here because this is the file that is
 * handed the context at startup.
 */



app_ctx_t *cli_ctx;


/* The console comes up even when the sensors did not, so that `scan` is available
 * to diagnose exactly that. Every command that touches hardware checks first. */
bool cli_no_sensors(void)
{
    if (!cli_ctx->sensors) {
        printf("no sensors -- bring-up failed at boot. Try 'scan'.\n");
        return true;
    }
    return false;
}

/* Commands that configure the current channel need the physical device, which only
 * exists once roles are resolved (DESIGN.md 2.10.6). */
ina219_handle_t cli_current_dev_or_complain(void)
{
    if (cli_no_sensors()) {
        return NULL;
    }
    ina219_handle_t d = sensors_current_dev(cli_ctx->sensors);
    if (!d) {
        printf("roles unresolved -- apply a load and run 'detect', or set the mode\n"
               "with 'sensors mode <p|n|single>'.\n");
    }
    return d;
}

/* --- registration ------------------------------------------------------------ */

static void register_cmd(const char *cmd, const char *help, const char *hint,
                         esp_console_cmd_func_t fn)
{
    const esp_console_cmd_t c = {
        .command = cmd,
        .help    = help,
        .hint    = hint,
        .func    = fn,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&c));
}

void cli_start(app_ctx_t *ctx)
{
    cli_ctx = ctx;

    /*
     * esp_console_init() rather than a REPL. The REPL owns the read loop and prints its
     * own results -- including its own text for an unknown command -- so a wrapper
     * cannot give the local console the same framing the BLE one has. console_io.c runs
     * the loop instead, and both transports go through cli_exec_line().
     */
    esp_console_config_t cc = ESP_CONSOLE_CONFIG_DEFAULT();
    cc.max_cmdline_length   = 160; /* matches the BLE line limit */
    cc.max_cmdline_args     = 12;
    ESP_ERROR_CHECK(esp_console_init(&cc));
    ESP_ERROR_CHECK(esp_console_register_help_command());

    register_cmd("ver",     "Protocol and firmware version, for clients",   NULL,             cmd_ver);
    register_cmd("read",    "Take and print one sample",                    NULL,             cmd_read);
    register_cmd("raw",     "Each INA219 read directly, and which pole each is on", NULL,   cmd_raw);
    register_cmd("env",     "Temperature, pressure and humidity",           NULL,             cmd_env);
    register_cmd("sensors", "Show or set the dual-sensor install mode",     "[mode <p|n|single|auto>]", cmd_sensors);
    register_cmd("detect",  "Work out which pole carries the shunt (needs a load)", "[samples]", cmd_detect);
    register_cmd("stream",  "Toggle or set the periodic dump",              "<on|off|csv|ms>", cmd_stream);
    register_cmd("mon",     "Live repainting dashboard of the whole state",  "[refresh_ms]",   cmd_mon);
    register_cmd("stats",   "Show or reset the statistics window",          "[reset]",        cmd_stats);
    register_cmd("zero",    "Zero-current calibration (load disconnected!)", NULL,             cmd_zero);
    register_cmd("shunt",   "Shunt resistance, or its location in the pack", "[uohm | loc <p|n|single|auto>]", cmd_shunt);
    register_cmd("curve",   "Current/voltage conversion curve and calibration", "[i|v <offset|gain|ref|divider> <v>]", cmd_curve);
    register_cmd("cal",     "Guided two-point calibration, saved to flash",  "<zero|top> <i|v> [value] | shunt <uA> | save | forget | reset", cmd_cal);
    register_cmd("gain",    "Show or set the gain trim in ppm",             "[ppm]",          cmd_gain);
    register_cmd("offset",  "Show or set the current offset in uA",         "[uA]",           cmd_offset);
    register_cmd("pga",     "Show or set the PGA range",                    "<auto|1|2|4|8>", cmd_pga);
    register_cmd("profile", "Switch sampling profile",                      "<continuous|triggered>", cmd_profile);
    register_cmd("sense",   "Low-side sensing: sign, bus comp, PGA ceiling", "[sign|vbuscomp|pgamax] [v]", cmd_sense);
    register_cmd("scan",    "Scan the I2C bus for devices",                 NULL,             cmd_scan);
    register_cmd("disp",    "Debug screens on the OLED",                    "[on|off|screen <n|auto>|contrast <v>]", cmd_disp);
#if CONFIG_BATMON_BLE_ENABLE
    register_cmd("ble",     "BLE link, pairing and bonds",                  "[pair <open|bonded>|passkey <random|NNNNNN>|bonds|unpair|disconnect]", cmd_ble);
#endif
    register_cmd("hist",    "SoC over time for graphs: 288 points, settable interval", "[clear | every <s>]", cmd_hist);
    register_cmd("battery", "Battery chemistry and cells: the SoC curve and endpoints", "[list | <chemistry> [cells]]", cmd_battery);
    register_cmd("soc",     "State of charge, endpoints and accumulators",   "[set|full|reset [all]|cap|v0|v100|vfull|rint|taper|rest|irest] [v]", cmd_soc);
    register_cmd("options", "Everything that is set, in one place",         NULL,             cmd_options);
    register_cmd("config",  "Every setting as key=value, for programs",   NULL,             cmd_config);
    register_cmd("ota",     "Firmware update: status, receive, confirm, roll back", "[status|begin <bytes> <sha256>|end|abort|confirm|rollback]", cmd_ota);
    register_cmd("reboot",  "Restart the device",                             NULL,             cmd_reboot);

    printf("\n");
    printf("bat-monitor console, protocol %d. 'help' lists commands, 'ver' for a\n",
           BATMON_CLI_PROTOCOL);
    printf("machine-readable handshake. Same commands and framing over BLE.\n");
    printf("Typical first run:  scan  ->  shunt loc  ->  cal zero i  ->  cal top i\n");
    printf("\n");

    cli_usb_start();
    ESP_LOGI(TAG, "console ready, protocol %d", BATMON_CLI_PROTOCOL);
}
