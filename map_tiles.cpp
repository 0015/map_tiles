#include "map_tiles.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char* TAG = "map_tiles";

// Internal structure for map tiles instance
struct map_tiles_t {
    // Configuration
    char* base_path;
    char* tile_folders[MAP_TILES_MAX_TYPES];
    int tile_type_count;
    int current_tile_type;
    int grid_cols;
    int grid_rows;
    int tile_count;
    int zoom;
    bool use_spiram;
    bool initialized;
    
    // Tile management
    int tile_x;
    int tile_y;
    int marker_offset_x;
    int marker_offset_y;
    bool tile_loading_error;
    
    // Tile data - arrays will be allocated dynamically based on actual grid size
    uint8_t** tile_bufs;
    lv_image_dsc_t* tile_imgs;
};

map_tiles_handle_t map_tiles_init(const map_tiles_config_t* config)
{
    if (!config || !config->base_path || config->tile_type_count <= 0 || 
        config->tile_type_count > MAP_TILES_MAX_TYPES || 
        config->default_tile_type < 0 || config->default_tile_type >= config->tile_type_count) {
        ESP_LOGE(TAG, "Invalid configuration");
        return NULL;
    }
    
    // Validate grid size - use defaults if not specified or invalid
    int grid_cols = config->grid_cols;
    int grid_rows = config->grid_rows;
    
    if (grid_cols <= 0 || grid_cols > MAP_TILES_MAX_GRID_COLS) {
        ESP_LOGW(TAG, "Invalid grid_cols %d, using default %d", grid_cols, MAP_TILES_DEFAULT_GRID_COLS);
        grid_cols = MAP_TILES_DEFAULT_GRID_COLS;
    }
    
    if (grid_rows <= 0 || grid_rows > MAP_TILES_MAX_GRID_ROWS) {
        ESP_LOGW(TAG, "Invalid grid_rows %d, using default %d", grid_rows, MAP_TILES_DEFAULT_GRID_ROWS);
        grid_rows = MAP_TILES_DEFAULT_GRID_ROWS;
    }
    
    int tile_count = grid_cols * grid_rows;
    
    // Validate that all tile folders are provided
    for (int i = 0; i < config->tile_type_count; i++) {
        if (!config->tile_folders[i]) {
            ESP_LOGE(TAG, "Tile folder %d is NULL", i);
            return NULL;
        }
    }
    
    map_tiles_handle_t handle = (map_tiles_handle_t)calloc(1, sizeof(struct map_tiles_t));
    if (!handle) {
        ESP_LOGE(TAG, "Failed to allocate handle");
        return NULL;
    }
    
    // Copy base path
    handle->base_path = strdup(config->base_path);
    if (!handle->base_path) {
        ESP_LOGE(TAG, "Failed to allocate base path");
        free(handle);
        return NULL;
    }
    
    // Copy tile folder names
    handle->tile_type_count = config->tile_type_count;
    for (int i = 0; i < config->tile_type_count; i++) {
        handle->tile_folders[i] = strdup(config->tile_folders[i]);
        if (!handle->tile_folders[i]) {
            ESP_LOGE(TAG, "Failed to allocate tile folder %d", i);
            // Clean up previously allocated folders
            for (int j = 0; j < i; j++) {
                free(handle->tile_folders[j]);
            }
            free(handle->base_path);
            free(handle);
            return NULL;
        }
    }
    
    handle->zoom = config->default_zoom;
    handle->use_spiram = config->use_spiram;
    handle->current_tile_type = config->default_tile_type;
    handle->grid_cols = grid_cols;
    handle->grid_rows = grid_rows;
    handle->tile_count = tile_count;
    handle->initialized = true;
    handle->tile_loading_error = false;
    
    // Initialize tile data - allocate arrays based on actual tile count
    handle->tile_bufs = (uint8_t**)calloc(tile_count, sizeof(uint8_t*));
    handle->tile_imgs = (lv_image_dsc_t*)calloc(tile_count, sizeof(lv_image_dsc_t));
    
    if (!handle->tile_bufs || !handle->tile_imgs) {
        ESP_LOGE(TAG, "Failed to allocate tile arrays");
        // Clean up
        if (handle->tile_bufs) free(handle->tile_bufs);
        if (handle->tile_imgs) free(handle->tile_imgs);
        for (int i = 0; i < handle->tile_type_count; i++) {
            free(handle->tile_folders[i]);
        }
        free(handle->base_path);
        free(handle);
        return NULL;
    }
    
    ESP_LOGI(TAG, "Map tiles initialized with base path: %s, %d tile types, current type: %s, zoom: %d, grid: %dx%d", 
             handle->base_path, handle->tile_type_count, 
             handle->tile_folders[handle->current_tile_type], handle->zoom, 
             handle->grid_cols, handle->grid_rows);
    
    return handle;
}

