idf_component_register(
    SRCS        "ble_serial.c"
    INCLUDE_DIRS "include"
    REQUIRES     bt esp_hw_support nvs_flash
)
