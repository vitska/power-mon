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
    char     mode[12];         /* the device's gauge state, as sent: CHARGE, FULL, REST... */
    float    amps_avg;         /* ~1 minute moving average, for time estimates */
    uint32_t capacity_mah;     /* learned capacity, else design, from `config` */
    char     chem[12];         /* battery.chem, "" on firmware before 0.8.0 */
    int      cells;
    int64_t  last_data_us;     /* esp_timer time of the last telemetry record */
    char     note[48];         /* the latest thing worth telling the user */
    /* Environment, from the `e` records; NAN where the board's sensor lacks a channel
     * (a BMP280 has no humidity) or has no sensor at all. */
    bool     have_env;
    float    temp_c, humid_pct, press_hpa;

    uint32_t rx_packets;       /* notifications received, ever: for the activity light */
    uint32_t tx_packets;       /* commands written, ever */

    bool    have_rssi;         /* false until the first read comes back */
    int8_t  rssi_dbm;          /* this connection's RSSI, read locally every ~2 s */
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

/* --- SoC history, from the monitor ------------------------------------------------- */

/*
 * The monitor keeps SoC every 10 minutes for 48 hours (`hist`, firmware 0.9.0 and
 * later); the remote fetches it on connect and every two minutes and draws it.
 */
#define LINK_HIST_MAX  288
#define LINK_HIST_NONE 0xFFFF

typedef struct {
    bool     supported;   /* false: the monitor's firmware has no `hist` */
    int      count;       /* points in pts[], oldest first */
    uint32_t interval_s;
    uint32_t age_s;       /* how long ago the newest point was taken, as of now */
    uint32_t seq;         /* bumps on every fetch that changed anything */
    uint16_t pts[LINK_HIST_MAX];
    char     st[LINK_HIST_MAX]; /* the monitor's gauge state letter per point, '-' none */
} link_hist_t;

void link_history(link_hist_t *out);

/* --- console commands from the UI (calibration) ------------------------------------ */

typedef struct {
    bool    busy;         /* sent, waiting for its terminator */
    bool    done;         /* finished; the fields below describe it */
    bool    answered;     /* false: timed out or the link dropped */
    int     exit;         /* the command's exit status; 0 is success */
    char    cmd[48];
    int64_t started_us;
    int     timeout_ms;
    char    reply[3][56]; /* the last lines of output, oldest first, "" if unused */
} link_cmd_t;

/** Queues one console command, run after anything already in flight. False when the
 *  link is not ready or another UI command has not finished. */
bool link_command(const char *cmd, int timeout_ms);

void link_command_status(link_cmd_t *out);

/** While pairing: true, with the board's name in `who`. */
bool link_passkey_wanted(char *who, size_t n);
void link_passkey_submit(uint32_t passkey);
void link_passkey_cancel(void);
