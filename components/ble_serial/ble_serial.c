/*
 * ble_serial.c — see ble_serial.h.
 *
 * Two structural decisions worth stating, because both are easy to get wrong and
 * expensive to debug:
 *
 * 1. RECEIVED BYTES ARE QUEUED, NOT EXECUTED IN PLACE. The GATT write callback runs
 *    on the NimBLE host task. Running a command there would block the host for as
 *    long as the command takes -- and `zero 512` is several seconds of blocking I2C.
 *    The link would drop mid-command, which looks exactly like a BLE bug and is not
 *    one. So the callback only pushes into a stream buffer; a worker task assembles
 *    lines and runs them.
 *
 * 2. OUTPUT IS CHUNKED TO THE NEGOTIATED MTU. A notification longer than ATT_MTU-3
 *    is silently truncated by the stack, so `help` would arrive with its tail
 *    missing. ble_serial_write() splits instead, and re-reads the MTU each call
 *    because it changes when the central negotiates after connecting.
 */

#include "ble_serial.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

/* Declared in NimBLE's store/config module but not exported through a public header
 * in ESP-IDF; every ESP-IDF NimBLE example forward-declares it the same way. Without
 * it, bonds live in RAM and evaporate on reboot, which defeats the point of bonding. */
void ble_store_config_init(void);

#define NVS_NS   "ble_ser"
#define NVS_KEY_MODE    "secmode"
#define NVS_KEY_PASSKEY "passkey"
#define PASSKEY_RANDOM  0xFFFFFFFFu

static const char *TAG = "ble_ser";

#define RX_BUF_BYTES  512
#define BLE_LINE_MAX  160
#define BLE_NAME_MAX  31

/* Nordic UART Service. The byte arrays are the UUIDs least-significant byte first,
 * which is what BLE_UUID128_DECLARE takes -- writing them in text order here is the
 * classic way to end up with a service nothing can discover. */
#define NUS_UUID_BASE(b0, b1)                                                       \
    BLE_UUID128_DECLARE(0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, \
                        0xA3, 0xB5, (b0), (b1), 0x40, 0x6E)

/* 6E400001-B5A3-F393-E0A9-E50E24DCCA9E and its two characteristics. */
#define NUS_SVC_UUID NUS_UUID_BASE(0x01, 0x00)
#define NUS_RX_UUID  NUS_UUID_BASE(0x02, 0x00) /* central -> device, write */
#define NUS_TX_UUID  NUS_UUID_BASE(0x03, 0x00) /* device -> central, notify */

static struct {
    char                 name[BLE_NAME_MAX + 1];
    ble_serial_line_cb_t on_line;
    void                *user;

    StreamBufferHandle_t rx;
    uint16_t             conn_handle;
    uint16_t             tx_val_handle;
    bool                 subscribed;
    bool                 advertising;
    uint8_t              addr_type;

    ble_serial_stats_t   stats;

    ble_sec_mode_t          mode;
    uint32_t                passkey_cfg;  /* PASSKEY_RANDOM or a fixed value */
    uint32_t                passkey_live; /* the one currently displayed */
    ble_serial_passkey_cb_t on_passkey;
} s_ble = {.conn_handle = BLE_HS_CONN_HANDLE_NONE,
           .passkey_cfg = PASSKEY_RANDOM};

static void advertise(void);

/* --- GATT ---------------------------------------------------------------------- */

static int gatt_rx_write(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    /*
     * Enforce the security mode here rather than through characteristic flags: the
     * mode is switchable at runtime and flags are fixed when the service registers.
     *
     * Both bits are required, not just encryption. An unauthenticated ("Just Works")
     * pairing is encrypted but gives no protection against an active man in the
     * middle, and this characteristic can run `zero` and `curve` -- commands that
     * silently corrupt a year of accumulated charge. Encryption alone is not the bar.
     */
    if (s_ble.mode == BLE_SEC_BONDED) {
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(conn_handle, &desc) != 0 ||
            !desc.sec_state.encrypted || !desc.sec_state.authenticated) {
            s_ble.stats.rejected++;
            return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
        }
    }

    /* Flatten the mbuf chain and push it on. Dropping the excess rather than
     * blocking is deliberate: this runs on the host task (see the file header), so
     * waiting here for a slow consumer would stall the whole BLE stack. A dropped
     * command line is recoverable by retyping; a stalled host is not. */
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    char     buf[128];

    while (len > 0) {
        const uint16_t take = len > sizeof(buf) ? (uint16_t)sizeof(buf) : len;
        if (ble_hs_mbuf_to_flat(ctxt->om, buf, take, NULL) != 0) {
            break;
        }
        const size_t sent = xStreamBufferSend(s_ble.rx, buf, take, 0);
        s_ble.stats.rx_bytes += sent;
        os_mbuf_adj(ctxt->om, take);
        len -= take;
    }
    return 0;
}

