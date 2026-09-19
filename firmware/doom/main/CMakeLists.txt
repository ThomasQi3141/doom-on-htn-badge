idf_component_register(
    SRCS "main.c" "wad.c" "video.c"
    INCLUDE_DIRS "."
    REQUIRES badge spi_flash esp_partition esp_timer
)
