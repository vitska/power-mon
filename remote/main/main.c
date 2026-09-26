/*
 * batmon-remote — a touch display for the battery monitor, on the ESP32-2432S028.
 *
 * It does what the phone app's Monitor tab does, on a screen that can live next to the
 * battery: connects to one `batmon-*` board over BLE, pairs with it if the board asks,
 * and shows state of charge, voltage, current, mode, the time to empty or full, and a
 * SoC graph. See README.md.
 */

#include "esp_app_desc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "lcd.h"
#include "link.h"
#include "touch.h"
#include "ui.h"

static const char *TAG = "remote";

static void ui_task(void *arg)
{
    ui_run();
}

void app_main(void)
{
    /* NVS holds the remembered board and the BLE bonds. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    lcd_init();
    touch_init();

    const char *ver = esp_app_get_description()->version;
    lcd_text(60, 100, "batmon remote", 3, C_WHITE, C_BLACK);
    lcd_text(60, 130, ver, 2, C_GREY, C_BLACK);
    ESP_LOGI(TAG, "batmon-remote %s", ver);

    link_start();
    xTaskCreate(ui_task, "ui", 6144, NULL, 3, NULL);
}
