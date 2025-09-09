#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Map tiles component for LVGL 9.x
 * 
 * This component provides functionality to load and display map tiles with GPS coordinate conversion.
 * Tiles are assumed to be 256x256 pixels in RGB565 format, stored in binary files.
 */

// Constants
#define MAP_TILES_TILE_SIZE 256
#define MAP_TILES_DEFAULT_GRID_COLS 5
#define MAP_TILES_DEFAULT_GRID_ROWS 5
#define MAP_TILES_MAX_GRID_COLS 9
#define MAP_TILES_MAX_GRID_ROWS 9
#define MAP_TILES_MAX_TILES (MAP_TILES_MAX_GRID_COLS * MAP_TILES_MAX_GRID_ROWS)
#define MAP_TILES_BYTES_PER_PIXEL 2
#define MAP_TILES_COLOR_FORMAT LV_COLOR_FORMAT_RGB565
#define MAP_TILES_MAX_TYPES 8
#define MAP_TILES_MAX_FOLDER_NAME 32

/**
 * @brief Configuration structure for map tiles
 */
typedef struct {
    const char* base_path;                                          /**< Base path where tiles are stored (e.g., "/sdcard") */
    const char* tile_folders[MAP_TILES_MAX_TYPES];                 /**< Array of folder names for different tile types */
    int tile_type_count;                                           /**< Number of tile types configured */
    int grid_cols;                                                  /**< Number of tile columns (default: 5, max: 9) */
    int grid_rows;                                                  /**< Number of tile rows (default: 5, max: 9) */
    int default_zoom;                                               /**< Default zoom level */
    bool use_spiram;                                               /**< Whether to use SPIRAM for tile buffers */
    int default_tile_type;                                         /**< Default tile type index (0 to tile_type_count-1) */
} map_tiles_config_t;

/**
 * @brief Map tiles handle
 */
typedef struct map_tiles_t* map_tiles_handle_t;

/**
 * @brief Initialize the map tiles system
 * 
 * @param config Configuration structure
 * @return map_tiles_handle_t Handle to the map tiles instance, NULL on failure
 */
map_tiles_handle_t map_tiles_init(const map_tiles_config_t* config);

/**
 * @brief Set the zoom level
 * 
 * @param handle Map tiles handle
 * @param zoom_level Zoom level (typically 0-18)
 */
void map_tiles_set_zoom(map_tiles_handle_t handle, int zoom_level);

/**
 * @brief Get the current zoom level
 * 
 * @param handle Map tiles handle
 * @return Current zoom level
 */
int map_tiles_get_zoom(map_tiles_handle_t handle);

/**
 * @brief Set tile type
 * 
 * @param handle Map tiles handle
 * @param tile_type Tile type index (0 to configured tile_type_count-1)
 * @return true if tile type was set successfully, false otherwise
 */
bool map_tiles_set_tile_type(map_tiles_handle_t handle, int tile_type);

/**
 * @brief Get current tile type
 * 
 * @param handle Map tiles handle
 * @return Current tile type index, -1 if error
 */
int map_tiles_get_tile_type(map_tiles_handle_t handle);

/**
 * @brief Get grid dimensions
 * 
 * @param handle Map tiles handle
 * @param cols Output grid columns (can be NULL)
 * @param rows Output grid rows (can be NULL)
 */
void map_tiles_get_grid_size(map_tiles_handle_t handle, int* cols, int* rows);

/**
 * @brief Get total tile count for current grid
 * 
 * @param handle Map tiles handle
 * @return Total number of tiles in the grid, 0 if error
 */
int map_tiles_get_tile_count(map_tiles_handle_t handle);

/**
 * @brief Get tile type count
 * 
 * @param handle Map tiles handle
 * @return Number of configured tile types, 0 if error
 */
int map_tiles_get_tile_type_count(map_tiles_handle_t handle);

/**
 * @brief Get tile type folder name
 * 
 * @param handle Map tiles handle
 * @param tile_type Tile type index
 * @return Folder name for the tile type, NULL if invalid
 */
const char* map_tiles_get_tile_type_folder(map_tiles_handle_t handle, int tile_type);

/**
 * @brief Load a specific tile dynamically
 * 
 * @param handle Map tiles handle
 * @param index Tile index (0 to total_tile_count-1)
 * @param tile_x Tile X coordinate
 * @param tile_y Tile Y coordinate
 * @return true if tile loaded successfully, false otherwise
 */
