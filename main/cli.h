/*
 * cli.h — one command execution path, two transports.
 *
 * The framing a client sees must not depend on which wire it arrived over, so both the
 * USB console and the BLE console call cli_exec_line(). It does the echo, the
 * capture, the execution and the terminator; the only thing that differs between
 * transports is the sink the bytes are handed to.
 *
 * This is why the USB side does not use esp_console_start_repl(): that helper owns the
 * read loop and prints its own results, including its own message for an unknown
 * command, which cannot be wrapped. Running the loop here costs about forty lines and
 * buys byte-identical framing on both transports — including the error paths, which is
 * exactly where a client most needs to be able to trust the framing.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#include "app_ctx.h"

/** Receives already-framed output. @p len is never zero. */
typedef void (*cli_sink_t)(void *user, const char *data, size_t len);

/**
 * Echo, run, frame. Emits, in order:
 *
 *     "> <line>\r\n"                 the echo
 *     <command output, LF -> CRLF>
 *     "exit <n>\r\n"
 *     "\x04"                         one byte, the terminator
 *
 * @param remote  true when the caller is not the local console. Commands that read
 *                stdin (only `mon`) use cli_is_remote() to refuse rather than
 *                block on a stream that will never carry a keypress.
 * @return the command's exit status, as reported in the `exit` line.
 */
int cli_exec_line(const char *line, cli_sink_t sink, void *user, bool remote);

/** True while the calling task is executing a command that arrived remotely. */
bool cli_is_remote(void);

/** Starts the local console: device, line editing, and the read loop. */
void cli_usb_start(void);

/** Registers every command and starts the local console. The single entry point
 *  main.c needs; the BLE side attaches itself from here when it is enabled. */
void cli_start(app_ctx_t *ctx);

/** Bridges the console onto the BLE transport. Safe to call when BLE is disabled in
 *  Kconfig -- it becomes a no-op. A failure here is logged, never fatal (§10). */
esp_err_t cli_ble_start(void);
