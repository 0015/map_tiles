idf_component_register(
    SRCS "map_tiles.cpp"
    INCLUDE_DIRS "include"
    REQUIRES lvgl esp_system
    PRIV_REQUIRES vfs fatfs
)
