idf_component_register(
    SRCS "radio.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_event esp_timer
)
