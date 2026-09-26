idf_component_register(
    SRCS "map_tiles.cpp"
         "map_geo.c"
         "map_route.c"
         "map_tile_cache.c"
         "map_view.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl esp_system
    PRIV_REQUIRES vfs fatfs
)