/*
 * TX is notify-only and nothing ever reads it -- but NimBLE requires an access
 * callback on EVERY characteristic: ble_gatts_count_resources() returns
 * BLE_HS_EINVAL (rc=3) for a NULL one, which surfaces as a service that fails to
 * register and a device that never advertises. Refuse the read explicitly rather
 * than hand back whatever the value handle points at.
 */
static int gatt_tx_access(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)ctxt; (void)arg;
    return BLE_ATT_ERR_READ_NOT_PERMITTED;
}

static const struct ble_gatt_chr_def nus_chrs[] = {
    {
        .uuid      = NUS_RX_UUID,
        .access_cb = gatt_rx_write,
        .flags     = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
    },
    {
        .uuid      = NUS_TX_UUID,
        .access_cb = gatt_tx_access,
        .val_handle = &s_ble.tx_val_handle,
        .flags     = BLE_GATT_CHR_F_NOTIFY,
    },
    {0},
};

static const struct ble_gatt_svc_def nus_svcs[] = {
    {
        .type            = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid            = NUS_SVC_UUID,
        .characteristics = nus_chrs,
    },
    {0},
};

/* --- GAP ----------------------------------------------------------------------- */

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_ble.conn_handle = event->connect.conn_handle;
            s_ble.advertising = false;
            ESP_LOGI(TAG, "connected, handle %u", s_ble.conn_handle);

            /* In BONDED mode ask for security immediately rather than waiting for
             * the first rejected write. A phone that is already bonded encrypts
             * silently and the user sees nothing; one that is not gets the pairing
             * prompt at the moment it makes sense, not after a confusing failure. */
            if (s_ble.mode == BLE_SEC_BONDED) {
                const int rc = ble_gap_security_initiate(s_ble.conn_handle);
                if (rc != 0 && rc != BLE_HS_EALREADY) {
                    ESP_LOGW(TAG, "security_initiate: %d", rc);
                }
            }
        } else {
            ESP_LOGW(TAG, "connect failed (%d), re-advertising",
                     event->connect.status);
            advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected (reason %d)", event->disconnect.reason);
        s_ble.conn_handle    = BLE_HS_CONN_HANDLE_NONE;
        s_ble.subscribed     = false;
        s_ble.passkey_live   = 0;
        /* Always come back up. A monitor that stops advertising after one session
         * is a monitor someone has to power-cycle in an awkward place. */
        advertise();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_ble.tx_val_handle) {
            s_ble.subscribed = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "notifications %s",
                     s_ble.subscribed ? "enabled" : "disabled");
            if (s_ble.subscribed) {
                /* A terminal app shows a blank screen until something arrives, so
                 * say hello: it proves the link works before anything is typed. */
                ble_serial_write("\r\nbat-monitor console over BLE. 'help' lists "
                                 "commands.\r\n", 0);
            }
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU %u", event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        /* io_cap is DISPLAY_ONLY, so the only action we can be asked for is DISP:
         * we choose the number, show it, and the phone types it back. That is what
         * makes the pairing authenticated (MITM-protected) rather than Just Works. */
        if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
            struct ble_sm_io io = {.action = BLE_SM_IOACT_DISP};

            /* A random passkey per pairing by default. esp_random() is fed by the
             * RF noise source once the radio is up, which it is by definition here.
             * The modulo bias across 2^32 / 1e6 is ~1e-4 of a digit and irrelevant
             * against a six-digit space used once. */
            io.passkey = (s_ble.passkey_cfg == PASSKEY_RANDOM)
                             ? (esp_random() % 1000000u)
                             : s_ble.passkey_cfg;
            s_ble.passkey_live = io.passkey;

            /* Log it as well as displaying it: on a board with no panel fitted the
             * USB console is the only way to read it, and this is a bench tool. */
            ESP_LOGW(TAG, "PAIRING passkey: %06lu", (unsigned long)io.passkey);
            if (s_ble.on_passkey) {
                s_ble.on_passkey(io.passkey, s_ble.user);
            }

            const int rc = ble_sm_inject_io(event->passkey.conn_handle, &io);
            if (rc != 0) {
                ESP_LOGE(TAG, "inject_io: %d", rc);
            }
        } else {
            ESP_LOGW(TAG, "unexpected passkey action %d -- refusing",
                     event->passkey.params.action);
        }
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE: {
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0) {
            ESP_LOGI(TAG, "encryption %s: encrypted=%d authenticated=%d bonded=%d",
                     event->enc_change.status == 0 ? "established" : "FAILED",
                     desc.sec_state.encrypted, desc.sec_state.authenticated,
                     desc.sec_state.bonded);
        }
        s_ble.passkey_live = 0;
        if (s_ble.on_passkey) {
            s_ble.on_passkey(0, s_ble.user); /* 0 = take the passkey off the screen */
        }
        return 0;
    }

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        /* The peer wants to pair again while we still hold a bond for it. Deleting
         * the old bond and allowing the new one is what every phone expects after
         * "forget this device" on its side; refusing leaves a link that can never
         * be re-established without physical access here. */
        ESP_LOGW(TAG, "repeat pairing -- replacing the stored bond");
        {
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
                ble_store_util_delete_peer(&desc.peer_id_addr);
            }
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        return 0;

    default:
        return 0;
    }
}

