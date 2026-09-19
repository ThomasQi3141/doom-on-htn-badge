idf_component_register(
    SRCS "display.c" "buttons.c" "video.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_timer
)
