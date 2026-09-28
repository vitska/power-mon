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
    /* The device's own estimates, seconds, -1 = not applicable (have_est false: the
     * monitor's firmware predates them). */
    bool     have_est;
    int32_t  t_full_s, t_empty_s, settle_s;
    float    amps_avg;         /* ~1 minute moving average, for time estimates */
    /*
     * Capacity, from `config`. The monitor LEARNS its pack's real capacity between two
     * reference points (DESIGN.md 5, `soc.learned_uah`) and that arithmetic stays there:
     * nothing here computes it, and the remote must not, or two devices watching one
     * pack would quote two different capacities. Both are kept because they mean
     * different things -- what the pack was sold as, and what it actually holds.
     */
    uint32_t capacity_mah;     /* learned capacity, else design: what to quote */
    /*
     * In MICRO-amp-hours, as the wire carries them. Not scaled to mAh on the way in:
     * the two figures differ by a fraction of an Ah for most of a pack's life, and a
     * unit conversion that happens before the display has decided how many decimals it
     * wants is a conversion that can silently round the difference away. The phone
     * shows the nameplate to 1 decimal and the learned figure to 2; this must be able
     * to do the same, or the two screens quote different capacities for one pack.
     */
    uint32_t capacity_design_uah;  /* the nameplate, `soc.cap_uah` */
    uint32_t capacity_learned_uah; /* what the gauge has learned; 0 until it reports */
    /* How many times the monitor has measured capacity. 0 = the learned figure is the
     * nameplate copied, not a measurement, and must not be shown as one. -1 = the
     * monitor's firmware predates the key (before 0.11.7) and cannot say. */
    int      learn_count;
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
 * The monitor keeps SoC at a fixed interval in a ring of `capacity` points (`hist`,
 * firmware 0.9.0 and later); the remote fetches it on connect and every two minutes and
 * draws it. The interval is a setting on the monitor -- 5 minutes covering 24 h by
 * default, 10 covering 48 -- so the span comes from interval_s x capacity and is never
 * assumed here.
 */
#define LINK_HIST_MAX  288
#define LINK_HIST_NONE 0xFFFF

typedef struct {
    bool     supported;   /* false: the monitor's firmware has no `hist` */
    int      count;       /* points in pts[], oldest first */
    uint32_t interval_s;
    uint32_t capacity;    /* points the monitor's ring holds: span = interval_s x this */
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
