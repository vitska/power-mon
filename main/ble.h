/*
 * ble.h — the console over BLE, as a Nordic UART Service.
 *
 * Why NUS and not the JBD emulation of DESIGN.md §7: they answer different questions.
 * §7.4's framed protocol exists so the Xiaoxiang app shows a battery, and it arrives at
 * M4 with the gauge it reports on. This is the console — `scan`, `cal top i 2000000`,
 * `soc` — reachable without a USB cable, which is what a shunt bolted into a pack in an
 * awkward place actually needs. §7.3 already puts NUS alongside 0xFF00 in the final
 * GATT layout, so this is that half, early.
 *
 * MULTIPLE CENTRALS. Up to BLE_MAX_CONNS at once — a phone watching telemetry
 * while a laptop configures, say. Two consequences shape the interface:
 *
 *   - Each connection gets its OWN line-assembly buffer. A single shared buffer would
 *     interleave bytes from two writers into one corrupt command, and the corruption
 *     would be intermittent and blamed on the radio.
 *   - A command's response goes only to the connection that sent it, while the
 *     telemetry stream goes to every subscriber. Anything else means one client's
 *     `help` output arriving in another's data feed.
 *
 * Transport only. It assembles lines and hands them to a callback; it knows nothing
 * about commands. The console bridge lives in main/ble_console.c.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Concurrent centrals. Each costs a connection slot in NimBLE and ~200 bytes here;
 *  three covers a phone, a laptop and something forgotten in a drawer. */
#define BLE_MAX_CONNS 3

/**
 * Called from the worker task, never from the NimBLE host task, so a handler may block
 * for as long as the command it is running needs.
 *
 * @param conn  the connection the line arrived on. Pass it to
 *              ble_write_conn() so the reply reaches only that client.
 */
typedef void (*ble_line_cb_t)(const char *line, uint16_t conn, void *user);

/**
 * Link security (DESIGN.md §8.5).
 *
 * OPEN is what a bench needs and what the Xiaoxiang app requires — it cannot pair.
 * BONDED is what an installation needs: LE Secure Connections, MITM protection via a
 * passkey the device displays, and a bond stored in NVS so it happens once.
 *
 * The mode is enforced in software on every write rather than through characteristic
 * flags, because flags are fixed at registration and this is switchable at runtime.
 */
typedef enum {
    BLE_SEC_OPEN   = 0, /**< no pairing; anything in range can run commands */
    BLE_SEC_BONDED = 1, /**< encryption + authenticated pairing required */
} ble_sec_mode_t;

/** Called when a passkey must be shown to the person pairing. Six digits, 0-999999.
 *  Called from the NimBLE host task, so it must not block. */
typedef void (*ble_passkey_cb_t)(uint32_t passkey, void *user);

typedef struct {
    const char *device_name;   /**< advertised name; truncated to 20 chars */
    bool        append_mac;    /**< append "-XXXX" from the MAC, so two boards on one
                                    bench are distinguishable (§7.7 uses the same
                                    suffix in its advertisement) */
    ble_line_cb_t on_line;
    ble_passkey_cb_t on_passkey; /**< optional; console log is used regardless */
    void       *user;
} ble_config_t;

esp_err_t ble_start(const ble_config_t *cfg);

/** True when at least one central is connected AND subscribed to notifications.
 *  Writing with no subscriber is dropped -- there is nowhere to put it. */
bool ble_ready(void);

/** Broadcasts to every subscribed connection. This is the telemetry path. */
void ble_write(const char *data, size_t n);

/** Sends to one connection only. This is the command-response path: a reply belongs to
 *  the client that asked, not to everyone watching. */
void ble_write_conn(uint16_t conn, const char *data, size_t n);

/** Like ble_write_conn(), but waits for the stack to free buffers rather than dropping
 *  what does not fit, giving up only when the link makes no progress for seconds. For
 *  command replies, whose tail carries the terminator. It blocks, so never call it from
 *  the NimBLE host task -- that is the task that frees the buffers. */
void ble_reply_conn(uint16_t conn, const char *data, size_t n);

/** Largest notification payload this connection takes right now: ATT_MTU - 3. */
size_t ble_payload_max(uint16_t conn);

/** Resolved advertised name, for the console to print. */
const char *ble_name(void);

typedef struct {
    bool     advertising;
    int      connections;   /**< how many centrals are attached */
    int      subscribers;   /**< how many of those enabled notifications */
    uint16_t mtu;           /**< smallest negotiated MTU across connections */
    uint32_t rx_bytes;
    uint32_t tx_bytes;
    uint32_t lines;
    uint32_t dropped;       /**< output bytes lost with no subscriber, or on overflow */
    bool     encrypted;     /**< true when EVERY connection is encrypted */
    bool     authenticated; /**< true when EVERY connection is authenticated */
    ble_sec_mode_t mode;
    int      bonds;
    uint32_t rejected;      /**< writes refused for insufficient security */
} ble_stats_t;

void ble_get_stats(ble_stats_t *out);

/* --- pairing (DESIGN.md §8.5) -------------------------------------------------- */

/** Sets the security mode and persists it. Switching to BONDED drops every open
 *  connection, because links established without pairing must not silently keep
 *  command access after the rules change. */
esp_err_t ble_set_sec_mode(ble_sec_mode_t mode);
ble_sec_mode_t ble_get_sec_mode(void);

/** Fixed passkey, or 0xFFFFFFFF for a fresh random one per pairing (the default and
 *  the safer choice -- a fixed passkey written in a manual is not a secret). */
esp_err_t ble_set_passkey(uint32_t passkey);
uint32_t  ble_get_passkey(void);

/** Lists bonded peers. Returns the count written, or negative on error. */
int ble_list_bonds(char out[][24], int max);

/** Forgets every bond. Peers must pair again. */
esp_err_t ble_clear_bonds(void);

/** Drops every current connection. */
esp_err_t ble_disconnect(void);

#ifdef __cplusplus
}
#endif
