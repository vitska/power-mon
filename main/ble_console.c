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

#include "app_ctx.h"
#include "ble_serial.h"
#include "console_io.h"
#include "display_debug.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "ble_con";

/* Output buffering, LF->CRLF translation, framing and the exit status all live in
 * console_io.c now, shared with the local console. What is left here is the sink. */

/*
 * Unicast: the reply belongs to the client that asked. With several centrals attached, a
 * broadcast reply would drop one client's `help` output into another's data feed. The
 * connection handle rides through console_exec_line()'s opaque user pointer.
 */
static void ble_sink(void *user, const char *data, size_t len)
{
    ble_serial_write_conn((uint16_t)(uintptr_t)user, data, len);
}

static void on_line(const char *line, uint16_t conn, void *user)
{
    (void)user;
    /* remote = true: `mon` refuses rather than repainting into a link that cannot
     * carry the keypress that would stop it. */
    console_exec_line(line, ble_sink, (void *)(uintptr_t)conn, true);
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