void map_tiles_set_zoom(map_tiles_handle_t handle, int zoom_level)
{
    if (!handle || !handle->initialized) {
        ESP_LOGE(TAG, "Handle not initialized");
        return;
    }
    
    handle->zoom = zoom_level;
    ESP_LOGI(TAG, "Zoom level set to %d", zoom_level);
}

int map_tiles_get_zoom(map_tiles_handle_t handle)
{
    if (!handle || !handle->initialized) {
        ESP_LOGE(TAG, "Handle not initialized");
        return 0;
    }
    
    return handle->zoom;
}

bool map_tiles_set_tile_type(map_tiles_handle_t handle, int tile_type)
{
    if (!handle || !handle->initialized) {
        ESP_LOGE(TAG, "Handle not initialized");
        return false;
    }
    
    if (tile_type < 0 || tile_type >= handle->tile_type_count) {
        ESP_LOGE(TAG, "Invalid tile type: %d (valid range: 0-%d)", tile_type, handle->tile_type_count - 1);
        return false;
    }
    
    handle->current_tile_type = tile_type;
    ESP_LOGI(TAG, "Tile type set to %d (%s)", tile_type, handle->tile_folders[tile_type]);
    return true;
}

int map_tiles_get_tile_type(map_tiles_handle_t handle)
{
    if (!handle || !handle->initialized) {
        ESP_LOGE(TAG, "Handle not initialized");
        return -1;
    }
    
    return handle->current_tile_type;
}

int map_tiles_get_tile_type_count(map_tiles_handle_t handle)
{
    if (!handle || !handle->initialized) {
        ESP_LOGE(TAG, "Handle not initialized");
        return 0;
    }
    
    return handle->tile_type_count;
}

const char* map_tiles_get_tile_type_folder(map_tiles_handle_t handle, int tile_type)
{
    if (!handle || !handle->initialized) {
        ESP_LOGE(TAG, "Handle not initialized");
        return NULL;
    }
    
    if (tile_type < 0 || tile_type >= handle->tile_type_count) {
        ESP_LOGE(TAG, "Invalid tile type: %d", tile_type);
        return NULL;
    }
    
    return handle->tile_folders[tile_type];
}

bool map_tiles_load_tile(map_tiles_handle_t handle, int index, int tile_x, int tile_y)
{
    if (!handle || !handle->initialized) {
        ESP_LOGE(TAG, "Handle not initialized");
        return false;
    }
    
    if (index < 0 || index >= handle->tile_count) {
        ESP_LOGE(TAG, "Invalid tile index: %d", index);
        return false;
    }
    
    char path[256];
    const char* folder = handle->tile_folders[handle->current_tile_type];
    snprintf(path, sizeof(path), "%s/%s/%d/%d/%d.bin", 
             handle->base_path, folder, handle->zoom, tile_x, tile_y);
    
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "Tile not found: %s", path);
        return false;
    }
    
    // Skip 12-byte header
    fseek(f, 12, SEEK_SET);
    
    // Allocate buffer if needed
    if (!handle->tile_bufs[index]) {
        uint32_t caps = handle->use_spiram ? (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : MALLOC_CAP_DMA;
        handle->tile_bufs[index] = (uint8_t*)heap_caps_malloc(
            MAP_TILES_TILE_SIZE * MAP_TILES_TILE_SIZE * MAP_TILES_BYTES_PER_PIXEL, caps);
        
        if (!handle->tile_bufs[index]) {
            ESP_LOGE(TAG, "Tile %d: allocation failed", index);
            fclose(f);
            return false;
        }
    }
    
    // Clear buffer
    memset(handle->tile_bufs[index], 0, 
           MAP_TILES_TILE_SIZE * MAP_TILES_TILE_SIZE * MAP_TILES_BYTES_PER_PIXEL);
    
    // Read tile data
    size_t bytes_read = fread(handle->tile_bufs[index], 1, 
                             MAP_TILES_TILE_SIZE * MAP_TILES_TILE_SIZE * MAP_TILES_BYTES_PER_PIXEL, f);
    fclose(f);
    
    if (bytes_read != MAP_TILES_TILE_SIZE * MAP_TILES_TILE_SIZE * MAP_TILES_BYTES_PER_PIXEL) {
        ESP_LOGW(TAG, "Incomplete tile read: %zu bytes", bytes_read);
    }
    
    // Setup image descriptor
    handle->tile_imgs[index].header.w = MAP_TILES_TILE_SIZE;
    handle->tile_imgs[index].header.h = MAP_TILES_TILE_SIZE;
    handle->tile_imgs[index].header.cf = MAP_TILES_COLOR_FORMAT;
    handle->tile_imgs[index].header.stride = MAP_TILES_TILE_SIZE * MAP_TILES_BYTES_PER_PIXEL;
    handle->tile_imgs[index].data = (const uint8_t*)handle->tile_bufs[index];
    handle->tile_imgs[index].data_size = MAP_TILES_TILE_SIZE * MAP_TILES_TILE_SIZE * MAP_TILES_BYTES_PER_PIXEL;
    handle->tile_imgs[index].reserved = NULL;
    handle->tile_imgs[index].reserved_2 = NULL;
    
    ESP_LOGD(TAG, "Loaded tile %d from %s", index, path);
    return true;
}

