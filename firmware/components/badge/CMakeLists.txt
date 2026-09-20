idf_component_register(
    SRCS "display.c" "buttons.c" "video.c" "badge_radio.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_timer esp_wifi esp_event nvs_flash
)
