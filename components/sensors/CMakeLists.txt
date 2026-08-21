idf_component_register(
    SRCS        "sensors.c"
    INCLUDE_DIRS "include"
    REQUIRES     driver esp_timer ina219
)