void map_tiles_gps_to_tile_xy(map_tiles_handle_t handle, double lat, double lon, double* x, double* y)
{
    if (!handle || !handle->initialized) {
        ESP_LOGE(TAG, "Handle not initialized");
        return;
    }
    
    if (!x || !y) {
        ESP_LOGE(TAG, "Invalid output parameters");
        return;
    }
    
    double lat_rad = lat * M_PI / 180.0;
    int n = 1 << handle->zoom;
    *x = (lon + 180.0) / 360.0 * n;
    *y = (1.0 - log(tan(lat_rad) + 1.0 / cos(lat_rad)) / M_PI) / 2.0 * n;
}

void map_tiles_tile_xy_to_gps(map_tiles_handle_t handle, double x, double y, double* lat, double* lon)
{
    if (!handle || !handle->initialized) {
        ESP_LOGE(TAG, "Handle not initialized");
        return;
    }
    
    if (!lat || !lon) {
        ESP_LOGE(TAG, "Invalid output parameters");
        return;
    }
    
    int n = 1 << handle->zoom;
    *lon = x / n * 360.0 - 180.0;
    double lat_rad = atan(sinh(M_PI * (1 - 2 * y / n)));
    *lat = lat_rad * 180.0 / M_PI;
}

void map_tiles_get_center_gps(map_tiles_handle_t handle, double* lat, double* lon)
{
    if (!handle || !handle->initialized) {
        ESP_LOGE(TAG, "Handle not initialized");
        return;
    }
    
    if (!lat || !lon) {
        ESP_LOGE(TAG, "Invalid output parameters");
        return;
    }
    
    // Calculate center tile coordinates (center of the grid)
    double center_x = handle->tile_x + handle->grid_cols / 2.0;
    double center_y = handle->tile_y + handle->grid_rows / 2.0;
    
    // Convert to GPS coordinates
    map_tiles_tile_xy_to_gps(handle, center_x, center_y, lat, lon);
}

void map_tiles_set_center_from_gps(map_tiles_handle_t handle, double lat, double lon)
{
    if (!handle || !handle->initialized) {
        ESP_LOGE(TAG, "Handle not initialized");
        return;
    }
    
    double x, y;
    map_tiles_gps_to_tile_xy(handle, lat, lon, &x, &y);
    
    handle->tile_x = (int)x - handle->grid_cols / 2;
    handle->tile_y = (int)y - handle->grid_rows / 2;
    
    // Calculate pixel offset within the tile
    handle->marker_offset_x = (int)((x - (int)x) * MAP_TILES_TILE_SIZE);
    handle->marker_offset_y = (int)((y - (int)y) * MAP_TILES_TILE_SIZE);
    
    ESP_LOGI(TAG, "GPS to tile: tile_x=%d, tile_y=%d, offset_x=%d, offset_y=%d", 
             handle->tile_x, handle->tile_y, handle->marker_offset_x, handle->marker_offset_y);
}

