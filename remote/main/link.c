/*
 * link.c — see link.h.
 *
 * THREE CONTEXTS. The NimBLE host task delivers every GAP and GATT event and the
 * notification bytes; it must never block. The link task runs the console
 * conversation -- one command at a time, each waiting for its 0x04 -- because that
 * waiting is exactly what the host task may not do. The UI task only reads snapshots
 * and calls the few link_* actions. The model and the found-list are the shared state,
 * behind one mutex held for microseconds.
 *
 * PAIRING. In `ble pair bonded` mode the monitor refuses writes to its command
 * characteristic from an unauthenticated link (ATT 0x05) and asks for security on
 * connect. The remote's IO capability is keyboard-only and the monitor's is display-
 * only, so LE Secure Connections chooses passkey entry: the monitor shows six digits
 * on its OLED and the remote asks for them on the touch keypad. The bond is kept in
 * NVS, so it happens once per board. A command refused for lack of authentication
 * starts pairing, and the handshake is simply run again once the link is encrypted.
 */

#include "link.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs.h"
#include "rcon.h"

/* Declared by NimBLE's store module but not exported in a public header. */
void ble_store_config_init(void);

static const char *TAG = "link";

#define NAME_PREFIX  "batmon"
#define MAX_FOUND    8
#define FOUND_TTL_US (30LL * 1000000)
#define NVS_NS       "remote"
#define NVS_PEER     "peer"

/* Nordic UART Service; bytes least significant first, as BLE_UUID128_INIT wants. */
#define NUS_UUID(b0) BLE_UUID128_INIT(0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, \
                                      0x93, 0xF3, 0xA3, 0xB5, (b0), 0x00, 0x40, 0x6E)
static const ble_uuid128_t UUID_SVC = NUS_UUID(0x01);
static const ble_uuid128_t UUID_RX  = NUS_UUID(0x02);
static const ble_uuid128_t UUID_TX  = NUS_UUID(0x03);

#define EV_READY BIT0 /* subscribed: the handshake may run */
#define EV_EOT   BIT1 /* the command in flight has been answered */
#define EV_KICK  BIT2 /* something changed; re-evaluate */

typedef struct {
    uint8_t addr[6];
    uint8_t type;
    char    name[24];
} peer_t;

static struct {
    SemaphoreHandle_t  lock;
    EventGroupHandle_t ev;
    uint8_t            own_addr_type;

    /* The remembered board. */
    bool   have_peer;
    peer_t peer;
    bool   take_first; /* nothing remembered yet: connect to the first board seen */

    /* The link. */
    uint16_t conn;
    bool     connecting;
    peer_t   target;     /* what we are connecting / connected to */
    uint16_t svc_end;
    uint16_t rx_handle, tx_handle, cccd_handle;
    bool     need_handshake;
    bool     pending;     /* a command is awaiting its 0x04 */
    bool     browse;      /* keep scanning while connected, for the Devices screen */

    /* Pairing. */
    bool     want_passkey;
    uint16_t pk_conn;

    /* A command from the UI, and what came of it. */
    bool       user_want;
    bool       user_active; /* capture this response into user_res */
    link_cmd_t user_res;

    /* SoC history: filled while a `hist` reply is parsed, published when it ends. */
    uint16_t    hist_tmp[LINK_HIST_MAX];
    int         hist_tmp_n;
    bool        hist_tmp_seen;
    uint32_t    hist_tmp_interval, hist_tmp_age;
    link_hist_t hist;
    int64_t     hist_fetched_us;

    link_model_t model;
    link_found_t found[MAX_FOUND];
    int64_t      found_seen[MAX_FOUND];

    char    line[256];
    size_t  line_len;
    int64_t last_fast_ms;
    uint32_t cap_design_mah, cap_learned_mah;
} s = {.conn = BLE_HS_CONN_HANDLE_NONE};

static void start_scan(void);
static int  gap_event(struct ble_gap_event *event, void *arg);

