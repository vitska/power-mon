/*
 * ota.c — see ota.h.
 *
 * WHY THE BOOTLOADER IS NOT THE ONE LISTENING. "Update over BLE from the bootloader"
 * would need a Bluetooth stack in the second-stage bootloader, which ESP-IDF does not
 * have and which would not fit its 32 KB anyway. What the bootloader does instead is
 * the part only it can do: choose between two app slots, and refuse to keep choosing an
 * image that never proved itself (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE). The running
 * firmware receives the new image into the spare slot; the bootloader guarantees that a
 * bad one costs a reset, not a trip to the bench with a USB cable.
 *
 * PROBATION. A newly written image boots in PENDING_VERIFY. It stays there until
 * something that has actually talked to it over the air says it works -- `ota confirm`,
 * which the phone app sends after its handshake succeeds. A reset before that, from a
 * crash, the watchdog or a power cycle, sends the bootloader back to the old image. So
 * does the timer here: an image that boots but has broken the radio would otherwise sit
 * running forever, unconfirmable and unreachable.
 *
 * THE HOST TASK RULE. ota_write() runs on the NimBLE host task and so must never wait:
 * it try-takes the lock and reports busy. The slow parts -- erasing the slot in
 * ota_begin(), reading it all back in ota_finish() -- run on the console worker, which
 * is allowed to block. The erase is done up front for the same reason: done lazily
 * inside esp_ota_write() it would stall the host task for tens of milliseconds at every
 * sector boundary.
 */

#include "ota.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/sha256.h"

static const char *TAG = "ota";

static struct {
    SemaphoreHandle_t      lock;
    ota_phase_t            phase;
    const esp_partition_t *part;
    esp_ota_handle_t       handle;
    uint32_t               size;
    uint32_t               received;
    uint32_t               last_off; /* start of the last accepted chunk */
    uint8_t                want[32];
    mbedtls_sha256_context sha;

    esp_timer_handle_t probation;
    int64_t            probation_deadline_us; /* 0 when not on probation */
    esp_timer_handle_t reboot;
    esp_timer_handle_t rollback;
} s;

/* --- timers -------------------------------------------------------------------- */

static void do_rollback(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "rolling back to the previous image");
    /* Returns only on failure: nothing valid to go back to. Then the running image is
     * the only one there is, and keeping it beats rebooting into nothing. */
    const esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
    ESP_LOGE(TAG, "rollback failed: %s -- keeping the running image", esp_err_to_name(err));
}

static void on_probation_expired(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "not confirmed within %d s", OTA_PROBATION_S);
    do_rollback(NULL);
}

static void do_reboot(void *arg)
{
    (void)arg;
    esp_restart();
}

static esp_timer_handle_t make_timer(esp_timer_cb_t cb, const char *name)
{
    const esp_timer_create_args_t a = {.callback = cb, .name = name};
    esp_timer_handle_t t = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&a, &t));
    return t;
}

/* --- boot ---------------------------------------------------------------------- */

void ota_init(void)
{
    s.lock      = xSemaphoreCreateMutex();
    s.probation = make_timer(on_probation_expired, "ota_probation");
    s.reboot    = make_timer(do_reboot, "ota_reboot");
    s.rollback  = make_timer(do_rollback, "ota_rollback");

    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t   st  = ESP_OTA_IMG_UNDEFINED;
    esp_ota_get_state_partition(run, &st);

    ESP_LOGI(TAG, "firmware %s running from %s", esp_app_get_description()->version,
             run->label);

    if (st == ESP_OTA_IMG_PENDING_VERIFY) {
        s.probation_deadline_us = esp_timer_get_time() + (int64_t)OTA_PROBATION_S * 1000000;
        esp_timer_start_once(s.probation, (uint64_t)OTA_PROBATION_S * 1000000);
        ESP_LOGW(TAG, "new image on probation: 'ota confirm' within %d s, or it rolls back",
                 OTA_PROBATION_S);
    }
}

/* --- session ------------------------------------------------------------------- */

