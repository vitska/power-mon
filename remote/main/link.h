/*
 * link.h — the BLE connection to one battery monitor, and what it has told us.
 *
 * The remote is a BLE central speaking the monitor's console protocol (CLI.md) over
 * the Nordic UART Service, exactly as the phone app does: subscribe, `ver`, `config`,
 * `stream csv`, then consume telemetry records. Everything the UI shows comes out of
 * link_get() as one consistent snapshot.
 *
 * It remembers one board. With none remembered it takes the first `batmon-*` it
 * finds; after that, switching is an explicit choice on the Devices screen. A dropped
 * link is re-established on its own, by scanning for the remembered address.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    LINK_SCANNING,   /* looking for the board */
    LINK_CONNECTING,
    LINK_SETUP,      /* discovering, subscribing, handshake */
    LINK_PAIRING,    /* the board wants a passkey typed in */
    LINK_READY,
} link_state_t;

typedef struct {
    link_state_t state;
    char         name[24];     /* the board connected to, or being looked for */
    bool         secure;       /* encrypted and authenticated link */
    int          protocol;     /* from `ver`; 0 until known */
    char         firmware[24];

    /* Telemetry. The have_ flags say whether a value has arrived on this link. */
    bool     have_fast, have_calc;
    float    volts, amps, watts, soc_pct, charge_ah;
    char     mode[12];         /* the gauge state: COUNTING, RESTING, FULL, ... */
    float    amps_avg;         /* ~1 minute moving average, for time estimates */
    uint32_t capacity_mah;     /* learned capacity, else design, from `config` */
    char     chem[12];         /* battery.chem, "" on firmware before 0.8.0 */
    int      cells;
    int64_t  last_data_us;     /* esp_timer time of the last telemetry record */
    char     note[48];         /* the latest thing worth telling the user */
} link_model_t;

typedef struct {
    uint8_t addr[6];
    uint8_t addr_type;
    char    name[24];
    int8_t  rssi;
    bool    saved;             /* the remembered board */
    bool    connected;
} link_found_t;

void link_start(void);

void link_get(link_model_t *out);

/** Boards seen recently, strongest first. Returns how many were written. */
int link_found(link_found_t *out, int max);

/** Keep scanning while connected, so the Devices screen shows every board in range.
 *  Off by default: scanning shares the radio with the link. */
void link_browse(bool on);

/** Connect to this board and remember it, dropping any current link. */
void link_select(const link_found_t *d);

/** Forget the remembered board and every bond; drop the link. */
void link_forget(void);

/** While pairing: true, with the board's name in `who`. */
bool link_passkey_wanted(char *who, size_t n);
void link_passkey_submit(uint32_t passkey);
void link_passkey_cancel(void);
