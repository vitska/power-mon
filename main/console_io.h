/*
 * console_io.h — one command execution path, two transports.
 *
 * The framing a client sees must not depend on which wire it arrived over, so both the
 * USB console and the BLE console call console_exec_line(). It does the echo, the
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

/** Receives already-framed output. @p len is never zero. */
typedef void (*console_sink_t)(void *user, const char *data, size_t len);

/**
 * Echo, run, frame. Emits, in order:
 *
 *     "> <line>\r\n"                 the echo
 *     <command output, LF -> CRLF>
 *     "exit <n>\r\n"
 *     "\x04"                         one byte, the terminator
 *
 * @param remote  true when the caller is not the local console. Commands that read
 *                stdin (only `mon`) use console_is_remote() to refuse rather than
 *                block on a stream that will never carry a keypress.
 * @return the command's exit status, as reported in the `exit` line.
 */
int console_exec_line(const char *line, console_sink_t sink, void *user, bool remote);

/** True while the calling task is executing a command that arrived remotely. */
bool console_is_remote(void);

/** Starts the local console: device, line editing, and the read loop. */
void console_usb_start(void);
