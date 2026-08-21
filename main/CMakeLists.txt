idf_component_register(
    SRCS        "main.c"
                "console_cmds.c"
                "display_debug.c"
                "ble_console.c"
                "cal_store.c"
    INCLUDE_DIRS "."
    REQUIRES     driver esp_timer console nvs_flash ina219 sensors ssd1306 ble_serial fuelgauge
)
