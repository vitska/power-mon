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

static bool parse_sha256(const char *hex, uint8_t out_sha[32])
{
    if (strlen(hex) != 64) return false;
    for (int i = 0; i < 32; i++) {
        unsigned v;
        if (sscanf(hex + 2 * i, "%2x", &v) != 1) return false;
        out_sha[i] = (uint8_t)v;
    }
    return true;
}

static int cmd_ver(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    char build[17];
    esp_app_get_elf_sha256(build, sizeof(build));
    /* protocol 3: the same framing and OTA commands as the monitor, which is all the
     * app needs from us. `device remote` says which of the two it is talking to. */
    out("protocol 3\n");
    out("firmware %s\n", esp_app_get_description()->version);
    out("build %s\n", build);
    out("device remote\n");
    out("chip esp32\n");
    out("mac %02X:%02X:%02X:%02X:%02X:%02X\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return 0;
}

static int cmd_ota(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "status") == 0) {
        ota_status_t st;
        ota_get_status(&st);
        out("running=%s\n", st.running);
        out("version=%s\n", st.version);
        out("build=%s\n", st.build);
        out("state=%s\n", st.pending ? "probation" : "valid");
        if (st.pending && st.probation_left_s >= 0) out("probation_s=%d\n", st.probation_left_s);
        out("rollback=%d\n", st.can_rollback ? 1 : 0);
        out("boot=%s\n", st.boot);
        out("spare=%s\n", st.spare);
        out("spare.version=%s\n", st.spare_version[0] ? st.spare_version : "none");
        out("session=%s\n", st.phase == OTA_RECEIVING ? "receiving" : "idle");
        if (st.phase == OTA_RECEIVING) {
            out("session.received=%lu\n", (unsigned long)st.received);
            out("session.size=%lu\n", (unsigned long)st.size);
        }
        return 0;
    }
    if (strcmp(argv[1], "begin") == 0) {
        uint8_t       sha[32];
        char         *end  = NULL;
        unsigned long size = argc >= 4 ? strtoul(argv[2], &end, 10) : 0;
        if (argc < 4 || !end || *end || !size || !parse_sha256(argv[3], sha)) {
            out("usage: ota begin <bytes> <sha256 as 64 hex digits>\n");
            return 1;
        }
        const esp_err_t err = ota_begin((uint32_t)size, sha);
        if (err == ESP_OK) {
            out("spare slot erased; send %lu bytes to the OTA characteristic\n", size);
            return 0;
        }
        if (err == ESP_ERR_OTA_ROLLBACK_INVALID_STATE) {
            out("the running image is on probation -- 'ota confirm' or 'ota rollback' first\n");
        } else if (err == ESP_ERR_NOT_FOUND) {
            out("no spare app slot -- flash this remote once over USB (flash.ps1 -Remote)\n");
        } else if (err == ESP_ERR_INVALID_SIZE) {
            out("%lu bytes does not fit the spare slot\n", size);
        } else {
            out("cannot start: %s\n", esp_err_to_name(err));
        }
        return 1;
    }
    if (strcmp(argv[1], "end") == 0) {
        char why[96];
        if (ota_finish(why, sizeof(why)) != ESP_OK) {
            out("%s\n", why);
            return 1;
        }
        out("firmware %s written; 'reboot' to start it\n", why);
        return 0;
    }
    if (strcmp(argv[1], "abort") == 0) {
        ota_abort();
        out("abandoned; the running image is untouched\n");
        return 0;
    }
    if (strcmp(argv[1], "confirm") == 0) {
        ota_status_t st;
        ota_get_status(&st);
        if (!st.pending) {
            out("not on probation; nothing to confirm\n");
            return 0;
        }
        if (ota_confirm() != ESP_OK) {
            out("confirm failed\n");
            return 1;
        }
        out("firmware %s confirmed; it stays\n", st.version);
        return 0;
    }
    if (strcmp(argv[1], "rollback") == 0) {
        if (ota_rollback() != ESP_OK) {
            out("nothing to roll back to -- the other slot has no valid image\n");
            return 1;
        }
        out("rolling back to the previous image; the link will drop\n");
        return 0;
    }
    out("usage: ota [status | begin <bytes> <sha256> | end | abort | confirm | rollback]\n");
    return 1;
}

static void exec_line(char *line)
{
    s_out_len = 0;
    out("> %s\n", line);

    char *argv[6];
    int   argc = 0;
    for (char *tok = strtok(line, " "); tok && argc < 6; tok = strtok(NULL, " ")) {
        argv[argc++] = tok;
    }

    int status = 0;
    if (argc == 0) {
        out("empty command\n");
        status = -3;
    } else if (strcmp(argv[0], "ver") == 0) {
        status = cmd_ver();
    } else if (strcmp(argv[0], "ota") == 0) {
        status = cmd_ota(argc, argv);
    } else if (strcmp(argv[0], "reboot") == 0) {
        out("rebooting; the link will drop\n");
        ota_reboot_after(500);
    } else if (strcmp(argv[0], "help") == 0) {
        out("batmon remote display. Commands: ver, ota [...], reboot, help\n");
    } else {
        out("unknown command -- try 'help'\n");
        status = -2; /* what the app reads as "this firmware has no such command" */
    }
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