static void advertise(void)
{
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name  = (uint8_t *)s_ble.name;
    fields.name_len        = (uint8_t)strlen(s_ble.name);
    fields.name_is_complete = 1;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields: %d", rc);
        return;
    }

    /* The 128-bit NUS UUID goes in the scan response: a 16-byte UUID plus a name
     * does not fit in one 31-byte advertisement, and dropping the name is worse --
     * the name is how a person finds the right board. */
    struct ble_hs_adv_fields rsp = {0};
    ble_uuid128_t            svc = *(ble_uuid128_t *)NUS_SVC_UUID;
    rsp.uuids128            = &svc;
    rsp.num_uuids128        = 1;
    rsp.uuids128_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGW(TAG, "adv_rsp_set_fields: %d (continuing without it)", rc);
    }

    struct ble_gap_adv_params adv = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        /* 500 ms, matching the T0/T1 advertising interval of DESIGN.md §9.5.
         * Units are 0.625 ms. */
        .itvl_min = 800,
        .itvl_max = 800,
    };
    rc = ble_gap_adv_start(s_ble.addr_type, NULL, BLE_HS_FOREVER, &adv, gap_event,
                           NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_start: %d", rc);
        return;
    }
    s_ble.advertising = true;
    ESP_LOGI(TAG, "advertising as '%s'", s_ble.name);
}

/* --- host callbacks ------------------------------------------------------------ */

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure_addr: %d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_ble.addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "infer_auto: %d", rc);
        return;
    }
    advertise();
}

static void on_reset(int reason)
{
    /* DESIGN.md §10: a BLE fault must never stop the gauge. Log and let NimBLE come
     * back; nothing above this line depends on the link. */
    ESP_LOGW(TAG, "stack reset, reason %d", reason);
    s_ble.conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_ble.subscribed  = false;
    s_ble.advertising = false;
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* --- output -------------------------------------------------------------------- */

void ble_serial_write(const char *data, size_t n)
{
    if (!data) {
        return;
    }
    if (n == 0) {
        n = strlen(data);
    }
    if (!ble_serial_ready()) {
        s_ble.stats.dropped += n;
        return;
    }

    /* ATT_MTU includes the 3-byte notification header. Re-read it every call: the
     * central usually negotiates upward a moment after connecting, and caching the
     * initial 23 would cap every later notification at 20 bytes. */
    uint16_t mtu = ble_att_mtu(s_ble.conn_handle);
    if (mtu < 23) {
        mtu = 23;
    }
    const size_t chunk = mtu - 3;

    size_t off = 0;
    while (off < n) {
        const size_t take = (n - off) > chunk ? chunk : (n - off);

        struct os_mbuf *om = ble_hs_mbuf_from_flat(&data[off], (uint16_t)take);
        if (!om) {
            /* Out of mbufs: the central is not draining. Drop the rest rather than
             * spin -- console output is not worth stalling a task for. */
            s_ble.stats.dropped += (n - off);
            return;
        }
        const int rc = ble_gatts_notify_custom(s_ble.conn_handle,
                                               s_ble.tx_val_handle, om);
        if (rc != 0) {
            s_ble.stats.dropped += (n - off);
            return;
        }
        s_ble.stats.tx_bytes += take;
        off += take;
    }
}

bool ble_serial_ready(void)
{
    return s_ble.conn_handle != BLE_HS_CONN_HANDLE_NONE && s_ble.subscribed;
}

const char *ble_serial_name(void)
{
    return s_ble.name;
}

void ble_serial_get_stats(ble_serial_stats_t *out)
{
    if (!out) {
        return;
    }
    *out = s_ble.stats;
    out->advertising = s_ble.advertising;
    out->connected   = s_ble.conn_handle != BLE_HS_CONN_HANDLE_NONE;
    out->subscribed  = s_ble.subscribed;
    out->mtu         = out->connected ? ble_att_mtu(s_ble.conn_handle) : 0;
    out->mode        = s_ble.mode;

    if (out->connected) {
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(s_ble.conn_handle, &desc) == 0) {
            out->encrypted     = desc.sec_state.encrypted;
            out->authenticated = desc.sec_state.authenticated;
            out->bonded_peer   = desc.sec_state.bonded;
        }
    }

    int n = 0;
    if (ble_store_util_count(BLE_STORE_OBJ_TYPE_PEER_SEC, &n) == 0) {
        out->bonds = n;
    }
}