/* --- helpers -------------------------------------------------------------------- */

static void lock(void)   { xSemaphoreTake(s.lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s.lock); }

static void set_state(link_state_t st)
{
    lock();
    s.model.state = st;
    unlock();
    xEventGroupSetBits(s.ev, EV_KICK);
}

static void note(const char *msg)
{
    lock();
    snprintf(s.model.note, sizeof(s.model.note), "%s", msg);
    unlock();
    ESP_LOGI(TAG, "%s", msg);
}

static void peer_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, NVS_PEER, &s.peer, sizeof(s.peer));
        nvs_commit(h);
        nvs_close(h);
    }
}

static void peer_load(void)
{
    nvs_handle_t h;
    size_t       len = sizeof(s.peer);
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        s.have_peer = nvs_get_blob(h, NVS_PEER, &s.peer, &len) == ESP_OK &&
                      len == sizeof(s.peer);
        nvs_close(h);
    }
    s.take_first = !s.have_peer;
}

static void reset_link_model(void)
{
    lock();
    s.model.have_fast = s.model.have_calc = false;
    s.model.protocol  = 0;
    s.model.secure    = false;
    s.model.firmware[0] = '\0';
    s.model.chem[0]     = '\0';
    s.model.have_env    = false;
    s.hist.count        = 0;     /* another board: its history, not the last one's */
    s.hist.supported    = false;
    s.hist.seq++;
    s.model.cells       = 0;
    s.model.capacity_mah = 0;
    unlock();
    s.cap_design_mah = s.cap_learned_mah = 0;
    s.last_fast_ms   = 0;
    s.line_len       = 0;
    s.pending        = false;
}

/* --- the console stream ---------------------------------------------------------- */

/* Splits a CSV line in place; returns the field count. */
static int split(char *line, char **f, int max)
{
    int n = 0;
    char *p = line;
    while (n < max) {
        f[n++] = p;
        char *c = strchr(p, ',');
        if (!c) break;
        *c = '\0';
        p = c + 1;
    }
    return n;
}

