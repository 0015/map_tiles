# Map Tiles Component for LVGL 9.x

A comprehensive map tiles component for ESP-IDF projects using LVGL 9.x. This component provides functionality to load and display map tiles with GPS coordinate conversion, designed for embedded applications requiring offline map display capabilities.

## Features

- **LVGL 9.x Compatible**: Fully compatible with LVGL 9.x image handling
- **GPS Coordinate Conversion**: Convert GPS coordinates to tile coordinates and vice versa
- **Dynamic Tile Loading**: Load map tiles on demand from file system
- **Configurable Grid Size**: Support for different grid sizes (3x3, 5x5, 7x7, etc.)
- **Multiple Tile Types**: Support for up to 8 different tile types (street, satellite, terrain, hybrid, etc.)
- **Memory Efficient**: Configurable memory allocation (SPIRAM or regular RAM)
- **Multiple Zoom Levels**: Support for different map zoom levels
- **Error Handling**: Comprehensive error handling and logging
- **C API**: Clean C API for easy integration

## Requirements

- ESP-IDF 5.0 or later
- LVGL 9.3
- File system support (FAT/SPIFFS/LittleFS)
- Map tiles in binary format (RGB565, 256x256 pixels)

## Installation

### Using ESP-IDF Component Manager

You can easily add this component to your project using the idf.py command or by manually updating your idf_component.yml file.

#### Option 1: Using the idf.py add-dependency command (Recommended)
From your project's root directory, simply run the following command in your terminal:

```bash
idf.py add-dependency "0015/map_tiles^1.1.0"
```

This command will automatically add the component to your idf_component.yml file and download the required files the next time you build your project.

#### Option 2: Manual idf_component.yml update
Add to your project's `main/idf_component.yml`:

```yaml
dependencies:
  map_tiles:
    git: "https://github.com/0015/map_tiles.git"
    version: "^1.1.0"
```

### Manual Installation

1. Copy the `map_tiles` folder to your project's `components` directory
2. The component will be automatically included in your build

## Usage

### Basic Setup

```c
#include "map_tiles.h"

// Configure the map tiles with multiple tile types and custom grid size
const char* tile_folders[] = {"street_map", "satellite", "terrain", "hybrid"};
map_tiles_config_t config = {
    .base_path = "/sdcard",                    // Base path to tile storage
    .tile_folders = {tile_folders[0], tile_folders[1], tile_folders[2], tile_folders[3]},
    .tile_type_count = 4,                      // Number of tile types
    .default_zoom = 10,                        // Default zoom level
    .use_spiram = true,                       // Use SPIRAM if available
    .default_tile_type = 0,                   // Start with street map (index 0)
    .grid_cols = 5,                           // Grid width (tiles)
    .grid_rows = 5                            // Grid height (tiles)
};

// Initialize map tiles
map_tiles_handle_t map_handle = map_tiles_init(&config);
if (!map_handle) {
    ESP_LOGE(TAG, "Failed to initialize map tiles");
    return;
}
```

### Loading Tiles

```c
// Set center position from GPS coordinates
map_tiles_set_center_from_gps(map_handle, 37.7749, -122.4194); // San Francisco

// Get grid dimensions
int grid_cols, grid_rows;
map_tiles_get_grid_size(map_handle, &grid_cols, &grid_rows);
int tile_count = map_tiles_get_tile_count(map_handle);

// Load tiles for the configured grid size
for (int row = 0; row < grid_rows; row++) {
    for (int col = 0; col < grid_cols; col++) {
        int index = row * grid_cols + col;
        int tile_x, tile_y;
        map_tiles_get_position(map_handle, &tile_x, &tile_y);
        
        bool loaded = map_tiles_load_tile(map_handle, index, 
                                         tile_x + col, tile_y + row);
        if (!loaded) {
            ESP_LOGW(TAG, "Failed to load tile %d", index);
        }
    }
}
```

### Displaying Tiles with LVGL

```c
// Get grid dimensions and tile count
int grid_cols, grid_rows;
map_tiles_get_grid_size(map_handle, &grid_cols, &grid_rows);
int tile_count = map_tiles_get_tile_count(map_handle);

// Create image widgets for each tile
lv_obj_t** tile_images = malloc(tile_count * sizeof(lv_obj_t*));

for (int i = 0; i < tile_count; i++) {
    tile_images[i] = lv_image_create(parent_container);
    
    // Get the tile image descriptor
    lv_image_dsc_t* img_dsc = map_tiles_get_image(map_handle, i);
    if (img_dsc) {
        lv_image_set_src(tile_images[i], img_dsc);
        
        // Position the tile in the grid
        int row = i / grid_cols;
        int col = i % grid_cols;
        lv_obj_set_pos(tile_images[i], 
                      col * MAP_TILES_TILE_SIZE, 
                      row * MAP_TILES_TILE_SIZE);
    }
}
```

### Switching Tile Types

