/*
 * ble.c — see ble.h.
 *
 * Three structural decisions, each of which is easy to get wrong and expensive to
 * debug:
 *
 * 1. RECEIVED BYTES ARE QUEUED, NOT EXECUTED IN PLACE. The GATT write callback runs on
 *    the NimBLE host task. Running a command there would block the host for as long as
 *    the command takes -- and `cal zero i 512` is seventy seconds of blocking I2C. The
 *    link would drop mid-command, which looks exactly like a BLE bug and is not one.
 *
 * 2. LINE ASSEMBLY IS PER CONNECTION. Two centrals writing at once into one buffer
 *    produce a spliced command. The failure is intermittent, depends on timing, and
 *    gets blamed on the radio. A buffer per slot removes the possibility.
 *
 * 3. RESPONSES ARE UNICAST, TELEMETRY IS BROADCAST. The queue carries the originating
 *    connection handle so a reply goes back to the client that asked. Only the stream
 *    goes to everyone.
 */

#include "ble.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

/* Declared in NimBLE's store/config module but not exported through a public header in
 * ESP-IDF; every ESP-IDF NimBLE example forward-declares it the same way. Without it,
 * bonds live in RAM and evaporate on reboot, which defeats the point of bonding. */
void ble_store_config_init(void);

#define NVS_NS          "ble_ser"
#define NVS_KEY_MODE    "secmode"
#define NVS_KEY_PASSKEY "passkey"
#define PASSKEY_RANDOM  0xFFFFFFFFu

static const char *TAG = "ble_ser";

#define BLE_LINE_MAX  160
#define BLE_NAME_MAX  31
#define CMD_QUEUE_LEN 4

/* Nordic UART Service. The byte arrays are the UUIDs least-significant byte first,
 * which is what BLE_UUID128_DECLARE takes -- writing them in text order here is the
 * classic way to end up with a service nothing can discover. */
#define NUS_UUID_BASE(b0, b1)                                                       \
    BLE_UUID128_DECLARE(0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, \
                        0xA3, 0xB5, (b0), (b1), 0x40, 0x6E)

#define NUS_SVC_UUID NUS_UUID_BASE(0x01, 0x00)
#define NUS_RX_UUID  NUS_UUID_BASE(0x02, 0x00) /* central -> device, write */
#define NUS_TX_UUID  NUS_UUID_BASE(0x03, 0x00) /* device -> central, notify */
/* Not part of Nordic's NUS: firmware-update data (CLI.md §6, "Firmware update"). Same
 * base UUID, next number, inside the same service so a client discovers it in the same
 * pass; generic NUS terminals ignore a characteristic they do not know. */
#define NUS_OTA_UUID NUS_UUID_BASE(0x04, 0x00) /* central -> device, write with response */

typedef struct {
    uint16_t handle;
    bool     subscribed;
    char     line[BLE_LINE_MAX];
    size_t   len;
} conn_slot_t;

typedef struct {
    uint16_t handle;
    char     line[BLE_LINE_MAX];
} cmd_msg_t;

static struct {
    char                 name[BLE_NAME_MAX + 1];
    ble_line_cb_t on_line;
    ble_ota_cb_t         on_ota;
    void                *user;

    conn_slot_t     conns[BLE_MAX_CONNS];
    QueueHandle_t   cmdq;
    uint16_t        tx_val_handle;
    bool            advertising;
    uint8_t         addr_type;

    ble_stats_t stats;

    ble_sec_mode_t          mode;
    uint32_t                passkey_cfg;
    ble_passkey_cb_t on_passkey;
} s_ble = {.passkey_cfg = PASSKEY_RANDOM};

static void advertise(void);

/* --- connection slots ---------------------------------------------------------- */

static conn_slot_t *slot_by_handle(uint16_t h)
{
    for (int i = 0; i < BLE_MAX_CONNS; i++) {
        if (s_ble.conns[i].handle == h) {
            return &s_ble.conns[i];
        }
    }
    return NULL;
}

static conn_slot_t *slot_alloc(uint16_t h)
{
    for (int i = 0; i < BLE_MAX_CONNS; i++) {
        if (s_ble.conns[i].handle == BLE_HS_CONN_HANDLE_NONE) {
            s_ble.conns[i].handle     = h;
            s_ble.conns[i].subscribed = false;
            s_ble.conns[i].len        = 0;
            return &s_ble.conns[i];
        }
    }
    return NULL;
}