static void on_line(char *line)
{
    char *f[10];

    if (line[0] == 'f' && line[1] == ',') {
        /* f,ms,volts,amps */
        if (split(line, f, 10) < 4) return;
        const int64_t ms   = strtoll(f[1], NULL, 10);
        const float   amps = strtof(f[3], NULL);
        lock();
        s.model.volts = strtof(f[2], NULL);
        s.model.amps  = amps;
        const int64_t dt = ms - s.last_fast_ms;
        if (!s.model.have_fast || dt <= 0 || dt > 10000) {
            s.model.amps_avg = amps; /* first sample, or a gap: restart the average */
        } else {
            const float a = (float)dt / (60000.0f + (float)dt); /* ~60 s time constant */
            s.model.amps_avg += a * (amps - s.model.amps_avg);
        }
        s.last_fast_ms        = ms;
        s.model.have_fast     = true;
        s.model.last_data_us  = esp_timer_get_time();
        unlock();
        return;
    }
    if (line[0] == 'c' && line[1] == ',') {
        /* c,ms,watts,soc_pct,charge_ah,state,ocv_v,peukert */
        if (split(line, f, 10) < 6) return;
        lock();
        s.model.watts     = strtof(f[2], NULL);
        s.model.soc_pct   = strtof(f[3], NULL);
        s.model.charge_ah = strtof(f[4], NULL);
        snprintf(s.model.mode, sizeof(s.model.mode), "%s", f[5]);
        s.model.have_calc    = true;
        s.model.last_data_us = esp_timer_get_time();
        unlock();
        return;
    }
    if (line[0] == 'e' && line[1] == ',') {
        /* e,ms,temp_c,humid_pct,press_hpa -- fields are EMPTY, not zero, when the
         * sensor lacks them (CLI.md §5), so an empty one must not read as 0. */
        if (split(line, f, 10) < 5) return;
        lock();
        s.model.temp_c    = f[2][0] ? strtof(f[2], NULL) : NAN;
        s.model.humid_pct = f[3][0] ? strtof(f[3], NULL) : NAN;
        s.model.press_hpa = f[4][0] ? strtof(f[4], NULL) : NAN;
        s.model.have_env  = f[2][0] || f[3][0] || f[4][0];
        unlock();
        return;
    }
    /* Other records (d) and headers: skip, as CLI.md asks of any client. */
    if (line[0] == 'd' && line[1] == ',') return;
    if (line[0] == '#') return;
    if (!s.pending) return; /* greeting and anything unsolicited */

    if (s.user_active) {
        /* A UI command: keep its exit status and its last few lines, verbatim. The
         * refusals name a physical cause, and that is what the user needs to read. */
        lock();
        if (strncmp(line, "exit ", 5) == 0) {
            s.user_res.exit = atoi(line + 5);
        } else if (line[0] && strncmp(line, "> ", 2) != 0) {
            memmove(s.user_res.reply[0], s.user_res.reply[1], sizeof(s.user_res.reply[0]) * 2);
            snprintf(s.user_res.reply[2], sizeof(s.user_res.reply[2]), "%s", line);
        }
        unlock();
        return;
    }

    /* A command's response: `ver` (key value) and `config` (key=value). */
    char *eq = strchr(line, '=');
    lock();
    if (eq) {
        *eq = '\0';
        const char *k = line, *v = eq + 1;
        if (strcmp(k, "soc") == 0) {
            /* `hist`: comma-separated permille, `-` for a gap, split over lines. */
            char *save = NULL;
            for (char *t = strtok_r((char *)v, ",", &save); t && s.hist_tmp_n < LINK_HIST_MAX;
                 t = strtok_r(NULL, ",", &save)) {
                s.hist_tmp[s.hist_tmp_n++] = (t[0] == '-') ? LINK_HIST_NONE : (uint16_t)atoi(t);
            }
        } else if (strcmp(k, "points") == 0) {
            s.hist_tmp_seen = true;
        } else if (strcmp(k, "interval_s") == 0) {
            s.hist_tmp_interval = strtoul(v, NULL, 10);
        } else if (strcmp(k, "age_s") == 0) {
            s.hist_tmp_age = strtoul(v, NULL, 10);
        } else if (strcmp(k, "soc.cap_uah") == 0) {
            s.cap_design_mah = strtoul(v, NULL, 10) / 1000;
        } else if (strcmp(k, "soc.learned_uah") == 0) {
            s.cap_learned_mah = strtoul(v, NULL, 10) / 1000;
        } else if (strcmp(k, "battery.chem") == 0) {
            snprintf(s.model.chem, sizeof(s.model.chem), "%s", v);
        } else if (strcmp(k, "battery.cells") == 0) {
            s.model.cells = atoi(v);
        }
        s.model.capacity_mah = s.cap_learned_mah ? s.cap_learned_mah : s.cap_design_mah;
    } else if (strncmp(line, "protocol ", 9) == 0) {
        s.model.protocol = atoi(line + 9);
    } else if (strncmp(line, "firmware ", 9) == 0) {
        snprintf(s.model.firmware, sizeof(s.model.firmware), "%s", line + 9);
    }
    unlock();
}

static void feed(const uint8_t *data, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        const char c = (char)data[i];
        if (c == 0x04) {
            s.pending = false;
            xEventGroupSetBits(s.ev, EV_EOT);
        } else if (c == '\n') {
            s.line[s.line_len] = '\0';
            on_line(s.line);
            s.line_len = 0;
        } else if (c != '\r' && s.line_len < sizeof(s.line) - 1) {
            s.line[s.line_len++] = c;
        }
    }
}