bool map_tiles_is_gps_within_tiles(map_tiles_handle_t handle, double lat, double lon)
{
    if (!handle || !handle->initialized) {
        return false;
    }
    
    double x, y;
    map_tiles_gps_to_tile_xy(handle, lat, lon, &x, &y);
    
    int gps_tile_x = (int)x;
    int gps_tile_y = (int)y;
    
    bool within_x = (gps_tile_x >= handle->tile_x && gps_tile_x < handle->tile_x + handle->grid_cols);
    bool within_y = (gps_tile_y >= handle->tile_y && gps_tile_y < handle->tile_y + handle->grid_rows);
    
    return within_x && within_y;
}

void map_tiles_get_position(map_tiles_handle_t handle, int* tile_x, int* tile_y)
{
    if (!handle || !handle->initialized) {
        ESP_LOGE(TAG, "Handle not initialized");
        return;
    }
    
    if (tile_x) *tile_x = handle->tile_x;
    if (tile_y) *tile_y = handle->tile_y;
}

void map_tiles_set_position(map_tiles_handle_t handle, int tile_x, int tile_y)
{
    if (!handle || !handle->initialized) {
        ESP_LOGE(TAG, "Handle not initialized");
        return;
    }
    
    handle->tile_x = tile_x;
    handle->tile_y = tile_y;
}

void map_tiles_get_marker_offset(map_tiles_handle_t handle, int* offset_x, int* offset_y)
{
    if (!handle || !handle->initialized) {
        ESP_LOGE(TAG, "Handle not initialized");
        return;
    }
    
    if (offset_x) *offset_x = handle->marker_offset_x;
    if (offset_y) *offset_y = handle->marker_offset_y;
}

void map_tiles_set_marker_offset(map_tiles_handle_t handle, int offset_x, int offset_y)
{
    if (!handle || !handle->initialized) {
        ESP_LOGE(TAG, "Handle not initialized");
        return;
    }
    
    handle->marker_offset_x = offset_x;
    handle->marker_offset_y = offset_y;
}

lv_image_dsc_t* map_tiles_get_image(map_tiles_handle_t handle, int index)
{
    if (!handle || !handle->initialized || index < 0 || index >= handle->tile_count) {
        return NULL;
    }
    
    return &handle->tile_imgs[index];
}

uint8_t* map_tiles_get_buffer(map_tiles_handle_t handle, int index)
{
    if (!handle || !handle->initialized || index < 0 || index >= handle->tile_count) {
        return NULL;
    }
    
    return handle->tile_bufs[index];
}

void map_tiles_set_loading_error(map_tiles_handle_t handle, bool error)
{
    if (!handle || !handle->initialized) {
        ESP_LOGE(TAG, "Handle not initialized");
        return;
    }
    
    handle->tile_loading_error = error;
}

bool map_tiles_has_loading_error(map_tiles_handle_t handle)
{
    if (!handle || !handle->initialized) {
        return true;
    }
    
    return handle->tile_loading_error;
}

void map_tiles_cleanup(map_tiles_handle_t handle)
{
    if (!handle) {
        return;
    }
    
    if (handle->initialized) {
        // Free tile buffers
        if (handle->tile_bufs) {
            for (int i = 0; i < handle->tile_count; i++) {
                if (handle->tile_bufs[i]) {
                    heap_caps_free(handle->tile_bufs[i]);
                    handle->tile_bufs[i] = NULL;
                }
            }
            free(handle->tile_bufs);
            handle->tile_bufs = NULL;
        }
        
        // Free tile image descriptors array
        if (handle->tile_imgs) {
            free(handle->tile_imgs);
            handle->tile_imgs = NULL;
        }
        
        handle->initialized = false;
        ESP_LOGI(TAG, "Map tiles cleaned up");
    }
    
    // Free base path and folder names, then handle
    if (handle->base_path) {
        free(handle->base_path);
    }
    for (int i = 0; i < handle->tile_type_count; i++) {
        if (handle->tile_folders[i]) {
            free(handle->tile_folders[i]);
        }
    }
    free(handle);
}

void map_tiles_get_grid_size(map_tiles_handle_t handle, int* cols, int* rows)
{
    if (!handle || !handle->initialized || !cols || !rows) {
        if (cols) *cols = 0;
        if (rows) *rows = 0;
        return;
    }
    
    *cols = handle->grid_cols;
    *rows = handle->grid_rows;
}

int map_tiles_get_tile_count(map_tiles_handle_t handle)
{
    if (!handle || !handle->initialized) {
        return 0;
    }
    
    return handle->tile_count;
}
