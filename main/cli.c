/*
 * cli.c — the textual command interface, on whichever transport it arrived.
 *
 * One module because it was always one concern, split only by accident of growth:
 * framing and running a line, feeding lines in from the radio, and implementing the
 * commands themselves. They share the framing contract documented in CLI.md, and
 * that contract is the reason to keep them together -- a command that behaves
 * differently over BLE than over USB is a bug this file exists to prevent.
 *
 * TRANSPORT NEUTRALITY. cli_exec_line() takes a sink, so the same echo, the same
 * CRLF output, the same `exit <n>` line and the same 0x04 terminator reach a USB
 * terminal and a phone. The one documented exception is `mon`, which reads stdin and
 * so refuses when cli_is_remote() says the caller is not local.
 */

#include "cli.h"

#include "config.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app_ctx.h"
#include "ble.h"
#include "bme280.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_console.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "fixed_fmt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fuelgauge.h"
#include "history_values.h"
#include "ina219.h"
#include "lcd.h"
#include "ota.h"
#include "linenoise/linenoise.h"
#include "sdkconfig.h"
#include "sensors.h"
#include "values.h"

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

/* --- the commands ---------------------------------------------------------- */

/*
 * The commands (was console_cmds.c): M1 bring-up console.
 *
 * The point of M1 is to convince yourself the measurements are right, which means
 * poking at shunt values, ranges and offsets while a meter is connected. Doing
 * that through menuconfig-and-reflash cycles is miserable, so it lives here.
 *
 * Everything set here is volatile. Persisting configuration is M3's job, once
 * there is an NVS layout to persist it into.
 */



static app_ctx_t *s_ctx;

/* Defined further down with the rest of the calibration plumbing; `sense` needs it
 * too, because a range ceiling is part of how a reading converts. */
static void cal_autosave(void);

/* The console comes up even when the sensors did not, so that `scan` is available
 * to diagnose exactly that. Every command that touches hardware checks first. */
static bool no_sensors(void)
{
    if (!s_ctx->sensors) {
        printf("no sensors -- bring-up failed at boot. Try 'scan'.\n");
        return true;
    }
    return false;
}

/* Commands that configure the current channel need the physical device, which only
 * exists once roles are resolved (DESIGN.md 2.10.6). */
static ina219_handle_t current_dev_or_complain(void)
{
    if (no_sensors()) {
        return NULL;
    }
    ina219_handle_t d = sensors_current_dev(s_ctx->sensors);
    if (!d) {
        printf("roles unresolved -- apply a load and run 'detect', or set the mode\n"
               "with 'sensors mode <p|n|single>'.\n");
    }
    return d;
}

/* --- commands ---------------------------------------------------------------- */

