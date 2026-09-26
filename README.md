# Map Tiles Component for LVGL 9.x

Offline maps on ESP32, over LVGL 9.x. Tiles come off an SD card, GPS coordinates
turn into screen positions, and nothing needs a network or an API key.

**v2 adds turn-by-turn navigation**: a route drawn on the map, a marker that
follows it, and the next turn - still entirely offline.

## v1 and v2

**One component, two APIs.** v2.0.0 *added* the navigation layer; it changed no
function, no struct and no behaviour in `map_tiles.h`. Every v1 project builds
untouched against v2.0.0 — the major version marks the new surface, not a break.
Nothing forces a choice either: both headers can be used in one firmware, and
they share the tile format and the SD card layout.

| | **v1 API** — `map_tiles.h` | **v2 API** — the navigation layer |
|---|---|---|
| Headers | `map_tiles.h` | `map_view.h`, `map_route.h`, `map_tile_cache.h`, `map_geo.h` |
| Added in | 1.0.0 | **2.0.0** |
| Shape | a fixed grid of `lv_image` widgets, one per tile | one `map_view` widget that draws every tile itself |
| View centre | integer tile index | continuous world-pixel coordinate |
| Panning | 256 px jumps when the grid re-anchors | smooth, with fling momentum |
| Tile loading | the whole grid, synchronously, on the UI thread | per tile, LRU cached, on a background task |
| A cache miss | blocks until the SD card answers | draws a placeholder and queues a read |
| Position | a marker you place yourself | a GPS fix snapped onto a route |
| Routes | — | polyline with per-zoom detail, map matching, turn instructions |
| Off route | — | detected, with the distance and direction back |
| Memory | one buffer per grid slot, all resident | a bounded LRU, 128 KB per slot |
| Built for | a map you drag by hand | a map that keeps up with a moving vehicle |
| Read | [Usage](#usage) below | [Navigation layer (v2)](#navigation-layer-v2) below |

Pick v1 when you want a map to look at: a viewer, a tracker, a position on a
screen. Pick v2 when something is moving and the map has to keep up with it.

## Recent Updates

**v2.0.0 (September 25, 2026)**
- **New navigation layer, added alongside the existing API - nothing was changed or removed.**
  - `map_geo` - Web Mercator, haversine, bearings, point-to-segment projection
  - `map_route` - offline `route.bin` loading, per-zoom level of detail, map matching, turn instructions
  - `map_tile_cache` - LRU tile cache with a background loader task, so the UI thread never blocks on the SD card
  - `map_view` - a continuous-coordinate LVGL widget that draws tiles, route and marker itself
- Major version because the component now carries two rendering paths, not
  because anything broke. The v1 API is unchanged, source-compatible, and still
  the right answer for a map you pan by hand.
- See [Navigation layer (v2)](#navigation-layer-v2) below.

**v1.3.0 (December 20, 2025)**
- **Fixed critical marker positioning bugs in example code:**
  - Bug Fix #1: Corrected `map_display_add_marker()` to calculate position from actual GPS coordinates instead of using stored offset
  - Bug Fix #2: Added scroll position compensation for scrollable map containers
  - Markers now appear at correct GPS locations regardless of map center position, zoom level, or scroll position
- Enhanced debug logging with detailed marker calculation information
- Added bounds checking for marker positions with warnings

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

**v2 adds:**

- **Smooth Panning**: Continuous world-pixel view centre, drag with fling momentum - no 256 px jumps
- **Non-blocking Tiles**: LRU cache fed by a background task, so the UI thread never waits on the SD card
- **Offline Routes**: `route.bin` geometry with per-zoom level of detail, map matching and turn instructions
- **Follow Mode**: Eased follow-the-vehicle with a configurable marker offset

## Requirements

- ESP-IDF 5.0 or later
- LVGL 9.3
- File system support (FAT/SPIFFS/LittleFS)
- Map tiles in binary format (RGB565, 256x256 pixels)

**PSRAM: optional for v1, in practice required for v2.** A decoded tile is
128 KB, and the navigation layer keeps a cache of them - 25 slots for a 466 px
view, 48 for a 720 px one, which is 3 to 6 MB. The v1 grid can be shrunk to fit
internal RAM; the v2 cache cannot. The `targets` list in `idf_component.yml`
covers the component as a whole, so it includes chips that can run v1 happily
and have nowhere to put a v2 tile cache.

## Installation

### Using ESP-IDF Component Manager

You can easily add this component to your project using the idf.py command or by manually updating your idf_component.yml file.

#### Option 1: Using the idf.py add-dependency command (Recommended)
From your project's root directory, simply run the following command in your terminal:

```bash
idf.py add-dependency "0015/map_tiles^2.0.0"
```

This command will automatically add the component to your idf_component.yml file and download the required files the next time you build your project.

#### Option 2: Manual idf_component.yml update
Add to your project's `main/idf_component.yml`:

```yaml
dependencies:
  map_tiles:
    git: "https://github.com/0015/map_tiles.git"
    version: "^2.0.0"
```

### Manual Installation

1. Copy the `map_tiles` folder to your project's `components` directory
2. The component will be automatically included in your build

## Usage

> The rest of this page documents the **v1 API** (`map_tiles.h`), which is
> unchanged in v2.0.0. For routes and navigation, jump to
> [Navigation layer (v2)](#navigation-layer-v2).

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

## Navigation layer (v2)

*Everything above this line is the v1 API and is unchanged in v2.0.0.*

The v1 API loads a fixed grid of tiles anchored to an integer tile coordinate.
That is the right shape for a map you drag by hand. It is the wrong shape for
navigation, where the map has to keep up with a moving position: crossing a tile
boundary means reloading the whole grid, synchronously, on the UI thread.

v2.0.0 adds a second path built for that case. It is **purely additive** - the
headers, functions and behaviour described above are unchanged.

### Four new headers (v2)

| Header | What it gives you |
|---|---|
| `map_geo.h` | Web Mercator conversions, haversine distance, bearings, point-to-segment projection, distance and duration formatting. No LVGL, no ESP-IDF. |
| `map_route.h` | Load `route.bin`, walk it at a level of detail suited to the current zoom, snap a GPS fix onto it, and find the next turn. |
| `map_tile_cache.h` | An LRU cache of decoded tiles fed by a background FreeRTOS task. A miss returns `NULL` immediately and queues a read; it never blocks. |
| `map_view.h` | One LVGL object that renders tiles, the route and the position marker, centred on a floating-point world-pixel coordinate. Drag to pan with momentum, or let it follow the vehicle. |

### Minimal example

```c
#include "map_view.h"
#include "map_route.h"

map_route_handle_t route = map_route_load("/sdcard/route.bin", true);

lv_obj_t *map = map_view_create(lv_screen_active());
lv_obj_set_size(map, 720, 720);
map_view_set_tile_source(map, "/sdcard", "tiles1", 48, true);
map_view_set_zoom_range(map, 12, 19);
map_view_set_zoom(map, 17);
map_view_set_route(map, route);
map_view_set_follow(map, true);
map_view_set_follow_offset(map, 120);   // put the marker below centre

// then, on every GPS fix:
map_route_match_t m;
map_route_match(route, lat, lon, 35.0, &m);

map_view_set_position(map, lat, lon, heading, true);
map_view_set_progress(map, m.traveled_m);
map_view_set_off_route(map, !m.on_route);
map_view_set_return_target(map, m.return_lat, m.return_lon, !m.on_route);
```

`map_view` raises `LV_EVENT_VALUE_CHANGED` whenever the user drags the map and
follow mode releases, so the app can offer a re-centre button.

### What one match gives you

`map_route_match()` is the whole per-fix step. Everything a navigation screen
shows comes out of this one struct:

| Field | |
|---|---|
| `on_route`, `cross_track_m` | whether the fix is on the road, and how far off it is |
| `snapped_lat/lon`, `route_bearing_deg` | the fix projected onto the route |
| `traveled_m`, `remaining_m` | progress along the current lap |
| `next_maneuver`, `dist_to_maneuver_m` | index into the turn list, and how far to it |
| `arrived` | inside the arrival radius; never set on a loop |
| `lapped` | this fix crossed the start line of a loop |
| `return_lat/lon`, `return_bearing_deg` | the nearest point on the route, and the bearing to it |

**Progress freezes while you are off route**, deliberately. A route that doubles
back or loops passes within metres of itself, so letting an off-route fix pick
the globally nearest segment can snap you onto the leg you rode an hour ago and
make the distance remaining lurch by kilometres. `traveled_m` and `remaining_m`
are therefore reported from the last fix that was genuinely on the road.

### Off route

`map_view_set_return_target()` draws a dashed tether from the marker to the
nearest point on the route and, when that point is off screen, an arrow at the
edge pointing at it. Pass the match's `return_lat` / `return_lon` and
`!on_route`.

Which way that is, said the way a person can act on it, is
`map_geo_angle_diff_deg(heading, m.return_bearing_deg)`: a signed angle
relative to where the rider is facing, so the UI can say "to your left" rather
than "bearing 274".

### Drive routes and exercise circuits

The route file says which it is; there is nothing to switch on the device.

| | `MAP_ROUTE_KIND_DRIVE` | `MAP_ROUTE_KIND_EXERCISE` |
|---|---|---|
| Reaching the end | `arrived` is set | counts a lap and carries on |
| What the app shows | remaining, ETA | distance covered, elapsed |

A closed circuit - one whose last point is its first - reports
`map_route_is_loop()`. Matching wraps across that seam, so crossing the start
line continues onto the next lap instead of running off the end of the array,
and `match.lapped` fires once each time round.

Counting laps is harder than it looks: standing on the start line you are
equally close to the first segment and the last, so successive fixes flip
between "just started" and "nearly done". A lap is therefore only counted when
the circuit has actually been ridden - start, middle, end, start - which wobble
at the line can never fake.

### What the card actually holds

A corridor download covers a few zoom levels and nothing else. Asking the view
for any other zoom is not an error anywhere in the stack: the cache reports a
miss, the view draws a placeholder, and the result looks like a broken map
rather than a missing download.

`map_tile_cache_probe_zooms()` reads the folder and says what is really there,
so an app can clamp itself to it at start-up:

```c
int lo = 0, hi = 0;
if (map_tile_cache_probe_zooms("/sdcard", "tiles1", &lo, &hi) > 0) {
    map_view_set_zoom_range(map, lo, hi);
}
```

### Threading

`map_view` is an LVGL widget and every `map_view_*` call needs the LVGL lock,
like any other. `map_tile_cache_get()` must be called from the LVGL thread -
it is the only place slots are allocated or evicted, which is what lets the
loader task read tiles without a lock.

The loader task is the only thread of its own the component starts, and it
touches nothing but the slot it was handed. Hand fixes to the LVGL thread
rather than calling into the view from a GPS callback.

### Memory

| | |
|---|---|
| Tile cache | 128 KB per slot. A 466x466 view draws at most 3x3 tiles and warms the ring around them, so 28 slots (3.5 MB) covers it; a 720x720 view wants 48 (6 MB). |
| Route | 16 bytes per point plus 20 per turn. A 10,000-point route is about 160 KB. |
| Widgets | one, rather than one per tile. |

Both the cache and the route allocate from SPIRAM when you ask them to.

### route.bin

Produced on a PC by the navigation packager in
[OfflineMapDownloader](https://github.com/0015/OfflineMapDownloader)
(`app_nav.py`), or from a GPX with `route_format.py`. Fixed-size little-endian
records, so the device reads the arrays straight into PSRAM and uses them in
place - no XML or JSON parsing, and no per-point allocation.

Each point carries a `min_zoom` byte: the lowest zoom at which that vertex still
has to be drawn. Rendering at zoom Z is one filtered pass over the array, which
is what keeps a 10,000-point route cheap to draw when zoomed out. The exact
layout is documented at the top of `include/map_route.h`; `route_format.py` is
the writer for the same contract.

### New API reference (v2)

**Geometry (`map_geo.h`)**
- `map_geo_lon_to_world_px()` / `map_geo_lat_to_world_py()` and their inverses
- `map_geo_world_size_px()`, `map_geo_meters_per_pixel()`
- `map_geo_distance_m()`, `map_geo_bearing_deg()`
- `map_geo_normalize_deg()`, `map_geo_angle_diff_deg()` - the signed difference
  that turns a bearing into "to your left"
- `map_geo_point_segment_dist_m()` - projection onto a segment
- `map_geo_format_distance()`, `map_geo_format_duration()`

**Routes (`map_route.h`)**
- `map_route_load()` / `map_route_free()`
- `map_route_peek()` - read a route file's header into a `map_route_info_t`
  without loading its geometry, so a card holding many routes can be listed
  cheaply
- `map_route_get_name()`, `map_route_get_total_distance_m()`,
  `map_route_get_total_duration_s()`
- `map_route_is_loop()`, `map_route_get_kind()`, `map_route_get_tile_folder()`
- `map_route_get_point_count()`, `map_route_get_point()`
- `map_route_get_maneuver_count()`, `map_route_get_maneuver()`
- `map_route_get_start()`, `map_route_get_destination()`, `map_route_get_bounds()`
- `map_route_next_lod_point()` - walk the polyline at a given zoom
- `map_route_match()` - snap a fix onto the route, with a windowed search
- `map_route_reset_match()`, `map_route_set_search_window()`, `map_route_set_arrival_radius()`
- `map_maneuver_type_name()`, `map_maneuver_type_symbol()`

**Tile cache (`map_tile_cache.h`)**
- `map_tile_cache_create()` / `map_tile_cache_destroy()`
- `map_tile_cache_begin_frame()`, `map_tile_cache_get()`, `map_tile_cache_prefetch()`
- `map_tile_cache_get_version()` - poll to know when to repaint
- `map_tile_cache_set_folder()`, `map_tile_cache_clear()`, `map_tile_cache_get_stats()`
- `map_tile_cache_probe_zooms()` - which zoom levels the card actually holds

**Map view (`map_view.h`)**
- `map_view_create()`, `map_view_set_tile_source()`, `map_view_set_tile_folder()`,
  `map_view_get_tile_cache()`
- `map_view_set_zoom()`, `map_view_get_zoom()`, `map_view_set_zoom_range()`,
  `map_view_zoom_in/out()`
- `map_view_set_center()`, `map_view_get_center()`, `map_view_fit_bounds()`
- `map_view_set_route()`, `map_view_set_progress()`
- `map_view_set_position()`, `map_view_set_off_route()`,
  `map_view_set_return_target()` - the tether back to the road
- `map_view_set_follow()`, `map_view_get_follow()`, `map_view_set_follow_offset()`
- `map_view_set_style()`, `map_view_get_style()`, `map_view_set_debug_overlay()`
- `map_view_screen_to_latlon()`, `map_view_latlon_to_screen()`

### Working examples

The [`v2/`](https://github.com/0015/map_tiles_projects/tree/main/v2) folder of
the examples repository: full turn-by-turn navigation on a battery-powered
466x466 round AMOLED. It includes a route simulator, so you can exercise the
whole guidance path - turns, off-route alerts, laps - without a GPS fix or
leaving your desk.

## Performance Considerations

**v1**

- **Memory Usage**: Each tile uses ~128KB (256×256×2 bytes)
- **Grid Size**: Larger grids use more memory (3x3=9 tiles, 5x5=25 tiles, 7x7=49 tiles)
- **SPIRAM**: Recommended for ESP32-S3 with PSRAM for better performance
- **File System**: Ensure adequate file system performance for tile loading
- **Tile Caching**: Component maintains tile buffers until cleanup

**v2**

- **Cache size is not a free choice.** `map_view` only warms the ring around
  the visible tiles when the cache can hold the ring *and* the screen. Below
  that it silently stops prefetching, and panning starts showing placeholders.
- **The SD card is off the UI thread**, but it is still the limit: a cache miss
  costs a placeholder for as long as the read takes.
- **Route drawing is one filtered pass** over the point array, so a
  10,000-point route costs what its level of detail at the current zoom costs,
  not what the whole route costs.
- **Rendering is software.** At 466x466 the whole frame is about 217,000
  pixels; a chip with 2D acceleration or a faster PSRAM bus will hold a higher
  frame rate than one without.

## Example Projects

[**map_tiles_projects**](https://github.com/0015/map_tiles_projects) holds the
complete, buildable projects, split by which API they use:

| | Project | What it shows |
|---|---|---|
| **v1** | [`v1/01.Simple_Map`](https://github.com/0015/map_tiles_projects/tree/main/v1/01.Simple_Map) | Interactive map viewer: touch, zoom, manual coordinate entry. Three boards. |
| **v1** | [`v1/02.ESP32-S3_Map_LoRa_GPS`](https://github.com/0015/map_tiles_projects/tree/main/v1/02.ESP32-S3_Map_LoRa_GPS) | Live GPS tracking with positions shared over LoRa. |
| **v2** | [`v2/01.Navigation_ESP32-S3-Touch-AMOLED-1.75`](https://github.com/0015/map_tiles_projects/tree/main/v2/01.Navigation_ESP32-S3-Touch-AMOLED-1.75) | Turn-by-turn navigation on a battery-powered 466x466 round AMOLED, 1.75 inch. |

The `examples/` directory in this repository holds one short snippet per API:
[`basic_map_display.c`](https://github.com/0015/map_tiles/blob/main/examples/basic_map_display.c)
for v1, and
[`navigation_minimal.c`](https://github.com/0015/map_tiles/blob/main/examples/navigation_minimal.c)
for v2 — loading a route, following it, and turning each fix into a turn
instruction, in one file.

## License

This component is released under the MIT License. See LICENSE file for details.

## Contributing

Contributions are welcome! Please feel free to submit pull requests or open issues for bugs and feature requests.

## Support

For questions and support, please open an issue on the GitHub repository.