static int on_write(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr,
                    void *arg)
{
    if (err->status == BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_AUTHEN) ||
        err->status == BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_ENC)) {
        /* The board is in bonded mode: pair, then run the handshake again. */
        note("board requires pairing");
        s.need_handshake = true;
        ble_gap_security_initiate(conn);
    } else if (err->status != 0) {
        ESP_LOGW(TAG, "command write failed: %d", err->status);
    }
    return 0;
}

/* Fetches `hist` and publishes it. Runs on the link task only. */
static bool run(const char *cmd, int timeout_ms);

static void fetch_history(void)
{
    s.hist_tmp_n        = 0;
    s.hist_tmp_seen     = false;
    s.hist_tmp_interval = 600;
    s.hist_tmp_age      = 0;
    const bool got = run("hist", 4000);
    lock();
    if (got) {
        /* No `points=` in the answer: a monitor from before 0.9.0, which says
         * "unknown command". */
        s.hist.supported = s.hist_tmp_seen;
        if (s.hist_tmp_seen) {
            const bool changed = s.hist.count != s.hist_tmp_n ||
                                 memcmp(s.hist.pts, s.hist_tmp, s.hist_tmp_n * 2) != 0;
            memcpy(s.hist.pts, s.hist_tmp, s.hist_tmp_n * 2);
            s.hist.count      = s.hist_tmp_n;
            s.hist.interval_s = s.hist_tmp_interval;
            s.hist.age_s      = s.hist_tmp_age;
            s.hist_fetched_us = esp_timer_get_time();
            if (changed) s.hist.seq++;
        }
    }
    unlock();
}

