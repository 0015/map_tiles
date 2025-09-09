#include "map_tiles.h"
#include "lvgl.h"
#include "esp_log.h"

static const char* TAG = "map_example";

// Map tiles handle
static map_tiles_handle_t map_handle = NULL;

// LVGL objects for displaying tiles
static lv_obj_t* map_container = NULL;
static lv_obj_t** tile_images = NULL;  // Dynamic array for configurable grid
static int grid_cols = 0, grid_rows = 0, tile_count = 0;

/**
 * @brief Initialize the map display
 */
void map_display_init(void)
{
    // Configure map tiles with multiple tile types and custom grid size
    const char* tile_folders[] = {"street_map", "satellite", "terrain", "hybrid"};
    map_tiles_config_t config = {
        .base_path = "/sdcard",
        .tile_folders = {tile_folders[0], tile_folders[1], tile_folders[2], tile_folders[3]},
        .tile_type_count = 4,
        .default_zoom = 10,
        .use_spiram = true,
        .default_tile_type = 0,  // Start with street map
        .grid_cols = 5,          // 5x5 grid (configurable)
        .grid_rows = 5
    };
    
    // Initialize map tiles
    map_handle = map_tiles_init(&config);
    if (!map_handle) {
        ESP_LOGE(TAG, "Failed to initialize map tiles");
        return;
    }
    
    // Get grid dimensions
    map_tiles_get_grid_size(map_handle, &grid_cols, &grid_rows);
    tile_count = map_tiles_get_tile_count(map_handle);
    
    // Allocate tile images array
    tile_images = malloc(tile_count * sizeof(lv_obj_t*));
    if (!tile_images) {
        ESP_LOGE(TAG, "Failed to allocate tile images array");
        map_tiles_cleanup(map_handle);
        return;
    }
    
    // Create map container
    map_container = lv_obj_create(lv_screen_active());
    lv_obj_set_size(map_container, 
                   grid_cols * MAP_TILES_TILE_SIZE,
                   grid_rows * MAP_TILES_TILE_SIZE);
    lv_obj_center(map_container);
    lv_obj_set_style_pad_all(map_container, 0, 0);
    lv_obj_set_style_border_width(map_container, 0, 0);
    
    // Create image widgets for each tile
    for (int i = 0; i < tile_count; i++) {
        tile_images[i] = lv_image_create(map_container);
        
        // Position tile in grid
        int row = i / grid_cols;
        int col = i % grid_cols;
        lv_obj_set_pos(tile_images[i], 
                      col * MAP_TILES_TILE_SIZE,
                      row * MAP_TILES_TILE_SIZE);
        lv_obj_set_size(tile_images[i], MAP_TILES_TILE_SIZE, MAP_TILES_TILE_SIZE);
    }
    
    ESP_LOGI(TAG, "Map display initialized");
}

/**
 * @brief Load and display map tiles for a GPS location
 * 
 * @param lat Latitude in degrees
 * @param lon Longitude in degrees
 */
void map_display_load_location(double lat, double lon)
{
    if (!map_handle) {
        ESP_LOGE(TAG, "Map not initialized");
        return;
    }
    
    ESP_LOGI(TAG, "Loading map for GPS: %.6f, %.6f", lat, lon);
    
    // Set center from GPS coordinates
    map_tiles_set_center_from_gps(map_handle, lat, lon);
    
    // Get current tile position
    int base_tile_x, base_tile_y;
    map_tiles_get_position(map_handle, &base_tile_x, &base_tile_y);
    
    // Load tiles in a configurable grid
    for (int row = 0; row < grid_rows; row++) {
        for (int col = 0; col < grid_cols; col++) {
            int index = row * grid_cols + col;
            int tile_x = base_tile_x + col;
            int tile_y = base_tile_y + row;
            
            // Load the tile
            bool loaded = map_tiles_load_tile(map_handle, index, tile_x, tile_y);
            if (loaded) {
                // Update the image widget
                lv_image_dsc_t* img_dsc = map_tiles_get_image(map_handle, index);
                if (img_dsc) {
                    lv_image_set_src(tile_images[index], img_dsc);
                    ESP_LOGD(TAG, "Loaded tile %d (%d, %d)", index, tile_x, tile_y);
                }
            } else {
                ESP_LOGW(TAG, "Failed to load tile %d (%d, %d)", index, tile_x, tile_y);
                // Set a placeholder or clear the image
                lv_image_set_src(tile_images[index], NULL);
            }
        }
    }
    
    ESP_LOGI(TAG, "Map tiles loaded for location");
}

/**
 * @brief Set the map tile type and reload tiles
 * 
 * @param tile_type Tile type index (0=street, 1=satellite, 2=terrain, 3=hybrid)
 * @param lat Current latitude
 * @param lon Current longitude
 */