static int cmd_read(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (no_sensors()) {
        return 1;
    }

    if (!sensor_lock_take(s_ctx, 2000)) {
        printf("sensor busy -- try again\n");
        return 1;
    }
    power_sample_t s;
    const esp_err_t err = sensors_read_blocking(s_ctx->sensors, &s);
    sensor_lock_give(s_ctx);
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

    if (sensors_get_mode(s_ctx->sensors) != SENSORS_MODE_SINGLE) {
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

static int cmd_sensors(int argc, char **argv)
{
    if (no_sensors()) {
        return 1;
    }

    if (argc >= 3 && strcmp(argv[1], "mode") == 0) {
        sensors_mode_t m;
        if      (strcmp(argv[2], "p")      == 0) m = SENSORS_MODE_P;
        else if (strcmp(argv[2], "n")      == 0) m = SENSORS_MODE_N;
        else if (strcmp(argv[2], "single") == 0) m = SENSORS_MODE_SINGLE;
        else if (strcmp(argv[2], "auto")   == 0) m = SENSORS_MODE_AUTO;
        else { printf("usage: sensors mode <p|n|single|auto>\n"); return 1; }

        const esp_err_t err = sensors_set_mode(s_ctx->sensors, m);
        stats_reset(history_window());
        if (err != ESP_OK) {
            printf("mode set but roles could not be assigned: %s\n",
                   esp_err_to_name(err));
        }
    } else if (argc >= 2) {
        printf("usage: sensors [mode <p|n|single|auto>]\n");
        return 1;
    }

    printf("mode          %s\n", sensors_mode_str(sensors_get_mode(s_ctx->sensors)));
    printf("positive pole %s\n",
           sensors_have_pos(s_ctx->sensors) ? "present" : "absent");
    printf("negative pole %s\n",
           sensors_have_neg(s_ctx->sensors) ? "present" : "absent");

    if (sensors_get_role_state(s_ctx->sensors) != SENSORS_ROLE_RESOLVED) {
        printf("roles        UNRESOLVED -- integration is disabled.\n");
        printf("             Apply a load and run 'detect', or set the mode.\n");
        return 0;
    }

    ina219_handle_t cd = sensors_current_dev(s_ctx->sensors);
    ina219_handle_t vd = sensors_voltage_dev(s_ctx->sensors);
    printf("current from  %s\n", cd == vd ? "shared device" : "dedicated device");
    printf("bus comp      %s\n",
           ina219_get_vbus_comp(vd) == INA219_VBUS_COMP_NONE ? "off (dedicated "
           "voltage sensor)" : "on (shared device, DESIGN.md 2.9.5)");
    return 0;
}

static int cmd_detect(int argc, char **argv)
{
    if (no_sensors()) {
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
    if (sensor_lock_take(s_ctx, 2000)) {
        err = sensors_detect_mode(s_ctx->sensors, n, &pos_uv, &neg_uv);
        sensor_lock_give(s_ctx);
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

    printf("Resolved: %s\n", sensors_mode_str(sensors_get_mode(s_ctx->sensors)));
    printf("Check the sign: charging must read positive. If it does not, use\n");
    printf("'sense sign invert' rather than rewiring.\n");
    stats_reset(history_window());
    return 0;
}

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
        ina219_get_continuous_adc(sensors_current_dev(s_ctx->sensors)) ==
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

static int cmd_stream(int argc, char **argv)
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
        s_ctx->stream_csv_header_done = false; /* re-emit the headers */
        config()->stream_enabled         = true;
    } else if (strcmp(argv[1], "text") == 0) {
        config()->stream_csv     = false;
        config()->stream_enabled = true;
    } else if (argc >= 3 && stream_set_rate(argv[1], argv[2])) {
        s_ctx->stream_csv_header_done = false; /* the header set may have changed */
        return 0;
    } else {
        printf("usage: stream <on|off|csv|text|fast|calc|diag|env <ms|off>>\n");
        return 1;
    }

    stream_show();
    return 0;
}

static int cmd_stats(int argc, char **argv)
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

static int cmd_zero(int argc, char **argv)
{
    if (!current_dev_or_complain()) {
        return 1;
    }

    uint32_t n = 256;
    if (argc >= 2) {
        const long v = strtol(argv[1], NULL, 10);
        if (v < 16 || v > 4096) {
            printf("sample count must be 16..4096\n");
            return 1;
        }
        n = (uint32_t)v;
    }

    printf("Zero-current calibration of the CURRENT sensor (DESIGN.md 5.5).\n");
    printf("DISCONNECT THE LOAD AND THE CHARGER. The firmware cannot verify this;\n");
    printf("if current is flowing it will be baked into the offset permanently.\n");
    printf("Collecting %lu samples at PGA/1", (unsigned long)n);

    const bool was_streaming = config()->stream_enabled;
    config()->stream_enabled    = false;

    int32_t   offset = 0, stddev = 0;
    esp_err_t err    = run_zero_calibration(s_ctx, n, &offset, &stddev);

    config()->stream_enabled = was_streaming;

    char b1[24], b2[24];
    printf("  measured offset : %s A\n", FMT_A(b1, offset));
    printf("  stddev          : %s A\n", FMT_A(b2, stddev));

    if (err == ESP_ERR_INVALID_STATE) {
        printf("REJECTED: too noisy -- current was flowing. Offset unchanged.\n");
        return 1;
    }
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
    if (no_sensors()) {
        return 1;
    }

    if (argc >= 3) {
        sensors_mode_t m;
        if (!parse_mode(argv[2], &m)) {
            return 1;
        }
        const esp_err_t err = sensors_set_mode(s_ctx->sensors, m);
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

    const sensors_mode_t m = sensors_get_mode(s_ctx->sensors);
    printf("shunt location %s\n", sensors_mode_str(m));
    printf("roles          %s\n",
           sensors_get_role_state(s_ctx->sensors) == SENSORS_ROLE_RESOLVED
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

static int cmd_shunt(int argc, char **argv)
{
    /* `shunt loc ...` is handled before the device check: the location can be set
     * before roles resolve, which is exactly when it is most often needed. */
    if (argc >= 2 && (strcmp(argv[1], "loc") == 0 ||
                      strcmp(argv[1], "location") == 0)) {
        return cmd_shunt_loc(argc, argv);
    }

    ina219_handle_t dev = current_dev_or_complain();
    if (!dev) {
        return 1;
    }

    if (argc >= 2) {
        const long v = strtol(argv[1], NULL, 10);
        if (v < 1000 || v > 1000000) {
            printf("shunt must be 1000..1000000 micro-ohms (below 1 mOhm the\n"
                   "full-scale current no longer fits the int32 reading)\n");
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

    if (fs_uv > 100000 && sensors_get_mode(s_ctx->sensors) != SENSORS_MODE_P) {
        char bd[24];
        printf("WARNING: %s mV full-scale drop exceeds the ~100 mV low-side budget.\n",
               FMT_MV(bd, fs_uv));
        printf("         Use a smaller shunt or lower the ceiling: sense pgamax 2\n");
    }
    printf("location   %s   ('shunt loc <p|n|single|auto>' to change)\n",
           sensors_mode_str(sensors_get_mode(s_ctx->sensors)));
    return 0;
}

static int cmd_gain(int argc, char **argv)
{
    ina219_handle_t dev = current_dev_or_complain();
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

static int cmd_offset(int argc, char **argv)
{
    ina219_handle_t dev = current_dev_or_complain();
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

static int cmd_pga(int argc, char **argv)
{
    ina219_handle_t dev = current_dev_or_complain();
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
static int cmd_profile(int argc, char **argv)
{
    ina219_handle_t dev = current_dev_or_complain();
    if (!dev) {
        return 1;
    }
    ina219_handle_t vd = sensors_voltage_dev(s_ctx->sensors);

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
            cal_autosave();
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

static int cmd_sense(int argc, char **argv)
{
    ina219_handle_t dev = current_dev_or_complain();
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
        ESP_ERROR_CHECK(ina219_set_vbus_comp(sensors_voltage_dev(s_ctx->sensors), c));
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
        cal_autosave();
    }

    static const char *vc[] = {"none", "add shunt", "subtract shunt"};
    printf("sign        %s\n",
           ina219_get_invert_sign(dev) ? "inverted" : "normal (charge = +)");
    printf("vbus comp   %s\n",
           vc[ina219_get_vbus_comp(sensors_voltage_dev(s_ctx->sensors))]);
    printf("pga ceiling %s\n", ina219_pga_str(ina219_get_pga_max(dev)));

    const uint32_t r = ina219_get_shunt_uohm(dev);
    const int64_t fs_uv = ina219_pga_fullscale_uv(ina219_get_pga_max(dev));
    char bfs[24], bdrop[24];
    printf("=> max current at the ceiling: +/-%s A  (drop %s mV)\n",
           FMT_A(bfs, (fs_uv * 1000000LL) / (int64_t)r), FMT_MV(bdrop, fs_uv));

    if (sensors_get_mode(s_ctx->sensors) != SENSORS_MODE_P) {
        printf("Low-side: keep that drop under ~100 mV (DESIGN.md 2.9.2), and make\n");
        printf("sure the shunt is the only path between battery negative and system\n");
        printf("ground -- a USB cable to this board can quietly become a second one,\n");
        printf("and the dual-sensor cross-checks cannot detect it (2.9.4 / 2.10.8).\n");
    }
    return 0;
}

static int cmd_scan(int argc, char **argv)
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
        if (i2c_master_probe(s_ctx->bus_a, addr, 50) == ESP_OK) {
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
           sensors_get_mode(s_ctx->sensors) == SENSORS_MODE_P ? "positive" : "negative");
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
    printf("  vpath    %lu uOhm  (harness drop, corrected per sample)\n",
           (unsigned long)sensors_get_r_vpath_uohm(s_ctx->sensors));
}

/* Averages n samples so a one-point gain solve is not decided by a single reading. */
static esp_err_t curve_average(uint32_t n, int64_t *sum_i_ua, int64_t *sum_v_uv,
                               uint32_t *got, bool *saturated)
{
    *sum_i_ua  = 0;
    *sum_v_uv  = 0;
    *got       = 0;
    *saturated = false;

    if (!sensor_lock_take(s_ctx, 2000)) {
        printf("sensor busy -- try again\n");
        return ESP_ERR_TIMEOUT;
    }

    /*
     * Freeze auto-ranging for the duration.
     *
     * Two reasons, and the second one is why this is not merely an optimisation. A
     * range change discards the next conversion, so a thrashing autoranger makes every
     * blocking read burn its retry budget -- measured at ~2.5 s per sample near zero
     * current, which turned a 64-sample average into well over a minute. And a
     * measurement whose scale changes underneath it is a worse measurement: the range
     * that was correct when averaging started is the right one to keep.
     *
     * Restored on every exit path below, including the error ones.
     */
    ina219_handle_t cd_f = sensors_current_dev(s_ctx->sensors);
    ina219_handle_t vd_f = sensors_voltage_dev(s_ctx->sensors);
    const bool auto_c = cd_f ? ina219_get_autorange(cd_f) : false;
    const bool auto_v = vd_f ? ina219_get_autorange(vd_f) : false;
    if (cd_f) ina219_set_autorange(cd_f, false);
    if (vd_f && vd_f != cd_f) ina219_set_autorange(vd_f, false);

    for (uint32_t k = 0; k < n; k++) {
        power_sample_t s;
        const esp_err_t err = sensors_read_blocking(s_ctx->sensors, &s);
        if (err != ESP_OK) {
            printf("\n");
            if (cd_f) ina219_set_autorange(cd_f, auto_c);
            if (vd_f && vd_f != cd_f) ina219_set_autorange(vd_f, auto_v);
            sensor_lock_give(s_ctx);
            return err;
        }
        if (s.i_valid) {
            *sum_i_ua += s.i_ua;
        }
        if (s.saturated) {
            *saturated = true;
        }
        if (s.v_valid) {
            *sum_v_uv += s.v_pack_uv;
        }
        (*got)++;
        if ((k % 4) == 0) {
            printf(".");
            fflush(stdout);
        }
    }
    printf("\n");
    if (cd_f) ina219_set_autorange(cd_f, auto_c);
    if (vd_f && vd_f != cd_f) ina219_set_autorange(vd_f, auto_v);
    sensor_lock_give(s_ctx);
    return ESP_OK;
}

/*
 * Solves gain so that `measured` reads as `reference`. Refuses well outside +/-10%,
 * where the fault is the shunt value or the divider ratio rather than gain -- see
 * ina219_set_gain_ppm(). Also refuses near zero, where the ratio is dominated by
 * offset error and the resulting gain is meaningless.
 */
static bool curve_solve_gain(int64_t measured, int64_t reference, int64_t floor_abs,
                             uint32_t old_ppm, uint32_t *new_ppm, const char *unit)
{
    char b1[24], b2[24];
    const int64_t am = measured < 0 ? -measured : measured;
    const int64_t ar = reference < 0 ? -reference : reference;

    if (ar < floor_abs || am < floor_abs) {
        /* %ld, not %lld: CONFIG_NEWLIB_NANO_FORMAT drops %ll entirely and prints
         * the literal letters instead of the number (see fixed_fmt.h). Both values
         * here are bounded well inside 32 bits. */
        printf("reference and reading must both exceed %ld %s: at low levels the\n",
               (long)floor_abs, unit);
        printf("ratio is dominated by offset error, and 'zero' is the right tool.\n");
        return false;
    }
    if ((measured < 0) != (reference < 0)) {
        printf("sign mismatch: reading and reference disagree on direction. Fix the\n");
        printf("sense leads or use 'sense sign invert' before trimming gain.\n");
        return false;
    }

    const int64_t want = ((int64_t)old_ppm * ar) / am;
    if (want < 900000 || want > 1100000) {
        printf("solved gain %ld ppm is outside +/-10%%.\n", (long)want);
        printf("That is not a gain error. Check the shunt resistance (current) or\n");
        printf("the divider ratio (voltage) -- trimming gain would only hide it.\n");
        return false;
    }

    printf("measured %s, reference %s -> gain %lu ppm\n",
           fixed_fmt(b1, sizeof(b1), measured, 1000000, 4),
           fixed_fmt(b2, sizeof(b2), reference, 1000000, 4),
           (unsigned long)want);
    *new_ppm = (uint32_t)want;
    return true;
}

/*
 * A saturated shunt reading is a range limit, not a measurement, so the ratio a gain
 * solve would compute is meaningless -- and the generic "check the shunt resistance"
 * advice actively misleads, because the resistance is usually right and the SENSE
 * WIRING is not. Diagnose it separately and say what to measure.
 */
static void explain_saturation(ina219_handle_t cd)
{
    const uint32_t r    = ina219_get_shunt_uohm(cd);
    const int64_t  fs   = ina219_pga_fullscale_uv(ina219_get_pga_max(cd));
    char           b1[24], b2[24];

    printf("the shunt channel is SATURATED at the %s ceiling.\n",
           ina219_pga_str(ina219_get_pga_max(cd)));
    printf("That reading is the range limit (%s mV = %s A), not a measurement, so\n",
           FMT_MV(b1, fs), FMT_A(b2, (fs * 1000000LL) / (int64_t)r));
    printf("no gain can be solved from it. Nothing changed.\n");
    printf("\n");
    printf("Measure VIN+ to VIN- AT THE INA219 PINS. It must equal the drop across\n");
    printf("the shunt and nothing else. If it reads volts rather than millivolts,\n");
    printf("the sense pair is not across the shunt -- a VIN- tied to system ground\n");
    printf("instead of the shunt's far side does exactly this, and so does a\n");
    printf("floating sense lead.\n");
    printf("Cross-check with 'read': if 'idle offset' is also at full scale, the\n");
    printf("fault is common to both sensors, so it is the topology and not one lead.\n");
}

static int cmd_curve(int argc, char **argv)
{
    if (no_sensors()) {
        return 1;
    }
    ina219_handle_t cd = sensors_current_dev(s_ctx->sensors);
    ina219_handle_t vd = sensors_voltage_dev(s_ctx->sensors);
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
        if (is_i && sat) {
            explain_saturation(cd);
            return 1;
        }

        uint32_t want = 0;
        bool     ok;
        if (is_i) {
            ok = curve_solve_gain(si / (int64_t)got, ref, 10000,
                                  ina219_get_gain_ppm(cd), &want, "uA");
            if (ok) {
                ESP_ERROR_CHECK(ina219_set_gain_ppm(cd, want));
            }
        } else {
            ok = curve_solve_gain(sv / (int64_t)got, ref, 500000,
                                  ina219_get_vbus_gain_ppm(vd), &want, "uV");
            if (ok) {
                ESP_ERROR_CHECK(ina219_set_vbus_gain_ppm(vd, want));
            }
        }
        if (!ok) {
            printf("nothing changed.\n");
            return 1;
        }
        stats_reset(history_window());
        curve_show(cd, vd);
        return 0;
    }

    printf("unknown: %s\n", argv[2]);
    return 1;
}

/* --- environmental sensor (DESIGN.md 2.4, 4.4) -------------------------------- */

/*
 * A fresh forced-mode read rather than the sampler's cached value. The cache exists so
 * the stream and the display cost nothing; someone typing `env` is asking what the
 * sensor says NOW, and waiting 12 ms for the truth beats being handed a value up to a
 * minute old with no way to tell.
 */
static int cmd_env(int argc, char **argv)
{
    (void)argc; (void)argv;

    if (!s_ctx->bme) {
        printf("no BME280/BMP280 fitted.\n");
        printf("Absence is a configuration, not a fault: the gauge runs without it and\n");
        printf("the temperature corrections of DESIGN.md 5.6 stay disabled rather than\n");
        printf("being guessed. 'scan' shows whether anything answers at 0x76 or 0x77.\n");
        return 1;
    }

    bme280_sample_t e;
    const esp_err_t err = bme280_read(s_ctx->bme, &e);
    if (err != ESP_OK) {
        printf("read failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    char b1[24];
    printf("chip        %s at 0x%02X\n", bme280_chip_str(bme280_chip(s_ctx->bme)),
           bme280_addr(s_ctx->bme));
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
static int cmd_ver(int argc, char **argv)
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

static int cmd_mon(int argc, char **argv)
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
               (s_ctx->sensors && sensors_have_pos(s_ctx->sensors)) ? "ok" : "--",
               CONFIG_BATMON_ADDR_NEG_POLE,
               (s_ctx->sensors && sensors_have_neg(s_ctx->sensors)) ? "ok" : "--",
               s_ctx->sensors ? sensors_mode_str(sensors_get_mode(s_ctx->sensors))
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

/* --- state of charge (DESIGN.md 5) -------------------------------------------- */

static void soc_show(void)
{
    fg_status_t   st;
    fg_config_t   c = fg_get_config();
    char          b1[24], b2[24], b3[24];

    fg_get(&st);

    printf("SoC          %s %%%s\n",
           fixed_fmt(b1, sizeof(b1), st.soc_permille, 10, 1),
           st.voltage_only ? "  (from VOLTAGE only -- no count behind it yet)" : "");
    printf("state        %s\n", fg_state_str(st.state));
    printf("charge       %s Ah of %s Ah\n",
           fixed_fmt(b1, sizeof(b1), st.charge_uas / 3600, 1000000, 3),
           fixed_fmt(b2, sizeof(b2), st.full_capacity_uah, 1000000, 1));
    printf("SoH          %lu %%   (%s Ah learned / %s Ah nameplate, %lu learn%s)\n",
           (unsigned long)((uint64_t)st.full_capacity_uah * 100 /
                           (st.design_capacity_uah ? st.design_capacity_uah : 1)),
           fixed_fmt(b1, sizeof(b1), st.full_capacity_uah, 1000000, 1),
           fixed_fmt(b2, sizeof(b2), st.design_capacity_uah, 1000000, 1),
           (unsigned long)st.learn_count, st.learn_count == 1 ? "" : "s");
    if (st.last_learn_uah) {
        printf("             last raw measurement %s Ah\n",
               fixed_fmt(b1, sizeof(b1), st.last_learn_uah, 1000000, 2));
    }
    if (st.have_full_anchor) {
        printf("learn window OPEN: %s Ah drawn since full, needs %s Ah\n",
               fixed_fmt(b1, sizeof(b1), -st.q_since_full_uas / 3600, 1000000, 2),
               fixed_fmt(b2, sizeof(b2),
                         ((int64_t)st.full_capacity_uah *
                          c.learn_min_depth_permille) / 1000, 1000000, 2));
    } else {
        printf("learn window closed -- opens at the next full-charge anchor\n");
    }
    printf("Peukert      k %s, x%s at the present rate\n",
           fixed_fmt(b1, sizeof(b1), c.peukert_q8, 256, 3),
           fixed_fmt(b2, sizeof(b2), st.peukert_factor_q16, 65536, 4));
    printf("OCV estimate %s V   (terminal minus I*R, %s mV of compensation)\n",
           FMT_V(b1, st.ocv_uv), FMT_MV(b2, st.ir_drop_uv));
    printf("lifetime     in %s Ah, out %s Ah\n",
           fixed_fmt(b1, sizeof(b1), st.cum_in_uas / 3600, 1000000, 3),
           fixed_fmt(b2, sizeof(b2), st.cum_out_uas / 3600, 1000000, 3));
    if (st.s_since_anchor == UINT32_MAX) {
        printf("last anchor  never since boot -- the count is carried over and\n");
        printf("             unverified; rest the pack to re-sync it\n");
    } else {
        printf("last anchor  %lu s ago\n", (unsigned long)st.s_since_anchor);
    }
    printf("\n");
    printf("0%% at        %s V (resting OCV)\n", FMT_V(b1, c.v_0pct_uv));
    printf("100%% at      %s V (resting OCV)\n", FMT_V(b2, c.v_100pct_uv));
    printf("full at      %s V with charge current below %s A\n",
           FMT_V(b3, c.v_full_uv), FMT_A(b1, c.i_taper_ua));
    printf("R internal   %lu uOhm   deadband %s A\n",
           (unsigned long)c.r_int_uohm, FMT_A(b2, c.i_deadband_ua));
    printf("rest         %lu s below %s A, then SoC re-syncs to resting voltage\n",
           (unsigned long)c.t_rest_s, FMT_A(b3, st.rest_current_ua));
    printf("rated rate   %s A -- the current the nameplate capacity assumes\n",
           FMT_A(b1, c.i_rated_ua));
    printf("learning     needs %lu.%lu %% depth, blended at %lu %%\n",
           (unsigned long)(c.learn_min_depth_permille / 10),
           (unsigned long)(c.learn_min_depth_permille % 10),
           (unsigned long)((uint32_t)c.learn_blend_q8 * 100 / 256));
}

static int cmd_soc(int argc, char **argv)
{
    if (argc < 2) {
        soc_show();
        printf("\n");
        printf("  soc set <permille>     force SoC, 0..1000 (500 = 50.0%%)\n");
        printf("  soc full               declare the pack full now\n");
        printf("  soc reset              forget the count; re-seed from voltage\n");
        printf("  soc cap <uAh>          capacity, e.g. 44000000 for 44 Ah\n");
        printf("  soc v0 <uV>            resting OCV at 0%%\n");
        printf("  soc v100 <uV>          resting OCV at 100%%\n");
        printf("  soc vfull <uV>         absorption voltage for full detection\n");
        printf("  soc rint <uOhm>        internal resistance for I*R compensation\n");
        printf("  soc taper <uA>         charge current below which full can latch\n");
        printf("  soc rest <s>           idle time before OCV is trusted\n");
        printf("  soc peukert <q8>       k in Q8: 256 = 1.00, 294 = 1.15 lead-acid\n");
        printf("  soc irated <uA>        rate the nameplate capacity assumes, C/20\n");
        printf("  soc depth <permille>   discharge depth required to learn capacity\n");
        printf("Everything here is stored in flash and survives a reboot.\n");
        return 0;
    }

    if (strcmp(argv[1], "full") == 0) {
        ESP_ERROR_CHECK(fg_set_full());
        printf("declared full.\n");
        soc_show();
        return 0;
    }

    if (strcmp(argv[1], "reset") == 0) {
        ESP_ERROR_CHECK(fg_reset());
        printf("count cleared. SoC will be re-seeded from the next voltage reading,\n");
        printf("and shown with a '?' until a rest period or a full charge anchors it.\n");
        return 0;
    }

    if (argc < 3) {
        printf("usage: soc <set|cap|v0|v100|vfull|rint|taper|rest> <value>\n");
        return 1;
    }

    const long v = strtol(argv[2], NULL, 10);
    if (v < 0) {
        printf("value must not be negative\n");
        return 1;
    }

    if (strcmp(argv[1], "set") == 0) {
        if (v > 1000) {
            printf("permille, so 0..1000. 500 is 50.0%%.\n");
            return 1;
        }
        ESP_ERROR_CHECK(fg_set_soc_permille((uint32_t)v));
        printf("SoC forced.\n");
        soc_show();
        return 0;
    }

    fg_config_t c = fg_get_config();
    if      (strcmp(argv[1], "cap")   == 0) c.design_capacity_uah = (uint32_t)v;
    else if (strcmp(argv[1], "v0")    == 0) c.v_0pct_uv           = (uint32_t)v;
    else if (strcmp(argv[1], "v100")  == 0) c.v_100pct_uv         = (uint32_t)v;
    else if (strcmp(argv[1], "vfull") == 0) c.v_full_uv           = (uint32_t)v;
    else if (strcmp(argv[1], "rint")  == 0) c.r_int_uohm          = (uint32_t)v;
    else if (strcmp(argv[1], "taper") == 0) c.i_taper_ua          = (uint32_t)v;
    else if (strcmp(argv[1], "rest")  == 0) c.t_rest_s            = (uint32_t)v;
    else if (strcmp(argv[1], "peukert") == 0) c.peukert_q8 = (uint16_t)v;
    else if (strcmp(argv[1], "irated")  == 0) c.i_rated_ua = (uint32_t)v;
    else if (strcmp(argv[1], "depth")   == 0)
        c.learn_min_depth_permille = (uint16_t)v;
    else {
        printf("unknown: %s\n", argv[1]);
        return 1;
    }

    const esp_err_t err = fg_set_config(&c);
    if (err != ESP_OK) {
        printf("rejected: %s\n", esp_err_to_name(err));
        printf("The window must satisfy v0 < v100 <= vfull, and capacity must be\n");
        printf("non-zero -- otherwise the SoC scale would invert or divide by zero.\n");
        printf("Peukert k must be 256..512 (1.00..2.00): below 1.0 would mean a fast\n");
        printf("discharge yields MORE capacity. Learn depth is 100..1000 permille.\n");
        return 1;
    }
    /* fg_set_config() only changes the running gauge: config.c owns persistence of
     * every setting, and without this the help text's promise ("stored in flash and
     * survives a reboot") was false -- a `soc v0` was quietly back to its old value
     * after the next reset. */
    cal_autosave();
    soc_show();
    return 0;
}

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
static void cal_autosave(void)
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

static int cmd_cal(int argc, char **argv)
{
    if (no_sensors()) {
        return 1;
    }
    ina219_handle_t cd = sensors_current_dev(s_ctx->sensors);
    ina219_handle_t vd = sensors_voltage_dev(s_ctx->sensors);
    if (!cd || !vd) {
        printf("roles unresolved -- set the location first: shunt loc <p|n|single>\n");
        return 1;
    }

    if (argc < 2) {
        cal_status(cd, vd);
        printf("\n");
        printf("  cal zero i [n]        no current flowing -> current offset\n");
        printf("  cal zero v [n]        no voltage on VBUS -> voltage offset\n");
        printf("  cal top i <uA> [n]    known current, from your meter -> gain\n");
        printf("  cal top v <uV> [n]    known voltage, from your meter -> gain\n");
        printf("  cal reset [i|v]       back to zero offset, unity gain\n");
        printf("  cal vpath <uV>        harness drop, from a LOADED terminal reading\n");
        printf("  cal save              write to flash (zero/top do this for you)\n");
        printf("  cal forget            erase the stored calibration\n");
        printf("\n");
        printf("Values are MICRO-units: 2.0134 A is 2013400, 12.6543 V is 12654300.\n");
        printf("Do zero before top on each channel; gain is solved assuming the\n");
        printf("offset is already correct.\n");
        return 0;
    }

    /* --- cal vpath ------------------------------------------------------------ */
    /*
     * Solve the harness resistance from ONE loaded reading:
     *
     *     r_vpath = (v_true − v_measured) / (−i)
     *
     * This is the fix for a voltage error that only appears under load. Absorbing such
     * an error into gain looks right at the calibration current and is wrong at every
     * other one -- it over-reads at rest by the same drop it was hiding. Measured once
     * here, the correction then tracks the current sample by sample.
     */
    if (strcmp(argv[1], "vpath") == 0) {
        if (argc < 3) {
            printf("usage: cal vpath <uV measured AT THE BATTERY TERMINALS>\n");
            printf("Apply a steady load first -- the bigger the better, since the\n");
            printf("drop being measured is proportional to it. At least 0.5 A.\n");
            return 1;
        }
        const long ref_uv = strtol(argv[2], NULL, 10);

        const bool was_streaming = config()->stream_enabled;
        config()->stream_enabled    = false;

        int64_t  si = 0, sv = 0;
        uint32_t got = 0;
        bool     sat = false;
        printf("Averaging 64 samples");
        const esp_err_t err = curve_average(64, &si, &sv, &got, &sat);
        config()->stream_enabled = was_streaming;

        if (err != ESP_OK || got == 0) {
            printf("read failed: %s\n", esp_err_to_name(err));
            return 1;
        }
        if (sat) {
            explain_saturation(cd);
            return 1;
        }

        const int64_t i_ua   = si / (int64_t)got;
        const int64_t v_meas = sv / (int64_t)got;
        char b1[24], b2[24], b3[24];

        /* Below half an amp the drop is a few millivolts and the solved resistance is
         * mostly quantisation. Refuse rather than store noise. */
        const int64_t ai = i_ua < 0 ? -i_ua : i_ua;
        if (ai < 500000) {
            printf("only %s A flowing. The drop being measured scales with current,\n",
                   FMT_A(b1, (int32_t)i_ua));
            printf("so below 0.5 A this solves mostly quantisation noise. Apply a\n");
            printf("real load and retry.\n");
            return 1;
        }

        /* r = (v_true - v_meas) / (-i). Discharge is i<0, so a v_true above v_meas
         * gives a positive resistance, as it must. */
        const int64_t num = (ref_uv - v_meas) * 1000000;
        const int64_t r   = num / (-i_ua);

        printf("measured %s V at %s A, true %s V\n", FMT_V(b1, (uint32_t)v_meas),
               FMT_A(b2, (int32_t)i_ua), FMT_V(b3, (uint32_t)ref_uv));

        if (r < 0) {
            printf("that solves a NEGATIVE resistance (%ld uOhm), which is not\n",
                   (long)r);
            printf("physical. Either the reference and the reading are swapped, or\n");
            printf("the current sign is inverted -- check 'sense sign'.\n");
            return 1;
        }
        if (r > 1000000) {
            printf("solved %ld uOhm (%s Ohm), which is implausible for a harness.\n",
                   (long)r, fixed_fmt(b1, sizeof(b1), r, 1000000, 3));
            printf("Suspect the voltage gain is already absorbing this drop: run\n");
            printf("'cal reset v' and calibrate voltage AT REST first.\n");
            return 1;
        }

        ESP_ERROR_CHECK(sensors_set_r_vpath_uohm(s_ctx->sensors, (uint32_t)r));
        printf("vpath %ld uOhm -- %s V of correction at this current\n", (long)r,
               FMT_V(b1, (uint32_t)(ref_uv - v_meas)));
        printf("The voltage now tracks load instead of being right at one current.\n");
        cal_autosave();
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
            cal_autosave();
        }
        printf("cleared, on flash too. The shunt resistance and divider ratio\n");
        printf("describe the hardware and are left alone.\n");
        cal_status(cd, vd);
        return 0;
    }

    const bool is_zero = (strcmp(argv[1], "zero") == 0);
    const bool is_top  = (strcmp(argv[1], "top") == 0);
    if ((!is_zero && !is_top) || argc < 3) {
        printf("usage: cal <zero|top> <i|v> [value] [samples]\n");
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
        uint32_t n = 256;
        if (argc >= 4) {
            const long v = strtol(argv[3], NULL, 10);
            if (v < 16 || v > 4096) {
                printf("sample count must be 16..4096\n");
                return 1;
            }
            n = (uint32_t)v;
        }

        const bool was_streaming = config()->stream_enabled;
        config()->stream_enabled    = false;
        char b1[24], b2[24];

        if (chan_i) {
            printf("ZERO POINT, current channel.\n");
            printf("DISCONNECT THE LOAD AND THE CHARGER. The firmware cannot check\n");
            printf("this; any current flowing now becomes part of the offset.\n");
            printf("Averaging %lu samples at PGA/1", (unsigned long)n);

            int32_t off = 0, sd = 0;
            const esp_err_t err = run_zero_calibration(s_ctx, n, &off, &sd);
            config()->stream_enabled = was_streaming;

            printf("  offset  %s A\n", FMT_A(b1, off));
            printf("  stddev  %s A\n", FMT_A(b2, sd));
            if (err == ESP_ERR_INVALID_STATE) {
                printf("REJECTED: too noisy -- current was flowing. Nothing changed.\n");
                printf("That rejection is the check working; find the load.\n");
                return 1;
            }
            if (err != ESP_OK) {
                printf("FAILED: %s. Nothing changed.\n", esp_err_to_name(err));
                return 1;
            }
            printf("Applied. Now: cal top i <uA> with a known current flowing.\n");
            cal_autosave();
        } else {
            printf("ZERO POINT, voltage channel.\n");
            printf("VBUS must be AT GROUND, not merely disconnected. It is measured\n");
            printf("against system ground, so an ungrounded input floats up to near\n");
            printf("the 3.3 V rail -- which looks exactly like a connected pack.\n");
            printf("Tie the VBUS node to GND. A voltage present now would be\n");
            printf("subtracted from every future reading.\n");
            printf("Averaging %lu samples", (unsigned long)n);

            int32_t  off = 0;
            uint32_t spread = 0;
            const esp_err_t err =
                run_zero_voltage_calibration(s_ctx, n, &off, &spread);
            config()->stream_enabled = was_streaming;

            printf("  offset  %s V\n", FMT_V(b1, off));
            printf("  spread  %s V\n", FMT_V(b2, (int32_t)spread));
            if (err == ESP_ERR_INVALID_STATE) {
                printf("REJECTED: that is a real voltage, not an offset. Nothing\n");
                printf("changed.\n");
                printf("If it reads near 3.3 V with nothing connected, the grounds\n");
                printf("are not tied together -- an ungrounded VBUS drifts to the\n");
                printf("rail. A good tell is the two sensors disagreeing: run\n");
                printf("'read' and compare 'pack voltage' against 'load voltage'.\n");
                printf("Identically wired sensors that differ mean a wiring fault,\n");
                printf("not a calibration problem.\n");
                return 1;
            }
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
            cal_autosave();
        }
        stats_reset(history_window());
        return 0;
    }

    /* --- cal top -------------------------------------------------------------- */
    if (argc < 4) {
        printf("usage: cal top %s <%s> [samples]\n", argv[2], chan_i ? "uA" : "uV");
        printf("Apply a steady, known %s and read it on your meter first.\n",
               chan_i ? "current" : "voltage");
        printf("Aim for 50-80%% of the working maximum: a top point near the\n");
        printf("bottom of the range makes the slope more sensitive to noise.\n");
        return 1;
    }

    const long ref = strtol(argv[3], NULL, 10);
    uint32_t   n   = 64;
    if (argc >= 5) {
        const long ns = strtol(argv[4], NULL, 10);
        if (ns < 8 || ns > 1024) {
            printf("sample count must be 8..1024\n");
            return 1;
        }
        n = (uint32_t)ns;
    }

    const bool was_streaming = config()->stream_enabled;
    config()->stream_enabled    = false;

    printf("TOP POINT, %s channel. Averaging %lu samples",
           chan_i ? "current" : "voltage", (unsigned long)n);

    int64_t  si = 0, sv = 0;
    uint32_t got = 0;
    bool     sat = false;
    const esp_err_t err = curve_average(n, &si, &sv, &got, &sat);

    config()->stream_enabled = was_streaming;

    if (err != ESP_OK || got == 0) {
        printf("read failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    if (chan_i && sat) {
        explain_saturation(cd);
        return 1;
    }
    if (chan_v && sat &&
        ina219_get_vbus_comp(vd) != INA219_VBUS_COMP_NONE) {
        /* With buscomp on, the bus reading is corrected by the shunt drop -- so a
         * saturated shunt corrupts the voltage point too. Without it, the bus
         * channel is independent and the point is still good. */
        printf("the shunt channel is saturated AND buscomp is on, so the bus\n");
        printf("reading is being corrected by a bogus shunt drop. Fix the sense\n");
        printf("wiring first, or 'sense vbuscomp none' if there is no low-side\n");
        printf("reference to correct. Nothing changed.\n");
        return 1;
    }

    uint32_t want = 0;
    bool     ok;
    if (chan_i) {
        ok = curve_solve_gain(si / (int64_t)got, ref, 10000,
                              ina219_get_gain_ppm(cd), &want, "uA");
        if (ok) {
            ESP_ERROR_CHECK(ina219_set_gain_ppm(cd, want));
        }
    } else {
        ok = curve_solve_gain(sv / (int64_t)got, ref, 500000,
                              ina219_get_vbus_gain_ppm(vd), &want, "uV");
        if (ok) {
            ESP_ERROR_CHECK(ina219_set_vbus_gain_ppm(vd, want));
        }
    }
    if (!ok) {
        printf("nothing changed.\n");
        return 1;
    }

    stats_reset(history_window());
    printf("Applied. Verify at a THIRD point -- a two-point fit always passes\n");
    printf("through its own two points: stats reset, change the load, stats.\n");
    cal_autosave();
    cal_status(cd, vd);
    return 0;
}

/* --- display (DESIGN.md 9.11) ------------------------------------------------- */

static int cmd_disp(int argc, char **argv)
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

static int cmd_ble(int argc, char **argv)
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
static int cmd_config(int argc, char **argv)
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

    ina219_handle_t cd = s_ctx->sensors ? sensors_current_dev(s_ctx->sensors) : NULL;
    ina219_handle_t vd = s_ctx->sensors ? sensors_voltage_dev(s_ctx->sensors) : NULL;

    if (s_ctx->sensors) {
        /* sensors_mode_str() is prose for a person ("N: shunt in negative lead").
         * This command's contract is that a value is what the setter takes, so
         * emit the keyword `shunt loc` accepts and nothing else. */
        const sensors_mode_t m = sensors_get_mode(s_ctx->sensors);
        printf("shunt.loc=%s\n",
               m == SENSORS_MODE_P      ? "p" :
               m == SENSORS_MODE_N      ? "n" :
               m == SENSORS_MODE_SINGLE ? "single" : "auto");
        printf("shunt.roles=%s\n",
               sensors_get_role_state(s_ctx->sensors) == SENSORS_ROLE_RESOLVED
                   ? "resolved" : "unresolved");
        printf("shunt.vpath_uohm=%lu\n",
               (unsigned long)sensors_get_r_vpath_uohm(s_ctx->sensors));
        printf("sensors.pos=%d\n", sensors_have_pos(s_ctx->sensors) ? 1 : 0);
        printf("sensors.neg=%d\n", sensors_have_neg(s_ctx->sensors) ? 1 : 0);
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
        printf("soc.cap_uah=%lu\n", (unsigned long)c.design_capacity_uah);
        printf("soc.learned_uah=%lu\n", (unsigned long)st.full_capacity_uah);
        printf("soc.v0_uv=%lu\n", (unsigned long)c.v_0pct_uv);
        printf("soc.v100_uv=%lu\n", (unsigned long)c.v_100pct_uv);
        printf("soc.vfull_uv=%lu\n", (unsigned long)c.v_full_uv);
        printf("soc.rint_uohm=%lu\n", (unsigned long)c.r_int_uohm);
        printf("soc.taper_ua=%lu\n", (unsigned long)c.i_taper_ua);
        printf("soc.rest_s=%lu\n", (unsigned long)c.t_rest_s);
        printf("soc.peukert_q8=%u\n", (unsigned)c.peukert_q8);
        printf("soc.irated_ua=%lu\n", (unsigned long)c.i_rated_ua);
        printf("soc.depth_permille=%u\n", (unsigned)c.learn_min_depth_permille);
        printf("soc.deadband_ua=%lu\n", (unsigned long)c.i_deadband_ua);
        printf("soc.permille=%lu\n", (unsigned long)st.soc_permille);
        printf("soc.voltage_only=%d\n", st.voltage_only ? 1 : 0);
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

    if (s_ctx->bme) {
        printf("env.sensor=%s\n", bme280_chip_str(bme280_chip(s_ctx->bme)));
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

static int cmd_ota(int argc, char **argv)
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

static int cmd_reboot(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("rebooting; the link will drop\n");
    ota_reboot_after(500);
    return 0;
}

static int cmd_options(int argc, char **argv)
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
    printf("errors        bus %lu, not-ready %lu, range %lu, unresolved %lu\n",
           (unsigned long)values()->err_bus, (unsigned long)values()->err_not_finished,
           (unsigned long)values()->err_range_discard,
           (unsigned long)values()->err_unresolved);

    printf("== shunt and sensors ===================== (shunt, sensors, detect)\n");
    if (s_ctx->sensors) {
        printf("location      %s\n",
               sensors_mode_str(sensors_get_mode(s_ctx->sensors)));
        printf("roles         %s\n",
               sensors_get_role_state(s_ctx->sensors) == SENSORS_ROLE_RESOLVED
                   ? "resolved" : "UNRESOLVED -- not integrating");
        printf("sensors       0x%02X %s, 0x%02X %s\n", CONFIG_BATMON_ADDR_POS_POLE,
               sensors_have_pos(s_ctx->sensors) ? "present" : "absent",
               CONFIG_BATMON_ADDR_NEG_POLE,
               sensors_have_neg(s_ctx->sensors) ? "present" : "absent");
    } else {
        printf("sensors       NONE -- bring-up failed; try 'scan'\n");
    }

    ina219_handle_t cd = s_ctx->sensors ? sensors_current_dev(s_ctx->sensors) : NULL;
    ina219_handle_t vd = s_ctx->sensors ? sensors_voltage_dev(s_ctx->sensors) : NULL;

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
    if (s_ctx->bme) {
        char be[24];
        printf("sensor        %s at 0x%02X\n",
               bme280_chip_str(bme280_chip(s_ctx->bme)), bme280_addr(s_ctx->bme));
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
    s_ctx = ctx;

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
    register_cmd("env",     "Temperature, pressure and humidity",           NULL,             cmd_env);
    register_cmd("sensors", "Show or set the dual-sensor install mode",     "[mode <p|n|single|auto>]", cmd_sensors);
    register_cmd("detect",  "Work out which pole carries the shunt (needs a load)", "[samples]", cmd_detect);
    register_cmd("stream",  "Toggle or set the periodic dump",              "<on|off|csv|ms>", cmd_stream);
    register_cmd("mon",     "Live repainting dashboard of the whole state",  "[refresh_ms]",   cmd_mon);
    register_cmd("stats",   "Show or reset the statistics window",          "[reset]",        cmd_stats);
    register_cmd("zero",    "Zero-current calibration (load disconnected!)", "[samples]",     cmd_zero);
    register_cmd("shunt",   "Shunt resistance, or its location in the pack", "[uohm | loc <p|n|single|auto>]", cmd_shunt);
    register_cmd("curve",   "Current/voltage conversion curve and calibration", "[i|v <offset|gain|ref|divider> <v>]", cmd_curve);
    register_cmd("cal",     "Guided two-point calibration, saved to flash",  "<zero|top> <i|v> [value] [n] | save | forget | reset", cmd_cal);
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
    register_cmd("soc",     "State of charge, endpoints and accumulators",   "[set|full|reset|cap|v0|v100|vfull|rint|taper|rest] [v]", cmd_soc);
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