static void slot_free(uint16_t h)
{
    conn_slot_t *s = slot_by_handle(h);
    if (s) {
        s->handle     = BLE_HS_CONN_HANDLE_NONE;
        s->subscribed = false;
        s->len        = 0;
    }
}

static int conn_count(void)
{
    int n = 0;
    for (int i = 0; i < BLE_MAX_CONNS; i++) {
        if (s_ble.conns[i].handle != BLE_HS_CONN_HANDLE_NONE) {
            n++;
        }
    }
    return n;
}

static int sub_count(void)
{
    int n = 0;
    for (int i = 0; i < BLE_MAX_CONNS; i++) {
        if (s_ble.conns[i].handle != BLE_HS_CONN_HANDLE_NONE &&
            s_ble.conns[i].subscribed) {
            n++;
        }
    }
    return n;
}

/* --- GATT ---------------------------------------------------------------------- */

/* True when this link meets the current security mode. In bonded mode that is an
 * encrypted AND authenticated link; in open mode, anything. */
static bool link_secure_enough(uint16_t conn_handle)
{
    if (s_ble.mode != BLE_SEC_BONDED) {
        return true;
    }
    struct ble_gap_conn_desc desc;
    return ble_gap_conn_find(conn_handle, &desc) == 0 && desc.sec_state.encrypted &&
           desc.sec_state.authenticated;
}

static int gatt_rx_write(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr_handle; (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    /*
     * Enforce the security mode here rather than through characteristic flags: the mode
     * is switchable at runtime and flags are fixed when the service registers.
     *
     * Both bits are required, not just encryption. An unauthenticated ("Just Works")
     * pairing is encrypted but gives no protection against an active man in the middle,
     * and this characteristic can run `cal` -- commands that silently corrupt a year of
     * accumulated charge. Encryption alone is not the bar.
     */
    if (!link_secure_enough(conn_handle)) {
        s_ble.stats.rejected++;
        return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    }

    conn_slot_t *slot = slot_by_handle(conn_handle);
    if (!slot) {
        return BLE_ATT_ERR_UNLIKELY; /* write from a connection we never saw */
    }

    /* Flatten the mbuf and assemble lines into THIS connection's buffer. Nothing here
     * blocks: it runs on the host task, and waiting for a slow consumer would stall the
     * whole stack. A dropped command is recoverable by retyping; a stalled host is not. */
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    char     buf[128];
    uint16_t off = 0;

    while (len > 0) {
        const uint16_t take = len > sizeof(buf) ? (uint16_t)sizeof(buf) : len;
        if (ble_hs_mbuf_to_flat(ctxt->om, buf, take, NULL) != 0) {
            break;
        }
        s_ble.stats.rx_bytes += take;

        for (uint16_t i = 0; i < take; i++) {
            const char c = buf[i];

            if (c == '\r' || c == '\n') {
                if (slot->len == 0) {
                    continue; /* bare newline: terminals send CRLF */
                }
                slot->line[slot->len] = '\0';

                cmd_msg_t msg;
                msg.handle = conn_handle;
                memcpy(msg.line, slot->line, slot->len + 1);
                slot->len = 0;

                if (xQueueSend(s_ble.cmdq, &msg, 0) != pdTRUE) {
                    /* The worker is busy with a long command. Dropping is honest and
                     * recoverable; blocking the host task is neither. */
                    s_ble.stats.dropped++;
                } else {
                    s_ble.stats.lines++;
                }
                continue;
            }

            if (c == 0x08 || c == 0x7F) { /* backspace, for interactive terminals */
                if (slot->len > 0) {
                    slot->len--;
                }
                continue;
            }

            if (slot->len < sizeof(slot->line) - 1) {
                slot->line[slot->len++] = c;
            } else {
                /* Overlong line: discard it whole rather than execute a truncated
                 * command. A silently shortened command is how you run `zero 5`
                 * instead of `zero 512`. */
                slot->len = 0;
                ble_write_conn(conn_handle, "\r\nline too long, discarded\r\n", 0);
            }
        }

        os_mbuf_adj(ctxt->om, take);
        len -= take;
        off += take;
    }
    return 0;
}

/*
 * TX is notify-only and nothing ever reads it -- but NimBLE requires an access callback
 * on EVERY characteristic: ble_gatts_count_resources() returns BLE_HS_EINVAL (rc=3) for
 * a NULL one, which surfaces as a service that fails to register and a device that
 * never advertises. Refuse the read explicitly rather than hand back whatever the value
 * handle points at.
 */
static int gatt_tx_access(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)ctxt; (void)arg;
    return BLE_ATT_ERR_READ_NOT_PERMITTED;
}

