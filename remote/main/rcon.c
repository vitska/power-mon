/*
 * rcon.c — see rcon.h.
 *
 * Same division of labour as the monitor's BLE console: the NimBLE host task only
 * assembles lines and queues them, and a worker task runs them -- `ota begin` erases
 * the spare slot for seconds, which the host task must never be kept waiting for.
 * Image bytes on the OTA characteristic go straight to ota_write(), which never blocks.
 *
 * The OTA core is the monitor's own ota.c, compiled into this firmware too: it is
 * chip-neutral, and checks an image against THIS firmware's project name, so a
 * monitor image is refused here exactly as a remote image is refused there.
 */

#include "rcon.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "console.h"
#include "ota.h"

static const char *TAG = "rcon";

#define RCON_LINE_MAX 160
#define OUT_MAX  1024

/* The monitor's UUIDs, so the phone app's client needs nothing new to talk to us. */
#define NUS_UUID(b0) BLE_UUID128_DECLARE(0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, \
                                         0x93, 0xF3, 0xA3, 0xB5, (b0), 0x00, 0x40, 0x6E)

/* The phone app reads these as the write's ATT status, as it does from the monitor. */
#define OTA_ATT_NO_SESSION 0x80
#define OTA_ATT_BAD_OFFSET 0x81
#define OTA_ATT_TOO_LONG   0x82
#define OTA_ATT_BUSY       0x83
#define OTA_ATT_FLASH      0x84

static struct {
    bool          want_adv;
    bool          synced;
    uint16_t      conn;
    bool          subscribed;
    uint16_t      tx_handle;
    char          name[24];
    char          line[RCON_LINE_MAX];
    size_t        len;
    QueueHandle_t q;
} s = {.conn = BLE_HS_CONN_HANDLE_NONE};

static void advertise(void);

/* --- output ------------------------------------------------------------------------- */

static char   s_out[OUT_MAX];
static size_t s_out_len;

static void out(const char *fmt, ...)
{
    char    tmp[160];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    /* CRLF on the wire, like the monitor: the client's line splitter is shared. */
    for (int i = 0; i < n && i < (int)sizeof(tmp) - 1 && s_out_len < OUT_MAX - 2; i++) {
        if (tmp[i] == '\n') s_out[s_out_len++] = '\r';
        s_out[s_out_len++] = tmp[i];
    }
}

/* Sends the reply in MTU-sized notifications, waiting out a full buffer pool rather
 * than dropping the tail -- the tail is where the terminator lives. */
static void flush(void)
{
    if (s.conn == BLE_HS_CONN_HANDLE_NONE || !s.subscribed) {
        s_out_len = 0;
        return;
    }
    uint16_t mtu = ble_att_mtu(s.conn);
    if (mtu < 23) mtu = 23;
    const size_t chunk = mtu - 3;
    size_t off = 0;
    int    stall = 0;
    while (off < s_out_len && stall < 300) {
        const size_t    n  = s_out_len - off < chunk ? s_out_len - off : chunk;
        struct os_mbuf *om = ble_hs_mbuf_from_flat(&s_out[off], n);
        if (om && ble_gatts_notify_custom(s.conn, s.tx_handle, om) == 0) {
            off  += n;
            stall = 0;
        } else {
            stall++;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    s_out_len = 0;
}

/* --- commands ------------------------------------------------------------------------- */

/* Every command is the console's (console.c): the phone app's `ver` and `ota ...`, and
 * the display's own settings, the same set the serial port takes. */
static void exec_line(char *line)
{
    s_out_len = 0;
    out("> %s\n", line);

    char *argv[12];
    int   argc = 0;
    for (char *tok = strtok(line, " "); tok && argc < 12; tok = strtok(NULL, " ")) {
        argv[argc++] = tok;
    }
    const int status = console_exec(argc, argv, out);
    out("exit %d\n", status);
    s_out[s_out_len++] = 0x04;
    flush();
}

static void rcon_task(void *arg)
{
    char line[RCON_LINE_MAX];
    for (;;) {
        if (xQueueReceive(s.q, line, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI(TAG, "> %s", line);
            exec_line(line);
        }
    }
}

/* --- GATT ------------------------------------------------------------------------------ */

static int on_rx(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    char           buf[128];
    uint16_t       len = OS_MBUF_PKTLEN(ctxt->om);
    struct os_mbuf *om = ctxt->om;
    while (len > 0) {
        const uint16_t take = len > sizeof(buf) ? sizeof(buf) : len;
        if (ble_hs_mbuf_to_flat(om, buf, take, NULL) != 0) break;
        for (uint16_t i = 0; i < take; i++) {
            const char c = buf[i];
            if (c == '\r' || c == '\n') {
                if (s.len) {
                    s.line[s.len] = '\0';
                    xQueueSend(s.q, s.line, 0); /* a full queue drops: never block the host */
                    s.len = 0;
                }
            } else if (s.len < RCON_LINE_MAX - 1) {
                s.line[s.len++] = c;
            } else {
                s.len = 0; /* overlong: discard whole rather than run a truncated command */
            }
        }
        os_mbuf_adj(om, take);
        len -= take;
    }
    return 0;
}

static int on_tx(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return BLE_ATT_ERR_READ_NOT_PERMITTED; /* notify-only; NimBLE still wants a callback */
}

static int on_ota(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    static uint8_t buf[512];
    const uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len > sizeof(buf)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, len, NULL) != 0) return BLE_ATT_ERR_UNLIKELY;
    switch (ota_write(buf, len)) {
    case ESP_OK:                return 0;
    case ESP_ERR_INVALID_STATE: return OTA_ATT_NO_SESSION;
    case ESP_ERR_INVALID_ARG:   return OTA_ATT_BAD_OFFSET;
    case ESP_ERR_INVALID_SIZE:  return OTA_ATT_TOO_LONG;
    case ESP_ERR_TIMEOUT:       return OTA_ATT_BUSY;
    default:                    return OTA_ATT_FLASH;
    }
}

static const struct ble_gatt_chr_def CHRS[] = {
    {.uuid = NUS_UUID(0x02), .access_cb = on_rx,
     .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP},
    {.uuid = NUS_UUID(0x03), .access_cb = on_tx, .val_handle = &s.tx_handle,
     .flags = BLE_GATT_CHR_F_NOTIFY},
    {.uuid = NUS_UUID(0x04), .access_cb = on_ota, .flags = BLE_GATT_CHR_F_WRITE},
    {0},
};

static const struct ble_gatt_svc_def SVCS[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = NUS_UUID(0x01), .characteristics = CHRS},
    {0},
};

