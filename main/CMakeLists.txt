idf_component_register(
    SRCS        "main.c"
                "console_cmds.c"
                "console_io.c"
                "display_debug.c"
                "ble_console.c"
                "ble.c"
                "cal_store.c"
    INCLUDE_DIRS "."
    REQUIRES     driver esp_timer console bt esp_hw_support nvs_flash esp_app_format esp_driver_usb_serial_jtag vfs ina219 sensors ssd1306 fuelgauge bme280
)
