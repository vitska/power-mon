/*
 * ota.h — firmware update: receiving an image into the spare slot, and the probation a
 * freshly booted image serves before it is trusted.
 *
 * Transport-neutral. The console drives the session (`ota begin`, `ota end`, ...) and
 * the image bytes arrive through ota_write(), which ble.c feeds from its OTA
 * characteristic. Nothing here knows about GATT.
 *
 * DATA FORMAT. Each write is a 4-byte little-endian offset followed by image bytes. The
 * offset must equal the number of bytes received so far: writes are acknowledged ATT
 * Write Requests and so arrive in order, and the offset turns a lost or duplicated
 * chunk into a refusal naming the position rather than a corrupt image that is only
 * noticed at the SHA check. An exact repeat of the last chunk is accepted and ignored,
 * so a client may retry a write whose acknowledgement it never saw.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OTA_HDR_BYTES 4 /* the offset prefix on every data write */

/* After an update, how long the new image has to be confirmed before it is rolled back.
 * Ten minutes: time for the phone to reconnect and confirm, with room for a person to
 * look at it first. A power cycle in the meantime rolls back too -- the bootloader does
 * that on its own. */
#define OTA_PROBATION_S 600

typedef enum {
    OTA_IDLE = 0,
    OTA_RECEIVING,
} ota_phase_t;

typedef struct {
    char        running[17];     /**< partition label, e.g. "ota_0" */
    char        version[32];
    char        build[17];       /**< first 16 hex digits of the ELF SHA-256 */
    bool        pending;         /**< on probation: not yet confirmed */
    int         probation_left_s;/**< seconds until forced rollback, or -1 */
    bool        can_rollback;    /**< the other slot holds a valid image */
    char        boot[17];        /**< what the NEXT reset boots */
    char        spare[17];       /**< the slot an update would be written to */
    char        spare_version[32]; /**< image in the spare slot, "" if none/invalid */
    ota_phase_t phase;
    uint32_t    received;
    uint32_t    size;
} ota_status_t;

/** At boot. Logs the running image and, if it is on probation, starts the clock. */
void ota_init(void);

/** Starts a session: erases the spare slot for `size` bytes (seconds of work -- call
 *  from a task that may block) and remembers the SHA-256 the finished image must have.
 *  Any session already in progress is abandoned. */
esp_err_t ota_begin(uint32_t size, const uint8_t sha256[32]);

/** One data write, offset prefix included. Called from the BLE host task, so it never
 *  waits: ESP_ERR_TIMEOUT means the session is busy (erasing or finishing).
 *  ESP_ERR_INVALID_STATE: no session. ESP_ERR_INVALID_ARG: wrong offset or length.
 *  ESP_ERR_INVALID_SIZE: past the announced size. Anything else is from flash. */
esp_err_t ota_write(const uint8_t *data, size_t len);

/** Ends the session: checks length and SHA-256, validates the image, checks it is this
 *  project's, and makes it the next boot. Does not reboot. On failure `why` says what
 *  was wrong in words and the session is abandoned. */
esp_err_t ota_finish(char *why, size_t why_len);

/** Abandons a session, if any. The running image is untouched. */
void ota_abort(void);

/** Ends probation: the running image is kept from now on. */
esp_err_t ota_confirm(void);

/** Marks the running image bad and reboots into the other slot, half a second from now
 *  so the reply can go out first. ESP_ERR_NOT_FOUND when there is nothing valid to go
 *  back to. */
esp_err_t ota_rollback(void);

/** Restarts after `ms`, so the reply saying so can still reach the client. */
void ota_reboot_after(uint32_t ms);

void ota_get_status(ota_status_t *out);

#ifdef __cplusplus
}
#endif