/* --- GAP ----------------------------------------------------------------------------- */

static int gap_event(struct ble_gap_event *e, void *arg)
{
    switch (e->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (e->connect.status == 0) {
            s.conn       = e->connect.conn_handle;
            s.subscribed = false;
            s.len        = 0;
            ESP_LOGI(TAG, "phone connected");
        } else {
            advertise();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "phone disconnected");
        s.conn       = BLE_HS_CONN_HANDLE_NONE;
        s.subscribed = false;
        /* An update abandoned mid-transfer must not leave the slot half-claimed. */
        ota_abort();
        advertise();
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (e->subscribe.attr_handle == s.tx_handle) s.subscribed = e->subscribe.cur_notify;
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        return 0;
    default:
        return 0;
    }
}

static void advertise(void)
{
    if (!s.synced || !s.want_adv || s.conn != BLE_HS_CONN_HANDLE_NONE || ble_gap_adv_active()) {
        return;
    }
    uint8_t own;
    ble_hs_id_infer_auto(0, &own);

    /* Name in the advertisement, service UUID in the scan response: both do not fit
     * in 31 bytes, and this is also how the monitor does it. */
    struct ble_hs_adv_fields f = {0};
    f.flags            = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.name             = (const uint8_t *)s.name;
    f.name_len         = strlen(s.name);
    f.name_is_complete = 1;
    ble_gap_adv_set_fields(&f);

    static const ble_uuid128_t svc = BLE_UUID128_INIT(0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9,
                                                      0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00,
                                                      0x40, 0x6E);
    struct ble_hs_adv_fields r = {0};
    r.uuids128             = &svc;
    r.num_uuids128         = 1;
    r.uuids128_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&r);

    const struct ble_gap_adv_params p = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min  = 0x0320, /* 500 ms, like the monitor */
        .itvl_max  = 0x0320,
    };
    const int rc = ble_gap_adv_start(own, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc != 0) ESP_LOGW(TAG, "advertise failed: %d", rc);
    else         ESP_LOGI(TAG, "advertising as %s", s.name);
}

/* --- public ---------------------------------------------------------------------------- */

void rcon_register(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    snprintf(s.name, sizeof(s.name), "batmon-remote-%02X%02X", mac[4], mac[5]);

    s.q = xQueueCreate(4, RCON_LINE_MAX);
    xTaskCreate(rcon_task, "rcon", 4096, NULL, 4, NULL);

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(s.name);
    ESP_ERROR_CHECK(ble_gatts_count_cfg(SVCS));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(SVCS));
}

void rcon_on_sync(void)
{
    s.synced = true;
    advertise();
}

void rcon_advertise(bool on)
{
    s.want_adv = on;
    if (on) {
        advertise();
    } else if (ble_gap_adv_active()) {
        ble_gap_adv_stop();
    }
}

void rcon_status(rcon_status_t *out_st)
{
    out_st->advertising = ble_gap_adv_active();
    out_st->connected   = s.conn != BLE_HS_CONN_HANDLE_NONE;
    snprintf(out_st->name, sizeof(out_st->name), "%s", s.name);
}
