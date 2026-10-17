idf_component_register(
    SRCS        "main.c"
                "lcd.c"
                "touch.c"
                "link.c"
                "ui.c"
                "rcon.c"
                "led.c"
                "console.c"
                # The OTA core is the monitor's, shared: chip-neutral, and it checks an
                # image against this firmware's own project name.
                "../../main/ota.c"
    INCLUDE_DIRS "."
    # font5x8.h is the monitor's own font: one bitmap font in the repository, not two.
    PRIV_INCLUDE_DIRS "../../main"
    REQUIRES     driver esp_lcd esp_timer bt nvs_flash esp_app_format app_update mbedtls
)
# The board tag is never referenced, and its section is not KEEP in the linker script.
target_link_libraries(${COMPONENT_LIB} "-u remote_board_tag")
