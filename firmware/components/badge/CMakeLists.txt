idf_component_register(
    SRCS "display.c" "buttons.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_timer
)