static void abort_locked(void)
{
    if (s.phase == OTA_RECEIVING) {
        esp_ota_abort(s.handle);
        mbedtls_sha256_free(&s.sha);
    }
    s.phase    = OTA_IDLE;
    s.received = 0;
    s.size     = 0;
}

esp_err_t ota_begin(uint32_t size, const uint8_t sha256[32])
{
    if (size == 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    xSemaphoreTake(s.lock, portMAX_DELAY);
    abort_locked();

    esp_err_t              err  = ESP_OK;
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        err = ESP_ERR_NOT_FOUND; /* single-slot partition table */
    } else if (size > part->size) {
        err = ESP_ERR_INVALID_SIZE;
    } else {
        /* Erases `size` bytes now; see the file comment for why not lazily. Refuses
         * with ESP_ERR_OTA_ROLLBACK_INVALID_STATE while the running image is itself on
         * probation -- writing over the only known-good image then is exactly what
         * rollback exists to prevent. */
        err = esp_ota_begin(part, size, &s.handle);
    }
    if (err == ESP_OK) {
        s.part     = part;
        s.size     = size;
        s.received = 0;
        s.last_off = UINT32_MAX;
        memcpy(s.want, sha256, sizeof(s.want));
        mbedtls_sha256_init(&s.sha);
        mbedtls_sha256_starts(&s.sha, 0);
        s.phase = OTA_RECEIVING;
        ESP_LOGI(TAG, "receiving %lu bytes into %s", (unsigned long)size, part->label);
    }
    xSemaphoreGive(s.lock);
    return err;
}