```c
// Switch to different tile types
map_tiles_set_tile_type(map_handle, 0);  // Street map
map_tiles_set_tile_type(map_handle, 1);  // Satellite
map_tiles_set_tile_type(map_handle, 2);  // Terrain
map_tiles_set_tile_type(map_handle, 3);  // Hybrid

// Get current tile type
int current_type = map_tiles_get_tile_type(map_handle);

// Get available tile types
int type_count = map_tiles_get_tile_type_count(map_handle);
for (int i = 0; i < type_count; i++) {
    const char* folder = map_tiles_get_tile_type_folder(map_handle, i);
    printf("Tile type %d: %s\n", i, folder);
}
```

### GPS Coordinate Conversion

```c
// Convert GPS to tile coordinates
double tile_x, tile_y;
map_tiles_gps_to_tile_xy(map_handle, 37.7749, -122.4194, &tile_x, &tile_y);

// Check if GPS position is within current tile grid
bool within_tiles = map_tiles_is_gps_within_tiles(map_handle, 37.7749, -122.4194);

// Get marker offset for precise positioning
int offset_x, offset_y;
map_tiles_get_marker_offset(map_handle, &offset_x, &offset_y);
```

### Memory Management

```c
// Clean up when done
map_tiles_cleanup(map_handle);
```

## Tile File Format

The component expects map tiles in a specific binary format:

- **File Structure**: `{base_path}/{map_tile}/{zoom}/{tile_x}/{tile_y}.bin`
- **Format**: 12-byte header + raw RGB565 pixel data
- **Size**: 256x256 pixels
- **Color Format**: RGB565 (16-bit per pixel)

### Example Tile Structure
```
/sdcard/
├── street_map/       // Tile type 0
│   ├── 10/
│   │   ├── 164/
│   │   │   ├── 395.bin
│   │   │   ├── 396.bin
│   │   │   └── ...
│   │   └── ...
│   └── ...
├── satellite/        // Tile type 1
│   ├── 10/
│   │   ├── 164/
│   │   │   ├── 395.bin
│   │   │   └── ...
│   │   └── ...
│   └── ...
├── terrain/          // Tile type 2
│   └── ...
└── hybrid/           // Tile type 3
    └── ...
```

## Configuration Options

| Parameter | Type | Description | Default |
|-----------|------|-------------|---------|
| `base_path` | `const char*` | Base directory for tile storage | Required |
| `tile_folders` | `const char*[]` | Array of folder names for different tile types | Required |
| `tile_type_count` | `int` | Number of tile types (max 8) | Required |
| `default_zoom` | `int` | Initial zoom level | Required |
| `use_spiram` | `bool` | Use SPIRAM for tile buffers | `false` |
| `default_tile_type` | `int` | Initial tile type index | Required |
| `grid_cols` | `int` | Number of tile columns (max 10) | 5 |
| `grid_rows` | `int` | Number of tile rows (max 10) | 5 |

## API Reference

### Initialization
- `map_tiles_init()` - Initialize map tiles system
- `map_tiles_cleanup()` - Clean up resources

### Tile Management
- `map_tiles_load_tile()` - Load a specific tile
- `map_tiles_get_image()` - Get LVGL image descriptor
- `map_tiles_get_buffer()` - Get raw tile buffer

### Grid Management
- `map_tiles_get_grid_size()` - Get current grid dimensions
- `map_tiles_get_tile_count()` - Get total number of tiles in grid

### Coordinate Conversion
- `map_tiles_gps_to_tile_xy()` - Convert GPS to tile coordinates
- `map_tiles_set_center_from_gps()` - Set center from GPS
- `map_tiles_is_gps_within_tiles()` - Check if GPS is within current tiles

### Position Management
- `map_tiles_get_position()` - Get current tile position
- `map_tiles_set_position()` - Set tile position
- `map_tiles_get_marker_offset()` - Get marker offset
- `map_tiles_set_marker_offset()` - Set marker offset

### Tile Type Management
- `map_tiles_set_tile_type()` - Set active tile type
- `map_tiles_get_tile_type()` - Get current tile type
- `map_tiles_get_tile_type_count()` - Get number of available types
- `map_tiles_get_tile_type_folder()` - Get folder name for a type

### Zoom Control
- `map_tiles_set_zoom()` - Set zoom level
- `map_tiles_get_zoom()` - Get current zoom level

### Error Handling
- `map_tiles_set_loading_error()` - Set error state
- `map_tiles_has_loading_error()` - Check error state

## Performance Considerations

- **Memory Usage**: Each tile uses ~128KB (256×256×2 bytes)
- **Grid Size**: Larger grids use more memory (3x3=9 tiles, 5x5=25 tiles, 7x7=49 tiles)
- **SPIRAM**: Recommended for ESP32-S3 with PSRAM for better performance
- **File System**: Ensure adequate file system performance for tile loading
- **Tile Caching**: Component maintains tile buffers until cleanup

## Example Projects

See the `examples` directory for complete implementation examples:
- Basic map display
- GPS tracking with map updates
- Interactive map with touch controls

## License

This component is released under the MIT License. See LICENSE file for details.

## Contributing

Contributions are welcome! Please feel free to submit pull requests or open issues for bugs and feature requests.

## Support

For questions and support, please open an issue on the GitHub repository.