/* One command, waiting for its terminator. Runs on the link task only. */
static bool run(const char *cmd, int timeout_ms)
{
    if (s.conn == BLE_HS_CONN_HANDLE_NONE || !s.rx_handle) {
        return false;
    }
    char buf[64];
    const int n = snprintf(buf, sizeof(buf), "%s\n", cmd);
    xEventGroupClearBits(s.ev, EV_EOT);
    s.pending = true;
    if (ble_gattc_write_flat(s.conn, s.rx_handle, buf, n, on_write, NULL) != 0) {
        s.pending = false;
        return false;
    }
    lock();
    s.model.tx_packets++;
    unlock();
    const EventBits_t b =
        xEventGroupWaitBits(s.ev, EV_EOT, pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    s.pending = false;
    return (b & EV_EOT) != 0;
}

static void link_task(void *arg)
{
    int64_t last_config = 0;
    for (;;) {
        /* EV_READY stays set for the whole connection, so it cannot be what this
         * waits on -- the wait would return at once, forever. Wait for a kick, or a
         * second to pass for the periodic `config`. */
        xEventGroupWaitBits(s.ev, EV_KICK, pdTRUE, pdFALSE, pdMS_TO_TICKS(1000));
        if (!(xEventGroupGetBits(s.ev) & EV_READY) || s.want_passkey) {
            continue;
        }
        if (s.need_handshake) {
            /* CLI.md §7: identify, read the settings worth showing, start telemetry.
             * Only `ver` succeeding ends the handshake; a refusal for lack of pairing
             * leaves it pending until the link is encrypted. */
            if (run("ver", 3000) && s.model.protocol != 0) {
                run("config", 5000);
                run("stream csv", 3000);
                fetch_history();
                s.need_handshake = false;
                last_config      = esp_timer_get_time();
                set_state(LINK_READY);
                note("connected");
                if (!s.have_peer) {
                    /* First board ever: remember it, as the phone app does. */
                    s.peer       = s.target;
                    s.have_peer  = true;
                    s.take_first = false;
                    peer_save();
                }
            }
        } else if (s.user_want) {
            s.user_want = false;
            lock();
            s.user_res.busy       = true;
            s.user_res.done       = false;
            s.user_res.exit       = -1;
            s.user_res.started_us = esp_timer_get_time();
            memset(s.user_res.reply, 0, sizeof(s.user_res.reply));
            char cmd[48];
            snprintf(cmd, sizeof(cmd), "%s", s.user_res.cmd);
            const int timeout = s.user_res.timeout_ms;
            unlock();

            s.user_active = true;
            const bool got = run(cmd, timeout);
            s.user_active = false;

            lock();
            s.user_res.busy     = false;
            s.user_res.done     = true;
            s.user_res.answered = got && s.conn != BLE_HS_CONN_HANDLE_NONE;
            unlock();
            ESP_LOGI(TAG, "%s -> %s, exit %d", cmd, got ? "answered" : "no answer",
                     s.user_res.exit);
        } else if (esp_timer_get_time() - last_config > 300LL * 1000000) {
            /* Capacity is learned over time; re-read it now and then. */
            run("config", 5000);
            last_config = esp_timer_get_time();
        } else if (s.hist.supported &&
                   esp_timer_get_time() - s.hist_fetched_us > 120LL * 1000000) {
            /* A new point every 10 minutes; checking every 2 costs ~1 KB. */
            fetch_history();
        }
    }
}

/* --- discovery ----------------------------------------------------------------------- */

static int on_cccd(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *a,
                   void *arg)
{
    if (err->status != 0) {
        note("could not subscribe");
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }
    s.need_handshake = true;
    xEventGroupSetBits(s.ev, EV_READY | EV_KICK);
    return 0;
}

static int on_dsc(uint16_t conn, const struct ble_gatt_error *err, uint16_t chr_val,
                  const struct ble_gatt_dsc *dsc, void *arg)
{
    if (err->status == 0) {
        if (!s.cccd_handle && ble_uuid_u16(&dsc->uuid.u) == BLE_GATT_DSC_CLT_CFG_UUID16) {
            s.cccd_handle = dsc->handle;
        }
        return 0;
    }
    if (err->status == BLE_HS_EDONE && s.cccd_handle) {
        static const uint8_t on[2] = {1, 0};
        ble_gattc_write_flat(conn, s.cccd_handle, on, sizeof(on), on_cccd, NULL);
    } else {
        note("board has no notify descriptor");
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    return 0;
}

static int on_chr(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_chr *chr,
                  void *arg)
{
    if (err->status == 0) {
        if (ble_uuid_cmp(&chr->uuid.u, &UUID_RX.u) == 0) s.rx_handle = chr->val_handle;
        if (ble_uuid_cmp(&chr->uuid.u, &UUID_TX.u) == 0) s.tx_handle = chr->val_handle;
        return 0;
    }
    if (err->status == BLE_HS_EDONE && s.rx_handle && s.tx_handle) {
        ble_gattc_disc_all_dscs(conn, s.tx_handle, s.svc_end, on_dsc, NULL);
    } else {
        note("not a batmon console");
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    return 0;
}

static int on_svc(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_svc *svc,
                  void *arg)
{
    static uint16_t start;
    if (err->status == 0) {
        start     = svc->start_handle;
        s.svc_end = svc->end_handle;
        return 0;
    }
    if (err->status == BLE_HS_EDONE && s.svc_end) {
        ble_gattc_disc_all_chrs(conn, start, s.svc_end, on_chr, NULL);
    } else {
        note("no UART service on that board");
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    return 0;
}

static int on_mtu(uint16_t conn, const struct ble_gatt_error *err, uint16_t mtu, void *arg)
{
    ble_gattc_disc_svc_by_uuid(conn, &UUID_SVC.u, on_svc, NULL);
    return 0;
}

/* --- GAP ------------------------------------------------------------------------------ */

static void connect_to(const ble_addr_t *addr, const char *name)
{
    ble_gap_disc_cancel();
    memcpy(s.target.addr, addr->val, 6);
    s.target.type = addr->type;
    snprintf(s.target.name, sizeof(s.target.name), "%s", name);
    lock();
    snprintf(s.model.name, sizeof(s.model.name), "%s", name);
    unlock();

    const int rc = ble_gap_connect(s.own_addr_type, addr, 10000, NULL, gap_event, NULL);
    if (rc == 0) {
        s.connecting = true;
        set_state(LINK_CONNECTING);
    } else {
        ESP_LOGW(TAG, "connect failed: %d", rc);
        start_scan();
    }
}

static void found_update(const ble_addr_t *addr, const char *name, int8_t rssi)
{
    lock();
    int slot = -1, oldest = 0;
    for (int i = 0; i < MAX_FOUND; i++) {
        if (s.found_seen[i] && memcmp(s.found[i].addr, addr->val, 6) == 0) {
            slot = i;
            break;
        }
        if (s.found_seen[i] < s.found_seen[oldest]) oldest = i;
    }
    if (slot < 0) slot = oldest;
    link_found_t *f = &s.found[slot];
    memcpy(f->addr, addr->val, 6);
    f->addr_type = addr->type;
    if (name) snprintf(f->name, sizeof(f->name), "%s", name);
    f->rssi = rssi;
    s.found_seen[slot] = esp_timer_get_time();
    unlock();
}

static void on_adv(const struct ble_gap_disc_desc *d)
{
    struct ble_hs_adv_fields fields;
    char name[24] = "";
    if (ble_hs_adv_parse_fields(&fields, d->data, d->length_data) == 0 && fields.name) {
        const int n = fields.name_len < sizeof(name) - 1 ? fields.name_len : sizeof(name) - 1;
        memcpy(name, fields.name, n);
        name[n] = '\0';
    }
    /* The name is in the advertisement, the service UUID in the scan response (it
     * does not fit alongside): a response without a name refreshes a known entry. */
    if (name[0] && strncmp(name, NAME_PREFIX, strlen(NAME_PREFIX)) != 0) {
        return;
    }
    /* Another remote display in update mode advertises as batmon-remote-XXXX: it is
     * not a monitor, and connecting to it would find no telemetry. */
    if (strncmp(name, "batmon-remote", 13) == 0) {
        return;
    }
    if (!name[0]) {
        bool known = false;
        lock();
        for (int i = 0; i < MAX_FOUND; i++) {
            known |= s.found_seen[i] && memcmp(s.found[i].addr, d->addr.val, 6) == 0;
        }
        unlock();
        if (!known) return;
    }
    found_update(&d->addr, name[0] ? name : NULL, d->rssi);

    if (s.conn != BLE_HS_CONN_HANDLE_NONE || s.connecting || !name[0]) {
        return;
    }
    const bool is_peer = s.have_peer && memcmp(s.peer.addr, d->addr.val, 6) == 0;
    if (is_peer || s.take_first) {
        connect_to(&d->addr, name);
    }
}

static void start_scan(void)
{
    if (ble_gap_disc_active()) {
        return;
    }
    const struct ble_gap_disc_params p = {
        .itvl              = 0x50,  /* 50 ms */
        .window            = 0x30,  /* 30 ms */
        .filter_duplicates = 0,     /* RSSI updates for the Devices list */
        .passive           = 0,     /* the scan response is part of the picture */
    };
    const int rc = ble_gap_disc(s.own_addr_type, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "scan failed: %d", rc);
    }
    if (s.conn == BLE_HS_CONN_HANDLE_NONE && !s.connecting) {
        lock();
        if (s.have_peer) snprintf(s.model.name, sizeof(s.model.name), "%s", s.peer.name);
        unlock();
        set_state(LINK_SCANNING);
    }
}

static int gap_event(struct ble_gap_event *e, void *arg)
{
    struct ble_gap_conn_desc desc;

    switch (e->type) {
    case BLE_GAP_EVENT_DISC:
        on_adv(&e->disc);
        return 0;

    case BLE_GAP_EVENT_DISC_COMPLETE:
        return 0;

    case BLE_GAP_EVENT_CONNECT:
        s.connecting = false;
        if (e->connect.status != 0) {
            note("board did not answer");
            start_scan();
            return 0;
        }
        s.conn        = e->connect.conn_handle;
        s.svc_end     = s.rx_handle = s.tx_handle = s.cccd_handle = 0;
        reset_link_model();
        set_state(LINK_SETUP);
        ble_gattc_exchange_mtu(s.conn, on_mtu, NULL);
        if (s.browse) start_scan();
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected, reason 0x%x", e->disconnect.reason);
        s.conn         = BLE_HS_CONN_HANDLE_NONE;
        s.want_passkey = false;
        xEventGroupClearBits(s.ev, EV_READY);
        xEventGroupSetBits(s.ev, EV_EOT); /* release a command waiting on a dead link */
        note("link lost; looking for the board");
        start_scan();
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX:
        if (e->notify_rx.attr_handle == s.tx_handle) {
            lock();
            s.model.rx_packets++;
            unlock();
            uint8_t        buf[520];
            const uint16_t n = OS_MBUF_PKTLEN(e->notify_rx.om);
            const uint16_t take = n < sizeof(buf) ? n : sizeof(buf);
            if (ble_hs_mbuf_to_flat(e->notify_rx.om, buf, take, NULL) == 0) {
                feed(buf, take);
            }
        }
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        if (ble_gap_conn_find(e->enc_change.conn_handle, &desc) == 0) {
            lock();
            s.model.secure = desc.sec_state.encrypted && desc.sec_state.authenticated;
            unlock();
        }
        s.want_passkey = false;
        if (e->enc_change.status == 0) {
            note("paired");
            s.need_handshake = true;
            set_state(LINK_SETUP);
        } else {
            note("pairing failed -- wrong code?");
            set_state(LINK_SETUP);
        }
        xEventGroupSetBits(s.ev, EV_KICK);
        return 0;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        if (e->passkey.params.action == BLE_SM_IOACT_INPUT) {
            s.pk_conn      = e->passkey.conn_handle;
            s.want_passkey = true;
            set_state(LINK_PAIRING);
        } else if (e->passkey.params.action == BLE_SM_IOACT_NUMCMP) {
            struct ble_sm_io io = {.action = BLE_SM_IOACT_NUMCMP, .numcmp_accept = 1};
            ble_sm_inject_io(e->passkey.conn_handle, &io);
        }
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        /* The board forgot us (`ble unpair`) but we kept its bond: drop ours and pair
         * afresh rather than failing until someone clears it by hand. */
        if (ble_gap_conn_find(e->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU %u", e->mtu.value);
        return 0;

    default:
        return 0;
    }
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s.own_addr_type);
    start_scan();
    rcon_on_sync();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "BLE host reset, reason %d", reason);
}

static void host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* --- public ---------------------------------------------------------------------------- */

void link_start(void)
{
    s.lock = xSemaphoreCreateMutex();
    s.ev   = xEventGroupCreate();
    peer_load();
    if (s.have_peer) {
        snprintf(s.model.name, sizeof(s.model.name), "%s", s.peer.name);
    }

    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb         = on_sync;
    ble_hs_cfg.reset_cb        = on_reset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    ble_hs_cfg.sm_io_cap         = BLE_SM_IO_CAP_KEYBOARD_ONLY;
    ble_hs_cfg.sm_bonding        = 1;
    ble_hs_cfg.sm_mitm           = 1;
    ble_hs_cfg.sm_sc             = 1;
    ble_hs_cfg.sm_our_key_dist   = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    /* The remote is also a peripheral, for its own firmware updates (rcon.c).
     * Services can only be added before the host starts. */
    rcon_register();
    ble_store_config_init();

    xTaskCreate(link_task, "link", 4096, NULL, 5, NULL);
    nimble_port_freertos_init(host_task);
}

void link_get(link_model_t *out)
{
    lock();
    *out = s.model;
    unlock();
}

int link_found(link_found_t *out, int max)
{
    const int64_t now = esp_timer_get_time();
    int n = 0;
    lock();
    for (int i = 0; i < MAX_FOUND && n < max; i++) {
        if (!s.found_seen[i] || now - s.found_seen[i] > FOUND_TTL_US || !s.found[i].name[0]) {
            continue;
        }
        out[n] = s.found[i];
        out[n].saved     = s.have_peer && memcmp(s.peer.addr, s.found[i].addr, 6) == 0;
        out[n].connected = s.conn != BLE_HS_CONN_HANDLE_NONE &&
                           memcmp(s.target.addr, s.found[i].addr, 6) == 0;
        n++;
    }
    unlock();
    /* Strongest first. */
    for (int i = 1; i < n; i++) {
        for (int j = i; j > 0 && out[j].rssi > out[j - 1].rssi; j--) {
            const link_found_t t = out[j]; out[j] = out[j - 1]; out[j - 1] = t;
        }
    }
    return n;
}

void link_browse(bool on)
{
    s.browse = on;
    if (on) start_scan();
    else if (s.conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_disc_cancel();
}

void link_select(const link_found_t *d)
{
    memcpy(s.peer.addr, d->addr, 6);
    s.peer.type = d->addr_type;
    snprintf(s.peer.name, sizeof(s.peer.name), "%s", d->name);
    s.have_peer  = true;
    s.take_first = false;
    peer_save();
    lock();
    snprintf(s.model.name, sizeof(s.model.name), "%s", d->name);
    unlock();

    if (s.conn != BLE_HS_CONN_HANDLE_NONE) {
        if (memcmp(s.target.addr, d->addr, 6) == 0) return; /* already on it */
        /* The disconnect event restarts the scan, which finds the new peer. */
        ble_gap_terminate(s.conn, BLE_ERR_REM_USER_CONN_TERM);
    } else if (!s.connecting) {
        const ble_addr_t a = {.type = d->addr_type, .val = {d->addr[0], d->addr[1], d->addr[2],
                                                            d->addr[3], d->addr[4], d->addr[5]}};
        connect_to(&a, d->name);
    }
}

void link_forget(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, NVS_PEER);
        nvs_commit(h);
        nvs_close(h);
    }
    s.have_peer  = false;
    s.take_first = false; /* after an explicit forget, wait to be told which board */
    lock();
    s.model.name[0] = '\0';
    unlock();
    if (s.conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s.conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    ble_store_clear();
    note("forgotten; pick a board");
}

void link_history(link_hist_t *out)
{
    lock();
    *out = s.hist;
    if (s.hist.count) {
        out->age_s += (uint32_t)((esp_timer_get_time() - s.hist_fetched_us) / 1000000);
    }
    unlock();
}

bool link_command(const char *cmd, int timeout_ms)
{
    lock();
    const bool ok = s.model.state == LINK_READY && !s.user_want && !s.user_res.busy;
    if (ok) {
        snprintf(s.user_res.cmd, sizeof(s.user_res.cmd), "%s", cmd);
        s.user_res.timeout_ms = timeout_ms;
        s.user_res.done       = false;
        s.user_want           = true;
    }
    unlock();
    if (ok) xEventGroupSetBits(s.ev, EV_KICK);
    return ok;
}

void link_command_status(link_cmd_t *out)
{
    lock();
    *out = s.user_res;
    unlock();
}

bool link_passkey_wanted(char *who, size_t n)
{
    if (!s.want_passkey) return false;
    lock();
    snprintf(who, n, "%s", s.model.name);
    unlock();
    return true;
}

void link_passkey_submit(uint32_t passkey)
{
    if (!s.want_passkey) return;
    s.want_passkey = false;
    struct ble_sm_io io = {.action = BLE_SM_IOACT_INPUT, .passkey = passkey};
    ble_sm_inject_io(s.pk_conn, &io);
    set_state(LINK_SETUP);
}

void link_passkey_cancel(void)
{
    if (!s.want_passkey) return;
    s.want_passkey = false;
    ble_gap_terminate(s.pk_conn, BLE_ERR_REM_USER_CONN_TERM);
}