esp_err_t ota_write(const uint8_t *data, size_t len)
{
    if (!s.lock || len < OTA_HDR_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s.lock, 0) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    const uint32_t off = (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
                         ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
    const uint8_t *p = data + OTA_HDR_BYTES;
    const size_t   n = len - OTA_HDR_BYTES;

    esp_err_t err;
    if (s.phase != OTA_RECEIVING) {
        err = ESP_ERR_INVALID_STATE;
    } else if (off == s.last_off && off + n == s.received) {
        err = ESP_OK; /* a retry of the chunk just taken: already written */
    } else if (off != s.received || n == 0) {
        err = ESP_ERR_INVALID_ARG;
    } else if (n > s.size - s.received) {
        err = ESP_ERR_INVALID_SIZE;
    } else {
        err = esp_ota_write(s.handle, p, n);
        if (err == ESP_OK) {
            mbedtls_sha256_update(&s.sha, p, n);
            s.last_off  = off;
            s.received += n;
        } else {
            /* Flash refused, or the first bytes are not an image at all. Nothing
             * later can succeed, so free the handle now. */
            ESP_LOGE(TAG, "write at %lu failed: %s", (unsigned long)off, esp_err_to_name(err));
            abort_locked();
        }
    }
    xSemaphoreGive(s.lock);
    return err;
}

esp_err_t ota_finish(char *why, size_t why_len)
{
    xSemaphoreTake(s.lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;

    if (s.phase != OTA_RECEIVING) {
        snprintf(why, why_len, "no update in progress -- 'ota begin' first");
        err = ESP_ERR_INVALID_STATE;
        goto out;
    }
    if (s.received != s.size) {
        snprintf(why, why_len, "only %lu of %lu bytes received",
                 (unsigned long)s.received, (unsigned long)s.size);
        abort_locked();
        err = ESP_ERR_INVALID_SIZE;
        goto out;
    }

    uint8_t got[32];
    mbedtls_sha256_finish(&s.sha, got);
    mbedtls_sha256_free(&s.sha);
    s.phase = OTA_IDLE; /* the SHA context is gone; only the handle remains */

    if (memcmp(got, s.want, sizeof(got)) != 0) {
        snprintf(why, why_len, "SHA-256 mismatch -- the image was corrupted in transit");
        esp_ota_abort(s.handle);
        err = ESP_ERR_INVALID_CRC;
        goto out;
    }

    /* Checks the image's own structure, checksum and appended hash, and that it was
     * built for this chip. Frees the handle whatever the outcome. */
    err = esp_ota_end(s.handle);
    if (err != ESP_OK) {
        snprintf(why, why_len, "%s",
                 err == ESP_ERR_OTA_VALIDATE_FAILED
                     ? "not a valid firmware image for an ESP32-C6"
                     : esp_err_to_name(err));
        goto out;
    }

    /* A valid ESP32-C6 image is not necessarily THIS firmware. Something else would
     * boot, pass nothing on to the next update, and need a cable to undo. */
    esp_app_desc_t desc;
    err = esp_ota_get_partition_description(s.part, &desc);
    if (err != ESP_OK) {
        snprintf(why, why_len, "cannot read the new image's description");
        goto out;
    }
    const char *mine = esp_app_get_description()->project_name;
    if (strncmp(desc.project_name, mine, sizeof(desc.project_name)) != 0) {
        snprintf(why, why_len, "image is '%.32s', not %s -- refusing", desc.project_name,
                 mine);
        err = ESP_ERR_INVALID_VERSION;
        goto out;
    }

    err = esp_ota_set_boot_partition(s.part);
    if (err != ESP_OK) {
        snprintf(why, why_len, "cannot select it for boot: %s", esp_err_to_name(err));
        goto out;
    }
    snprintf(why, why_len, "%.32s", desc.version);
    ESP_LOGI(TAG, "firmware %s written to %s; boots on next reset", desc.version,
             s.part->label);

out:
    s.received = 0;
    s.size     = 0;
    xSemaphoreGive(s.lock);
    return err;
}

void ota_abort(void)
{
    xSemaphoreTake(s.lock, portMAX_DELAY);
    abort_locked();
    xSemaphoreGive(s.lock);
}

/* --- probation ----------------------------------------------------------------- */

esp_err_t ota_confirm(void)
{
    const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK && s.probation_deadline_us) {
        esp_timer_stop(s.probation);
        s.probation_deadline_us = 0;
        ESP_LOGI(TAG, "image confirmed");
    }
    return err;
}

esp_err_t ota_rollback(void)
{
    if (!esp_ota_check_rollback_is_possible()) {
        return ESP_ERR_NOT_FOUND;
    }
    /* Deferred like a reboot, so the reply announcing it still goes out. */
    esp_timer_stop(s.rollback);
    return esp_timer_start_once(s.rollback, 500 * 1000);
}

void ota_reboot_after(uint32_t ms)
{
    esp_timer_stop(s.reboot);
    esp_timer_start_once(s.reboot, (uint64_t)ms * 1000);
}

/* --- status -------------------------------------------------------------------- */

static void label(char *dst, size_t n, const esp_partition_t *p)
{
    snprintf(dst, n, "%s", p ? p->label : "none");
}

void ota_get_status(ota_status_t *out)
{
    memset(out, 0, sizeof(*out));

    const esp_partition_t *run = esp_ota_get_running_partition();
    label(out->running, sizeof(out->running), run);
    snprintf(out->version, sizeof(out->version), "%s", esp_app_get_description()->version);
    esp_app_get_elf_sha256(out->build, sizeof(out->build));

    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    esp_ota_get_state_partition(run, &st);
    out->pending          = (st == ESP_OTA_IMG_PENDING_VERIFY);
    out->probation_left_s = -1;
    if (out->pending && s.probation_deadline_us) {
        const int64_t left = s.probation_deadline_us - esp_timer_get_time();
        out->probation_left_s = left > 0 ? (int)(left / 1000000) : 0;
    }
    out->can_rollback = esp_ota_check_rollback_is_possible();

    label(out->boot, sizeof(out->boot), esp_ota_get_boot_partition());

    const esp_partition_t *spare = esp_ota_get_next_update_partition(NULL);
    label(out->spare, sizeof(out->spare), spare);
    esp_app_desc_t d;
    if (spare && s.phase == OTA_IDLE && esp_ota_get_partition_description(spare, &d) == ESP_OK) {
        snprintf(out->spare_version, sizeof(out->spare_version), "%.31s", d.version);
    }

    out->phase    = s.phase;
    out->received = s.received;
    out->size     = s.size;
}
