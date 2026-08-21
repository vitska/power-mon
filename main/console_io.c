/*
 * console_io.c — see console_io.h.
 */

#include "console_io.h"

#include <stdio.h>
#include <string.h>

#include "app_ctx.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_console.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "linenoise/linenoise.h"
#include "sdkconfig.h"

static const char *TAG = "console";

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

bool console_is_remote(void)
{
    return s_remote_task == xTaskGetCurrentTaskHandle();
}

static void emit(console_sink_t sink, void *user, const char *s)
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
static void emit_crlf(console_sink_t sink, void *user, const char *s, size_t n)
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

int console_exec_line(const char *line, console_sink_t sink, void *user, bool remote)
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
            console_exec_line(line, usb_sink, NULL, false);
            fflush(stdout);
        }
        linenoiseFree(line);
    }
}

void console_usb_start(void)
{
#if defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
    usb_serial_jtag_vfs_use_driver();

    /* CR from the terminal, and no translation outbound -- console_exec_line() emits
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
