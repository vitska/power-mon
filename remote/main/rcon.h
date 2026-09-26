/*
 * rcon.h — the remote's own small console, served to the phone app over BLE, so the
 * app can update the remote's firmware.
 *
 * The remote is a BLE central towards the monitor. For updates it is also a
 * peripheral: it advertises as batmon-remote-XXXX and serves the same Nordic UART
 * service, the same framing (echo, CRLF, `exit N`, 0x04) and the same OTA
 * characteristic and `ota ...` commands as the monitor (CLI.md §6). The phone app's
 * firmware update therefore works against it unchanged.
 *
 * It advertises only while asked to -- the Firmware update screen, or on the first
 * boot of a new image so the app can reconnect and confirm it -- so the rest of the
 * time the remote stays out of every device list and accepts no firmware from anyone.
 */
#pragma once

#include <stdbool.h>

/** Registers the GATT service. Called by link.c between nimble_port_init() and the
 *  host starting, which is the only time services can be added. */
void rcon_register(void);

/** Called by link.c when the host has synced: starts advertising if it was asked for
 *  before the stack was ready. */
void rcon_on_sync(void);

/** Start or stop advertising. Stopping leaves a phone already connected alone. */
void rcon_advertise(bool on);

typedef struct {
    bool advertising;
    bool connected;   /* a phone is connected to the remote */
    char name[24];    /* the advertised name */
} rcon_status_t;

void rcon_status(rcon_status_t *out);