void map_display_set_tile_type(int tile_type, double lat, double lon)
{
    if (!map_handle) {
        ESP_LOGE(TAG, "Map not initialized");
        return;
    }
    
    // Validate tile type
    int max_types = map_tiles_get_tile_type_count(map_handle);
    if (tile_type < 0 || tile_type >= max_types) {
        ESP_LOGW(TAG, "Invalid tile type %d (valid range: 0-%d)", tile_type, max_types - 1);
        return;
    }
    
    ESP_LOGI(TAG, "Setting tile type to %d (%s)", tile_type, 
             map_tiles_get_tile_type_folder(map_handle, tile_type));
    
    // Set tile type
    if (map_tiles_set_tile_type(map_handle, tile_type)) {
        // Reload tiles for the new type
        map_display_load_location(lat, lon);
    }
}

/**
 * @brief Set the zoom level and reload tiles
 * 
 * @param zoom New zoom level
 * @param lat Current latitude
 * @param lon Current longitude
 */
void map_display_set_zoom(int zoom, double lat, double lon)
{
    if (!map_handle) {
        ESP_LOGE(TAG, "Map not initialized");
        return;
    }
    
    ESP_LOGI(TAG, "Setting zoom to %d", zoom);
    
    // Update zoom level
    map_tiles_set_zoom(map_handle, zoom);
    
    // Reload tiles for the new zoom level
    map_display_load_location(lat, lon);
}

/**
 * @brief Add a GPS marker to the map
 * 
 * @param lat Latitude in degrees
 * @param lon Longitude in degrees
 */
void map_display_add_marker(double lat, double lon)
{
    if (!map_handle) {
        ESP_LOGE(TAG, "Map not initialized");
        return;
    }
    
    // Check if GPS position is within current tiles
    if (!map_tiles_is_gps_within_tiles(map_handle, lat, lon)) {
        ESP_LOGW(TAG, "GPS position outside current tiles, reloading map");
        map_display_load_location(lat, lon);
        return;
    }
    
    // Get marker offset within the current tile grid
    int offset_x, offset_y;
    map_tiles_get_marker_offset(map_handle, &offset_x, &offset_y);
    
    // Create or update marker object
    static lv_obj_t* marker = NULL;
    if (!marker) {
        marker = lv_obj_create(map_container);
        lv_obj_set_size(marker, 10, 10);
        lv_obj_set_style_bg_color(marker, lv_color_hex(0xFF0000), 0);
        lv_obj_set_style_radius(marker, 5, 0);
        lv_obj_set_style_border_width(marker, 1, 0);
        lv_obj_set_style_border_color(marker, lv_color_hex(0xFFFFFF), 0);
    }
    
    // Position marker based on GPS offset
    int center_tile_col = grid_cols / 2;
    int center_tile_row = grid_rows / 2;
    int marker_x = center_tile_col * MAP_TILES_TILE_SIZE + offset_x - 5;
    int marker_y = center_tile_row * MAP_TILES_TILE_SIZE + offset_y - 5;
    
    lv_obj_set_pos(marker, marker_x, marker_y);
    
    ESP_LOGI(TAG, "GPS marker positioned at (%d, %d)", marker_x, marker_y);
}

/**
 * @brief Clean up map display resources
 */
void map_display_cleanup(void)
{
    if (tile_images) {
        free(tile_images);
        tile_images = NULL;
    }
    
    if (map_handle) {
        map_tiles_cleanup(map_handle);
        map_handle = NULL;
    }
    
    if (map_container) {
        lv_obj_delete(map_container);
        map_container = NULL;
    }
    
    grid_cols = grid_rows = tile_count = 0;
    
    ESP_LOGI(TAG, "Map display cleaned up");
}

/**
 * @brief Example usage in main application
 */
void app_main(void)
{
    // Initialize LVGL and display driver first...
    
    // Initialize map display
    map_display_init();
    
    // Load map for San Francisco
    double lat = 37.7749;
    double lon = -122.4194;
    map_display_load_location(lat, lon);
    
    // Add GPS marker
    map_display_add_marker(lat, lon);
    
    // Example: Change to satellite view (tile type 1)
    // map_display_set_tile_type(1, lat, lon);
    
    // Example: Change to terrain view (tile type 2)
    // map_display_set_tile_type(2, lat, lon);
    
    // Example: Change zoom level
    // map_display_set_zoom(12, lat, lon);
    
    // Example: Update GPS position
    // map_display_add_marker(37.7849, -122.4094);
    
    // NOTE: To use different grid sizes, modify the grid_cols and grid_rows
    // in the config structure above. Examples:
    // - 3x3 grid: .grid_cols = 3, .grid_rows = 3  (9 tiles, ~1.1MB RAM)
    // - 5x5 grid: .grid_cols = 5, .grid_rows = 5  (25 tiles, ~3.1MB RAM)
    // - 7x7 grid: .grid_cols = 7, .grid_rows = 7  (49 tiles, ~6.1MB RAM)
}
