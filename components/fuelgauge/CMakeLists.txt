idf_component_register(
    SRCS        "fuelgauge.c"
    INCLUDE_DIRS "include"
    REQUIRES     sensors nvs_flash
)
