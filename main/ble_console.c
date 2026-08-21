/*
 * ble_console.c — runs console commands arriving over BLE and sends the output back.
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

#include "ble_console.h"

#include <stdio.h>
#include <string.h>

#include "ble_serial.h"
#include "display_debug.h"
#include "esp_console.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "ble_con";

/* One command's worth of output. `help` with every command registered is ~1.5 KB. */
static char s_out[2560];

/*
 * printf writes '\n'; a BLE terminal wants "\r\n". Translating on the way out keeps
 * every command's format strings unchanged and correct for both transports.
 */
static void write_crlf(const char *s, size_t n)
{
    size_t start = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\n') {
            if (i > start) {
                ble_serial_write(&s[start], i - start);
            }
            ble_serial_write("\r\n", 2);
            start = i + 1;
        }
    }
    if (n > start) {
        ble_serial_write(&s[start], n - start);
    }
}

static void on_line(const char *line, void *user)
{
    (void)user;

    FILE *saved = stdout;
    FILE *mem   = fmemopen(s_out, sizeof(s_out), "w");
    if (mem) {
        setvbuf(mem, NULL, _IONBF, 0);
        stdout = mem;
    }

    int             ret = 0;
    const esp_err_t err = esp_console_run(line, &ret);

    /* Restore before touching the BLE transport: ble_serial_write() may log, and a
     * log line landing in the capture buffer would be confusing at best. */
    stdout = saved;

    size_t n = 0;
    if (mem) {
        fflush(mem);
        n = (size_t)ftell(mem);
        if (n > sizeof(s_out)) {
            n = sizeof(s_out);
        }
        fclose(mem);
    }

    if (n > 0) {
        write_crlf(s_out, n);
    }

    /* fmemopen stops writing at the buffer end, so a long output is silently short.
     * Say so -- a missing tail that looks like a complete answer is the worst
     * possible failure for a diagnostic console. */
    if (n >= sizeof(s_out) - 1) {
        ble_serial_write("\r\n[output truncated -- use the USB console]\r\n", 0);
    }

    switch (err) {
    case ESP_OK:
        if (ret != 0) {
            char b[48];
            snprintf(b, sizeof(b), "(exit %d)\r\n", ret);
            ble_serial_write(b, 0);
        }
        break;
    case ESP_ERR_NOT_FOUND:
        ble_serial_write("unknown command -- try 'help'\r\n", 0);
        break;
    case ESP_ERR_INVALID_ARG:
        ble_serial_write("empty command\r\n", 0);
        break;
    default:
        ble_serial_write("command failed to run\r\n", 0);
        break;
    }
}

/*
 * Show the pairing passkey wherever it can be seen. The OLED is the primary place --
 * that is what makes DISPLAY_ONLY pairing honest -- but a board with no panel fitted
 * is a normal configuration here, and ble_serial already logs the passkey at WARN so
 * the USB console shows it either way. A zero clears the screen after pairing.
 */
static void on_passkey(uint32_t passkey, void *user)
{
    (void)user;
    if (display_debug_present()) {
        display_debug_show_passkey(passkey);
    }
}

esp_err_t ble_console_start(void)
{
    const ble_serial_config_t cfg = {
        .device_name = CONFIG_BATMON_BLE_NAME,
#ifdef CONFIG_BATMON_BLE_APPEND_MAC
        .append_mac = true,
#endif
        .on_line    = on_line,
        .on_passkey = on_passkey,
        .user       = NULL,
    };

    const esp_err_t err = ble_serial_start(&cfg);
    if (err != ESP_OK) {
        /* Not fatal, on purpose: the USB console is unaffected and the gauge does not
         * depend on the radio (DESIGN.md §10). */
        ESP_LOGW(TAG, "BLE console unavailable: %s", esp_err_to_name(err));
    }
    return err;
}
