idf_component_register(
    SRCS        "main.c"
                "cli.c"
                "cli_sense.c"
                "cli_stream.c"
                "cli_gauge.c"
                "cli_cal.c"
                "cli_report.c"
                "lcd.c"
                "ble.c"
                "values.c"
                "history_values.c"
                "config.c"
                "ota.c"
    INCLUDE_DIRS "."
    REQUIRES     driver esp_timer console bt esp_hw_support nvs_flash esp_app_format app_update mbedtls esp_driver_usb_serial_jtag vfs ina219 sensors fuelgauge bme280
)
