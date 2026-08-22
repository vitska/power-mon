idf_component_register(
    SRCS        "main.c"
                "cli.c"
                "lcd.c"
                "ble.c"
                "cal_store.c"
    INCLUDE_DIRS "."
    REQUIRES     driver esp_timer console bt esp_hw_support nvs_flash esp_app_format esp_driver_usb_serial_jtag vfs ina219 sensors fuelgauge bme280
)