/* --- pairing ------------------------------------------------------------------- */

static void sec_persist(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_u8(h, NVS_KEY_MODE, (uint8_t)s_ble.mode);
    nvs_set_u32(h, NVS_KEY_PASSKEY, s_ble.passkey_cfg);
    nvs_commit(h);
    nvs_close(h);
}

static void sec_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return; /* first boot: defaults stand */
    }
    uint8_t  m = (uint8_t)s_ble.mode;
    uint32_t k = s_ble.passkey_cfg;
    if (nvs_get_u8(h, NVS_KEY_MODE, &m) == ESP_OK) {
        s_ble.mode = (m == (uint8_t)BLE_SEC_BONDED) ? BLE_SEC_BONDED : BLE_SEC_OPEN;
    }
    if (nvs_get_u32(h, NVS_KEY_PASSKEY, &k) == ESP_OK) {
        s_ble.passkey_cfg = k;
    }
    nvs_close(h);
}

esp_err_t ble_serial_set_sec_mode(ble_sec_mode_t mode)
{
    if (mode != BLE_SEC_OPEN && mode != BLE_SEC_BONDED) {
        return ESP_ERR_INVALID_ARG;
    }
    const bool tightening = (mode == BLE_SEC_BONDED && s_ble.mode == BLE_SEC_OPEN);
    s_ble.mode = mode;
    sec_persist();

    /* Tightening must not leave the existing link with the access it had under the
     * old rules. Dropping it forces the peer through the new path immediately, which
     * is also the only way the change is visible to the person who made it. */
    if (tightening && s_ble.conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_ble.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    return ESP_OK;
}

ble_sec_mode_t ble_serial_get_sec_mode(void)
{
    return s_ble.mode;
}

esp_err_t ble_serial_set_passkey(uint32_t passkey)
{
    if (passkey != PASSKEY_RANDOM && passkey > 999999u) {
        return ESP_ERR_INVALID_ARG;
    }
    s_ble.passkey_cfg = passkey;
    sec_persist();
    return ESP_OK;
}

uint32_t ble_serial_get_passkey(void)
{
    return s_ble.passkey_cfg;
}

int ble_serial_list_bonds(char out[][24], int max)
{
    ble_addr_t peers[8];
    int        num = 0;

    if (max <= 0) {
        return 0;
    }
    if (ble_store_util_bonded_peers(peers, &num,
                                    (int)(sizeof(peers) / sizeof(peers[0]))) != 0) {
        return -1;
    }
    if (num > max) {
        num = max;
    }
    for (int i = 0; i < num; i++) {
        snprintf(out[i], 24, "%02X:%02X:%02X:%02X:%02X:%02X", peers[i].val[5],
                 peers[i].val[4], peers[i].val[3], peers[i].val[2], peers[i].val[1],
                 peers[i].val[0]);
    }
    return num;
}

esp_err_t ble_serial_clear_bonds(void)
{
    /* ble_store_clear() drops the current encrypted link's keys too, so terminate
     * first: an encrypted connection whose keys have been deleted underneath it is a
     * connection in an undefined state. */
    if (s_ble.conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_ble.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    return ble_store_clear() == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t ble_serial_disconnect(void)
{
    if (s_ble.conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return ESP_ERR_INVALID_STATE;
    }
    return ble_gap_terminate(s_ble.conn_handle, BLE_ERR_REM_USER_CONN_TERM) == 0
               ? ESP_OK : ESP_FAIL;
}

/* --- worker: bytes to lines to commands ---------------------------------------- */

static void worker_task(void *arg)
{
    (void)arg;
    static char line[BLE_LINE_MAX];
    size_t      len = 0;

    for (;;) {
        char c;
        if (xStreamBufferReceive(s_ble.rx, &c, 1, portMAX_DELAY) != 1) {
            continue;
        }

        if (c == '\r' || c == '\n') {
            if (len == 0) {
                continue; /* bare newline: a terminal app sends CRLF */
            }
            line[len] = '\0';
            s_ble.stats.lines++;

            /* Echo the command back. A BLE terminal shows only what it received, so
             * without this the output has no visible connection to what was typed. */
            ble_serial_write("> ", 2);
            ble_serial_write(line, len);
            ble_serial_write("\r\n", 2);

            if (s_ble.on_line) {
                s_ble.on_line(line, s_ble.user);
            }
            len = 0;
            continue;
        }

        if (c == 0x08 || c == 0x7F) { /* backspace, for the interactive terminals */
            if (len > 0) {
                len--;
            }
            continue;
        }

        if (len < sizeof(line) - 1) {
            line[len++] = c;
        } else {
            /* Overlong line: drop it whole rather than execute a truncated command.
             * A silently shortened command is how you end up running `zero 5`
             * instead of `zero 512`. */
            ble_serial_write("\r\nline too long, discarded\r\n", 0);
            len = 0;
        }
    }
}

/* --- start --------------------------------------------------------------------- */

esp_err_t ble_serial_start(const ble_serial_config_t *cfg)
{
    if (!cfg || !cfg->device_name) {
        return ESP_ERR_INVALID_ARG;
    }

    s_ble.on_line    = cfg->on_line;
    s_ble.on_passkey = cfg->on_passkey;
    s_ble.user       = cfg->user;

    snprintf(s_ble.name, sizeof(s_ble.name), "%s", cfg->device_name);
    if (cfg->append_mac) {
        uint8_t mac[6] = {0};
        if (esp_read_mac(mac, ESP_MAC_BT) == ESP_OK) {
            const size_t at = strlen(s_ble.name);
            snprintf(&s_ble.name[at], sizeof(s_ble.name) - at, "-%02X%02X", mac[4],
                     mac[5]);
        }
    }

    s_ble.rx = xStreamBufferCreate(RX_BUF_BYTES, 1);
    if (!s_ble.rx) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    /*
     * Security manager (DESIGN.md §8.5). Configured unconditionally, even in OPEN
     * mode: these settings only describe what happens IF a peer asks to pair, and a
     * peer may ask at any time. Enforcement -- whether an unpaired peer can actually
     * run commands -- is the mode check in gatt_rx_write(), not these flags.
     *
     * DISPLAY_ONLY with sm_mitm gives passkey-display pairing: the device picks six
     * digits, the phone types them, and the exchange is authenticated. sm_sc selects
     * LE Secure Connections (ECDH) over legacy pairing, which matters because legacy
     * passkey pairing is offline-crackable from a single sniffed exchange.
     */
    ble_hs_cfg.sm_io_cap         = BLE_HS_IO_DISPLAY_ONLY;
    ble_hs_cfg.sm_bonding        = 1;
    ble_hs_cfg.sm_mitm           = 1;
    ble_hs_cfg.sm_sc             = 1;
    ble_hs_cfg.sm_our_key_dist   = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.store_status_cb   = ble_store_util_status_rr;

    /* Bonds in NVS, so pairing survives a reboot. */
    ble_store_config_init();
    sec_load();

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(nus_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "count_cfg: %d", rc);
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(nus_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "add_svcs: %d", rc);
        return ESP_FAIL;
    }
    rc = ble_svc_gap_device_name_set(s_ble.name);
    if (rc != 0) {
        ESP_LOGW(TAG, "device_name_set: %d", rc);
    }

    /* Priority 4: above the display (3), below the sampler (6). The gauge outlives
     * everything else (DESIGN.md §10), and that ordering is how it does so. */
    if (xTaskCreate(worker_task, "ble_cmd", 6144, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "NUS console starting, name '%s', security %s", s_ble.name,
             s_ble.mode == BLE_SEC_BONDED ? "bonded" : "OPEN (no pairing)");
    if (s_ble.mode == BLE_SEC_OPEN) {
        ESP_LOGW(TAG, "anything in range can run console commands -- 'ble pair "
                      "bonded' to require pairing (DESIGN.md 8.5)");
    }
    return ESP_OK;
}
