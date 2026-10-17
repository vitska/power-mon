/*
 * console.h — the remote's own console: every setting the touch screens offer, as
 * commands, on every board.
 *
 * One command set, two ways in. The serial console (the USB port, 115200) reads lines
 * here; the BLE console the phone app uses (rcon.c) hands its lines to console_exec()
 * too. On a board without touch (board.h) this is the only way to pick a board, pair,
 * calibrate or open the update screen; on the others it is a second way.
 *
 *   help                       the list below
 *   ver                        protocol, firmware, board, chip, mac
 *   status                     the link and the latest reading
 *   battery                    the pack's black box, as the monitor keeps it
 *   scan [seconds]             list the boards in range (default 5 s)
 *   connect <n | name | addr>  use that board, and remember it
 *   forget                     drop the remembered board and every bond
 *   passkey <six digits>       answer a pairing request; `passkey cancel` refuses it
 *   monitor <command ...>      run a console command on the monitor (CLI.md), e.g.
 *                              `monitor cal zero i`; prints its last lines of reply
 *   update on | off            firmware update mode: visible to the phone app
 *   ota [...]                  the OTA session, as on the monitor (CLI.md §6)
 *   reboot
 */
#pragma once

/** Where a command's output goes: printf-like, '\n' line ends. */
typedef void (*console_out_t)(const char *fmt, ...);

/**
 * Runs one command, already split into words. Returns its exit status: 0 success,
 * 1 refused or failed, -2 unknown command (what the phone app reads as "this firmware
 * has no such command"). May block: `scan` for its seconds, `monitor` until the
 * monitor answers.
 */
int console_exec(int argc, char **argv, console_out_t out);

/** Starts the serial console's task. */
void console_start(void);