/*
 * Firmware-update data. Held to exactly the same bar as RX, because it is the more
 * dangerous of the two: RX can miscalibrate the gauge, this can replace the firmware.
 *
 * Write WITH response only. Each acknowledgement is the flow control -- the next chunk
 * is not sent until this one is in flash -- and a refusal comes back as the write's
 * status instead of vanishing the way a failed write-without-response does.
 */
static int gatt_ota_write(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr_handle; (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (!link_secure_enough(conn_handle)) {
        s_ble.stats.rejected++;
        return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    }
    if (!s_ble.on_ota) {
        return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    }

    /* One ATT write is at most MTU - 3 = 509 bytes. Static: only the host task runs
     * this, and half a kilobyte is a lot of its 4 KB stack. */
    static uint8_t buf[512];
    const uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len > sizeof(buf)) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, len, NULL) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    s_ble.stats.rx_bytes += len;
    return s_ble.on_ota(buf, len, conn_handle, s_ble.user);
}

static const struct ble_gatt_chr_def nus_chrs[] = {
    {
        .uuid      = NUS_RX_UUID,
        .access_cb = gatt_rx_write,
        .flags     = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
    },
    {
        .uuid       = NUS_TX_UUID,
        .access_cb  = gatt_tx_access,
        .val_handle = &s_ble.tx_val_handle,
        .flags      = BLE_GATT_CHR_F_NOTIFY,
    },
    {
        .uuid      = NUS_OTA_UUID,
        .access_cb = gatt_ota_write,
        .flags     = BLE_GATT_CHR_F_WRITE,
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
            if (slot_alloc(event->connect.conn_handle) == NULL) {
                ESP_LOGW(TAG, "no free slot; dropping connection %u",
                         event->connect.conn_handle);
                ble_gap_terminate(event->connect.conn_handle,
                                  BLE_ERR_REM_USER_CONN_TERM);
                return 0;
            }
            ESP_LOGI(TAG, "connected, handle %u (%d of %d)",
                     event->connect.conn_handle, conn_count(), BLE_MAX_CONNS);

            if (s_ble.mode == BLE_SEC_BONDED) {
                const int rc = ble_gap_security_initiate(event->connect.conn_handle);
                if (rc != 0 && rc != BLE_HS_EALREADY) {
                    ESP_LOGW(TAG, "security_initiate: %d", rc);
                }
            }
        }
        /*
         * Keep advertising while a slot remains. Without this a second central can
         * never find the device -- the first connection would silently make it
         * invisible, which is the whole reason multi-connection support looks broken
         * when it is not.
         */
        s_ble.advertising = false;
        if (conn_count() < BLE_MAX_CONNS) {
            advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected (reason %d)", event->disconnect.reason);
        slot_free(event->disconnect.conn.conn_handle);
        if (s_ble.on_passkey && sub_count() == 0) {
            s_ble.on_passkey(0, s_ble.user); /* clear any passkey left on screen */
        }
        advertise();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE: {
        if (event->subscribe.attr_handle != s_ble.tx_val_handle) {
            return 0;
        }
        conn_slot_t *slot = slot_by_handle(event->subscribe.conn_handle);
        if (slot) {
            slot->subscribed = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "handle %u notifications %s", event->subscribe.conn_handle,
                     slot->subscribed ? "enabled" : "disabled");
            if (slot->subscribed) {
                /* A terminal app shows nothing until something arrives, so say hello:
                 * it proves the link works before anything is typed. Unicast -- the
                 * other clients do not need to see it. */
                ble_write_conn(event->subscribe.conn_handle,
                                      "\r\nbat-monitor console over BLE. 'help' lists "
                                      "commands.\r\n", 0);
            }
        }
        return 0;
    }

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "handle %u MTU %u", event->mtu.conn_handle, event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        /* io_cap is DISPLAY_ONLY, so the only action we can be asked for is DISP: we
         * choose the number, show it, and the phone types it back. That is what makes
         * the pairing authenticated rather than Just Works. */
        if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
            struct ble_sm_io io = {.action = BLE_SM_IOACT_DISP};
            io.passkey = (s_ble.passkey_cfg == PASSKEY_RANDOM)
                             ? (esp_random() % 1000000u)
                             : s_ble.passkey_cfg;

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
            ESP_LOGI(TAG, "handle %u encryption %s: enc=%d auth=%d bonded=%d",
                     event->enc_change.conn_handle,
                     event->enc_change.status == 0 ? "established" : "FAILED",
                     desc.sec_state.encrypted, desc.sec_state.authenticated,
                     desc.sec_state.bonded);
        }
        if (s_ble.on_passkey) {
            s_ble.on_passkey(0, s_ble.user); /* take the passkey off the screen */
        }
        return 0;
    }

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        /* The peer wants to pair again while we still hold a bond for it. Deleting the
         * old bond and allowing the new one is what every phone expects after "forget
         * this device" on its side; refusing leaves a link that can never be
         * re-established without physical access here. */
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
    if (conn_count() >= BLE_MAX_CONNS || s_ble.advertising) {
        return;
    }

    struct ble_hs_adv_fields fields = {0};
    fields.flags                 = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name                  = (uint8_t *)s_ble.name;
    fields.name_len              = (uint8_t)strlen(s_ble.name);
    fields.name_is_complete      = 1;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl            = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields: %d", rc);
        return;
    }

    /* The 128-bit NUS UUID goes in the scan response: a 16-byte UUID plus a name does
     * not fit in one 31-byte advertisement, and dropping the name is worse -- the name
     * is how a person finds the right board. */
    struct ble_hs_adv_fields rsp = {0};
    ble_uuid128_t            svc = *(ble_uuid128_t *)NUS_SVC_UUID;
    rsp.uuids128                 = &svc;
    rsp.num_uuids128             = 1;
    rsp.uuids128_is_complete     = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGW(TAG, "adv_rsp_set_fields: %d (continuing without it)", rc);
    }

    struct ble_gap_adv_params adv = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        /* 500 ms, matching the T0/T1 advertising interval of DESIGN.md §9.5. Units are
         * 0.625 ms. */
        .itvl_min = 800,
        .itvl_max = 800,
    };
    rc = ble_gap_adv_start(s_ble.addr_type, NULL, BLE_HS_FOREVER, &adv, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_start: %d", rc);
        return;
    }
    s_ble.advertising = true;
}