bool map_tiles_load_tile(map_tiles_handle_t handle, int index, int tile_x, int tile_y);

/**
 * @brief Convert GPS coordinates to tile coordinates
 * 
 * @param handle Map tiles handle
 * @param lat Latitude in degrees
 * @param lon Longitude in degrees
 * @param x Output tile X coordinate
 * @param y Output tile Y coordinate
 */
void map_tiles_gps_to_tile_xy(map_tiles_handle_t handle, double lat, double lon, double* x, double* y);

/**
 * @brief Convert tile coordinates to GPS coordinates
 * 
 * @param handle Map tiles handle
 * @param x Tile X coordinate
 * @param y Tile Y coordinate
 * @param lat Output latitude in degrees
 * @param lon Output longitude in degrees
 */
void map_tiles_tile_xy_to_gps(map_tiles_handle_t handle, double x, double y, double* lat, double* lon);

/**
 * @brief Get center GPS coordinates of current map view
 * 
 * @param handle Map tiles handle
 * @param lat Output latitude in degrees
 * @param lon Output longitude in degrees
 */
void map_tiles_get_center_gps(map_tiles_handle_t handle, double* lat, double* lon);

/**
 * @brief Set the tile center from GPS coordinates
 * 
 * @param handle Map tiles handle
 * @param lat Latitude in degrees
 * @param lon Longitude in degrees
 */
void map_tiles_set_center_from_gps(map_tiles_handle_t handle, double lat, double lon);

/**
 * @brief Check if GPS coordinates are within current tile grid
 * 
 * @param handle Map tiles handle
 * @param lat Latitude in degrees
 * @param lon Longitude in degrees
 * @return true if GPS position is within current tiles, false otherwise
 */
bool map_tiles_is_gps_within_tiles(map_tiles_handle_t handle, double lat, double lon);

/**
 * @brief Get current tile position
 * 
 * @param handle Map tiles handle
 * @param tile_x Output tile X coordinate (can be NULL)
 * @param tile_y Output tile Y coordinate (can be NULL)
 */
void map_tiles_get_position(map_tiles_handle_t handle, int* tile_x, int* tile_y);

/**
 * @brief Set tile position
 * 
 * @param handle Map tiles handle
 * @param tile_x Tile X coordinate
 * @param tile_y Tile Y coordinate
 */
void map_tiles_set_position(map_tiles_handle_t handle, int tile_x, int tile_y);

/**
 * @brief Get marker offset within the current tile
 * 
 * @param handle Map tiles handle
 * @param offset_x Output X offset in pixels (can be NULL)
 * @param offset_y Output Y offset in pixels (can be NULL)
 */
void map_tiles_get_marker_offset(map_tiles_handle_t handle, int* offset_x, int* offset_y);

/**
 * @brief Set marker offset within the current tile
 * 
 * @param handle Map tiles handle
 * @param offset_x X offset in pixels
 * @param offset_y Y offset in pixels
 */
void map_tiles_set_marker_offset(map_tiles_handle_t handle, int offset_x, int offset_y);

/**
 * @brief Get tile image descriptor
 * 
 * @param handle Map tiles handle
 * @param index Tile index (0 to total_tile_count-1)
 * @return Pointer to LVGL image descriptor, NULL if invalid
 */
lv_image_dsc_t* map_tiles_get_image(map_tiles_handle_t handle, int index);

/**
 * @brief Get tile buffer
 * 
 * @param handle Map tiles handle
 * @param index Tile index (0 to total_tile_count-1)
 * @return Pointer to tile buffer, NULL if invalid
 */
uint8_t* map_tiles_get_buffer(map_tiles_handle_t handle, int index);

/**
 * @brief Set tile loading error state
 * 
 * @param handle Map tiles handle
 * @param error Error state
 */
void map_tiles_set_loading_error(map_tiles_handle_t handle, bool error);

/**
 * @brief Check if there's a tile loading error
 * 
 * @param handle Map tiles handle
 * @return true if there's an error, false otherwise
 */
bool map_tiles_has_loading_error(map_tiles_handle_t handle);

/**
 * @brief Clean up and free map tiles resources
 * 
 * @param handle Map tiles handle
 */
void map_tiles_cleanup(map_tiles_handle_t handle);

#ifdef __cplusplus
}
#endif