/* --- host callbacks ------------------------------------------------------------ */

static void on_sync(void)
{
    if (ble_hs_util_ensure_addr(0) != 0 ||
        ble_hs_id_infer_auto(0, &s_ble.addr_type) != 0) {
        ESP_LOGE(TAG, "address setup failed");
        return;
    }
    advertise();
    ESP_LOGI(TAG, "advertising as '%s', up to %d connections", s_ble.name,
             BLE_MAX_CONNS);
}

static void on_reset(int reason)
{
    /* DESIGN.md §10: a BLE fault must never stop the gauge. Log and let NimBLE come
     * back; nothing above this line depends on the link. */
    ESP_LOGW(TAG, "stack reset, reason %d", reason);
    for (int i = 0; i < BLE_MAX_CONNS; i++) {
        s_ble.conns[i].handle     = BLE_HS_CONN_HANDLE_NONE;
        s_ble.conns[i].subscribed = false;
        s_ble.conns[i].len        = 0;
    }
    s_ble.advertising = false;
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* --- output -------------------------------------------------------------------- */

/* How long a reply may wait for the stack to free buffers before the rest is given up.
 * Measured from the last notification that went out, not from the start: a long reply
 * over a 20-byte MTU legitimately takes seconds, and only a link making no progress at
 * all is a reason to stop. */
#define REPLY_STALL_MS 3000
#define REPLY_RETRY_MS 5

size_t ble_payload_max(uint16_t conn)
{
    /* ATT_MTU includes the 3-byte notification header. Re-read it per call and per
     * connection: centrals negotiate upward a moment after connecting, and they do not
     * all agree, so a cached or shared value caps somebody at 20 bytes. */
    uint16_t mtu = ble_att_mtu(conn);
    if (mtu < 23) {
        mtu = 23;
    }
    return mtu - 3;
}

/*
 * `wait` decides what running out of mbufs means. Telemetry and anything sent from the
 * host task must not wait: the host task is what frees the buffers, and a stale record
 * is replaced by the next one anyway. A command reply is different -- losing its tail
 * loses the `exit` line and the 0x04 terminator, so the client cannot tell the reply
 * ended and times out on a command that ran fine. The pool holds a couple of dozen
 * buffers and drains only a few per connection event, so a reply of more than a few
 * notifications outruns it every time. Replies therefore wait for it to drain.
 */
static void notify_one(uint16_t conn, const char *data, size_t n, bool wait)
{
    const size_t chunk = ble_payload_max(conn);

    size_t     off   = 0;
    TickType_t since = xTaskGetTickCount();
    while (off < n) {
        const size_t take = (n - off) > chunk ? chunk : (n - off);

        /* ble_gatts_notify_custom() consumes the mbuf on failure as well as success,
         * so a retry needs a fresh one. */
        struct os_mbuf *om = ble_hs_mbuf_from_flat(&data[off], (uint16_t)take);
        int rc = om ? ble_gatts_notify_custom(conn, s_ble.tx_val_handle, om)
                    : BLE_HS_ENOMEM;
        if (rc == 0) {
            s_ble.stats.tx_bytes += take;
            off  += take;
            since = xTaskGetTickCount();
            continue;
        }

        const bool stalled = (xTaskGetTickCount() - since) >= pdMS_TO_TICKS(REPLY_STALL_MS);
        if (rc != BLE_HS_ENOMEM || !wait || stalled || !slot_by_handle(conn)) {
            /* Not a full pool, or not worth waiting for, or the central went away. */
            s_ble.stats.dropped += (n - off);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(REPLY_RETRY_MS) + 1); /* +1: 5 ms is 0 ticks at 100 Hz */
    }
}

void ble_write_conn(uint16_t conn, const char *data, size_t n)
{
    if (!data) {
        return;
    }
    if (n == 0) {
        n = strlen(data);
    }
    const conn_slot_t *s = slot_by_handle(conn);
    if (!s || !s->subscribed) {
        s_ble.stats.dropped += n;
        return;
    }
    notify_one(conn, data, n, false);
}

void ble_reply_conn(uint16_t conn, const char *data, size_t n)
{
    if (!data || n == 0) {
        return;
    }
    const conn_slot_t *s = slot_by_handle(conn);
    if (!s || !s->subscribed) {
        s_ble.stats.dropped += n;
        return;
    }
    notify_one(conn, data, n, true);
}

void ble_write(const char *data, size_t n)
{
    if (!data) {
        return;
    }
    if (n == 0) {
        n = strlen(data);
    }
    if (sub_count() == 0) {
        s_ble.stats.dropped += n;
        return;
    }
    for (int i = 0; i < BLE_MAX_CONNS; i++) {
        if (s_ble.conns[i].handle != BLE_HS_CONN_HANDLE_NONE &&
            s_ble.conns[i].subscribed) {
            notify_one(s_ble.conns[i].handle, data, n, false);
        }
    }
}

bool ble_ready(void)
{
    return sub_count() > 0;
}

const char *ble_name(void)
{
    return s_ble.name;
}

void ble_get_stats(ble_stats_t *out)
{
    if (!out) {
        return;
    }
    *out              = s_ble.stats;
    out->advertising  = s_ble.advertising;
    out->connections  = conn_count();
    out->subscribers  = sub_count();
    out->mode         = s_ble.mode;

    /* The security flags describe the WEAKEST link, not the best one: with several
     * centrals attached, "encrypted" must not read true because one of them is. */
    out->encrypted     = out->connections > 0;
    out->authenticated = out->connections > 0;
    out->mtu           = 0;

    for (int i = 0; i < BLE_MAX_CONNS; i++) {
        const uint16_t h = s_ble.conns[i].handle;
        if (h == BLE_HS_CONN_HANDLE_NONE) {
            continue;
        }
        const uint16_t m = ble_att_mtu(h);
        if (out->mtu == 0 || m < out->mtu) {
            out->mtu = m;
        }
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(h, &desc) == 0) {
            if (!desc.sec_state.encrypted)     out->encrypted     = false;
            if (!desc.sec_state.authenticated) out->authenticated = false;
        } else {
            out->encrypted = out->authenticated = false;
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

esp_err_t ble_set_sec_mode(ble_sec_mode_t mode)
{
    if (mode != BLE_SEC_OPEN && mode != BLE_SEC_BONDED) {
        return ESP_ERR_INVALID_ARG;
    }
    const bool tightening = (mode == BLE_SEC_BONDED && s_ble.mode == BLE_SEC_OPEN);
    s_ble.mode = mode;
    sec_persist();

    /* Tightening must not leave existing links with the access they had under the old
     * rules. Dropping them forces every peer through the new path, which is also the
     * only way the change is visible to the person who made it. */
    if (tightening) {
        (void)ble_disconnect();
    }
    return ESP_OK;
}

ble_sec_mode_t ble_get_sec_mode(void)
{
    return s_ble.mode;
}

esp_err_t ble_set_passkey(uint32_t passkey)
{
    if (passkey != PASSKEY_RANDOM && passkey > 999999u) {
        return ESP_ERR_INVALID_ARG;
    }
    s_ble.passkey_cfg = passkey;
    sec_persist();
    return ESP_OK;
}

uint32_t ble_get_passkey(void)
{
    return s_ble.passkey_cfg;
}

int ble_list_bonds(char out[][24], int max)
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

esp_err_t ble_clear_bonds(void)
{
    /* ble_store_clear() drops the live links' keys too, so terminate first: an
     * encrypted connection whose keys have been deleted underneath it is a connection
     * in an undefined state. */
    (void)ble_disconnect();
    return ble_store_clear() == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t ble_disconnect(void)
{
    int n = 0;
    for (int i = 0; i < BLE_MAX_CONNS; i++) {
        const uint16_t h = s_ble.conns[i].handle;
        if (h != BLE_HS_CONN_HANDLE_NONE) {
            ble_gap_terminate(h, BLE_ERR_REM_USER_CONN_TERM);
            n++;
        }
    }
    return n ? ESP_OK : ESP_ERR_INVALID_STATE;
}

/* --- worker: queued lines to commands ------------------------------------------ */

static void worker_task(void *arg)
{
    (void)arg;
    cmd_msg_t msg;

    for (;;) {
        if (xQueueReceive(s_ble.cmdq, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (s_ble.on_line) {
            s_ble.on_line(msg.line, msg.handle, s_ble.user);
        }
    }
}

/* --- start --------------------------------------------------------------------- */

esp_err_t ble_start(const ble_config_t *cfg)
{
    if (!cfg || !cfg->device_name) {
        return ESP_ERR_INVALID_ARG;
    }

    s_ble.on_line    = cfg->on_line;
    s_ble.on_ota     = cfg->on_ota;
    s_ble.on_passkey = cfg->on_passkey;
    s_ble.user       = cfg->user;
    for (int i = 0; i < BLE_MAX_CONNS; i++) {
        s_ble.conns[i].handle = BLE_HS_CONN_HANDLE_NONE;
    }

    snprintf(s_ble.name, sizeof(s_ble.name), "%s", cfg->device_name);
    if (cfg->append_mac) {
        uint8_t mac[6] = {0};
        if (esp_read_mac(mac, ESP_MAC_BT) == ESP_OK) {
            const size_t at = strlen(s_ble.name);
            snprintf(&s_ble.name[at], sizeof(s_ble.name) - at, "-%02X%02X", mac[4],
                     mac[5]);
        }
    }

    s_ble.cmdq = xQueueCreate(CMD_QUEUE_LEN, sizeof(cmd_msg_t));
    if (!s_ble.cmdq) {
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
     * Security manager (DESIGN.md §8.5). Configured unconditionally, even in OPEN mode:
     * these settings only describe what happens IF a peer asks to pair, and a peer may
     * ask at any time. Enforcement -- whether an unpaired peer can actually run
     * commands -- is the mode check in gatt_rx_write(), not these flags.
     *
     * DISPLAY_ONLY with sm_mitm gives passkey-display pairing: the device picks six
     * digits, the phone types them, and the exchange is authenticated. sm_sc selects LE
     * Secure Connections (ECDH) over legacy pairing, which matters because legacy
     * passkey pairing is offline-crackable from a single sniffed exchange.
     */
    ble_hs_cfg.sm_io_cap         = BLE_HS_IO_DISPLAY_ONLY;
    ble_hs_cfg.sm_bonding        = 1;
    ble_hs_cfg.sm_mitm           = 1;
    ble_hs_cfg.sm_sc             = 1;
    ble_hs_cfg.sm_our_key_dist   = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.store_status_cb   = ble_store_util_status_rr;

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
