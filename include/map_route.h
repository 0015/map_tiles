/**
 * @file map_route.h
 * @brief Offline route ("route.bin") loading, level-of-detail and map matching.
 *
 * Added in map_tiles v2.0.0 (the navigation layer).
 *
 * A route is produced on a PC by `script/route_export.py` (or the "Navigation"
 * tab of OfflineMapDownloader v2) and copied to the SD card next to the tiles.
 * The device never parses GPX/XML: everything is a fixed-size little-endian
 * record so the file can be read straight into PSRAM and used in place.
 *
 * ## File layout (all little-endian)
 *
 * ```
 * Header, 64 bytes
 *   0   char[4]  magic "MRT1"
 *   4   uint16   version          (= MAP_ROUTE_FORMAT_VERSION)
 *   6   uint16   header_size      (= 64)
 *   8   uint32   point_count
 *   12  uint32   maneuver_count
 *   16  uint32   string_bytes
 *   20  uint32   total_distance_m
 *   24  uint32   total_duration_s
 *   28  int32    min_lat_e7
 *   32  int32    min_lon_e7
 *   36  int32    max_lat_e7
 *   40  int32    max_lon_e7
 *   44  uint32   name_off         (into the string pool, MAP_ROUTE_NO_STRING if unset)
 *   48  uint8    min_zoom
 *   49  uint8    max_zoom
 *   50  uint16   flags            (MAP_ROUTE_FLAG_*)
 *   52  uint8    kind             (map_route_kind_t)
 *   53  uint8    reserved
 *   54  uint16   reserved
 *   56  uint32   tile_folder_off  (into the string pool; empty = use the default)
 *   60  uint32   reserved
 *
 * Point, 16 bytes  x point_count
 *   0   int32    lat_e7
 *   4   int32    lon_e7
 *   8   uint32   cum_dist_m       (distance from the start of the route)
 *   12  uint8    min_zoom         (lowest zoom at which this point survives simplification)
 *   13  uint8    flags            (MAP_ROUTE_PT_*)
 *   14  uint16   bearing_cd       (bearing to the next point, centi-degrees; 0xFFFF on the last)
 *
 * Maneuver, 20 bytes x maneuver_count
 *   0   uint32   point_index
 *   4   uint32   dist_from_start_m
 *   8   uint32   name_off         (street name)
 *   12  uint32   instr_off        (full instruction text)
 *   16  uint8    type             (map_maneuver_type_t)
 *   17  uint8    modifier         (roundabout exit number, else 0)
 *   18  uint16   bearing_after_cd
 *
 * String pool, string_bytes bytes: NUL-terminated UTF-8, offset 0 is always "".
 * ```
 *
 * The per-point @c min_zoom is what makes level-of-detail free at runtime:
 * `route_export.py` runs Douglas-Peucker once per zoom with a tolerance of a few
 * screen pixels and records the lowest zoom at which each point is still needed.
 * Drawing at zoom Z is then a single filtered pass over the array - no second
 * copy of the geometry and no work on the device.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAP_ROUTE_MAGIC             "MRT1"
#define MAP_ROUTE_FORMAT_VERSION    1
#define MAP_ROUTE_HEADER_SIZE       64
#define MAP_ROUTE_POINT_SIZE        16
#define MAP_ROUTE_MANEUVER_SIZE     20
#define MAP_ROUTE_NO_STRING         0xFFFFFFFFu
#define MAP_ROUTE_NO_BEARING        0xFFFFu

/** @brief Per-point flag bits. */
#define MAP_ROUTE_PT_MANEUVER       (1u << 0)   /**< A maneuver starts at this point */

/** @brief Route-wide flag bits, at header offset 50. */
#define MAP_ROUTE_FLAG_LOOP         (1u << 0)   /**< Ends where it starts; lap it */

/**
 * @brief What the route is for.
 *
 * Drive and exercise want different things on screen and behave differently at
 * the end: a drive arrives, a lap rolls over. Both are read from the file, so
 * the device does not have to be told which kind of route it was handed.
 *
 * Files written before this field existed read as DRIVE, which is right.
 */
typedef enum {
    MAP_ROUTE_KIND_DRIVE = 0,
    MAP_ROUTE_KIND_EXERCISE = 1,
} map_route_kind_t;

/** @brief Turn instruction kinds, mapped from OSRM/GraphHopper by the exporter. */
typedef enum {
    MAP_MANEUVER_NONE = 0,
    MAP_MANEUVER_DEPART,
    MAP_MANEUVER_ARRIVE,
    MAP_MANEUVER_CONTINUE,
    MAP_MANEUVER_TURN_SLIGHT_LEFT,
    MAP_MANEUVER_TURN_LEFT,
    MAP_MANEUVER_TURN_SHARP_LEFT,
    MAP_MANEUVER_TURN_SLIGHT_RIGHT,
    MAP_MANEUVER_TURN_RIGHT,
    MAP_MANEUVER_TURN_SHARP_RIGHT,
    MAP_MANEUVER_UTURN,
    MAP_MANEUVER_ROUNDABOUT,
    MAP_MANEUVER_MERGE,
    MAP_MANEUVER_FORK_LEFT,
    MAP_MANEUVER_FORK_RIGHT,
    MAP_MANEUVER_EXIT_LEFT,
    MAP_MANEUVER_EXIT_RIGHT,
    MAP_MANEUVER_MAX
} map_maneuver_type_t;

/** @brief One decoded route vertex. */
typedef struct {
    double   lat;
    double   lon;
    uint32_t cum_dist_m;
    uint8_t  min_zoom;
    uint8_t  flags;
    double   bearing_deg;   /**< Bearing to the next point; 0 on the last point */
} map_route_point_t;

/** @brief One decoded turn instruction. */
typedef struct {
    uint32_t            point_index;
    uint32_t            dist_from_start_m;
    map_maneuver_type_t type;
    uint8_t             modifier;
    double              bearing_after_deg;
    const char         *street;         /**< Never NULL; "" when unknown */
    const char         *instruction;    /**< Never NULL; "" when unknown */
} map_maneuver_t;

/** @brief Result of snapping a GPS fix onto the route. */
typedef struct {
    bool     on_route;              /**< Cross-track error was within tolerance */
    bool     arrived;               /**< Within the arrival radius; never set on a loop */
    bool     lapped;                /**< This fix crossed the start line of a loop */
    int      segment_index;         /**< Index of the segment start point */
    double   t;                     /**< Position along that segment, 0..1 */
    double   snapped_lat;
    double   snapped_lon;
    double   cross_track_m;         /**< Distance from the fix to the route */
    double   route_bearing_deg;     /**< Heading of the route at the snapped point */

    /**
     * Where to rejoin, and which way that is.
     *
     * The nearest point on the route to the fix, and the bearing to it. While
     * on route this is the fix's own position; the moment you leave, it is the
     * road you should be aiming for.
     */
    double   return_lat;
    double   return_lon;
    double   return_bearing_deg;    /**< From the fix to that point, clockwise from north */
    /**
     * Progress along the route.
     *
     * Frozen at the last on-route fix while you are away from it. A route that
     * doubles back or loops passes close to itself, so letting an off-route fix
     * pick the globally nearest segment can jump you onto the other leg and
     * make the distance remaining lurch by kilometres.
     */
    uint32_t traveled_m;
    uint32_t remaining_m;
    int      next_maneuver;         /**< -1 when the route has no more turns */
    uint32_t dist_to_maneuver_m;
} map_route_match_t;

typedef struct map_route_t *map_route_handle_t;

/**
 * @brief What a route file says about itself, without loading its geometry.
 *
 * Reading this costs two short seeks, so a card holding a dozen routes can be
 * listed on screen without pulling any of them into memory.
 */
typedef struct {
    char             name[48];
    char             tile_folder[32];   /**< "" when the route does not care */
    uint32_t         distance_m;
    uint32_t         duration_s;
    uint32_t         point_count;
    uint32_t         maneuver_count;
    map_route_kind_t kind;
    bool             loop;
    uint8_t          min_zoom;
    uint8_t          max_zoom;
} map_route_info_t;

/**
 * @brief Read a route file's header without loading it.
 *
 * @return false if the file is missing or is not a "MRT1" file.
 */
bool map_route_peek(const char *path, map_route_info_t *out);

/**
 * @brief Load a route file into memory.
 *
 * @param path       Absolute path, e.g. "/sdcard/route.bin"
 * @param use_spiram Allocate the geometry in SPIRAM (recommended)
 * @return Handle, or NULL if the file is missing, truncated or not a "MRT1" file.
 */
map_route_handle_t map_route_load(const char *path, bool use_spiram);

/** @brief Release a route and everything it owns. */
void map_route_free(map_route_handle_t route);

/* ---------------------------------------------------------------- queries */

int         map_route_get_point_count(map_route_handle_t route);
bool        map_route_get_point(map_route_handle_t route, int index, map_route_point_t *out);
int         map_route_get_maneuver_count(map_route_handle_t route);
bool        map_route_get_maneuver(map_route_handle_t route, int index, map_maneuver_t *out);
uint32_t    map_route_get_total_distance_m(map_route_handle_t route);
uint32_t    map_route_get_total_duration_s(map_route_handle_t route);
const char *map_route_get_name(map_route_handle_t route);
void        map_route_get_bounds(map_route_handle_t route,
                                 double *min_lat, double *min_lon,
                                 double *max_lat, double *max_lon);
bool        map_route_get_start(map_route_handle_t route, double *lat, double *lon);
bool        map_route_get_destination(map_route_handle_t route, double *lat, double *lon);

/**
 * @brief True when the route closes on itself and should be lapped.
 *
 * Matching wraps across the seam, so crossing the start line continues rather
 * than running off the end of the array.
 */
bool             map_route_is_loop(map_route_handle_t route);
map_route_kind_t map_route_get_kind(map_route_handle_t route);

/**
 * @brief Tile folder this route wants, or "" to use the application default.
 *
 * Lets one card carry routes in different regions: the big data is the tiles,
 * so routes that share a region share a folder, and one that does not names
 * its own.
 */
const char *map_route_get_tile_folder(map_route_handle_t route);

/**
 * @brief Index of the next point at or after @p from_index that is kept at @p zoom.
 *
 * Renderers walk the polyline with this instead of every vertex, which is what
 * keeps a 10 000-point route cheap to draw when zoomed out.
 *
 * @return Point index, or -1 when the end of the route is reached.
 */
int map_route_next_lod_point(map_route_handle_t route, int from_index, int zoom);

/* ---------------------------------------------------------- map matching */

/**
 * @brief Snap a GPS fix onto the route and derive the navigation state.
 *
 * The search is windowed around the previously matched segment, so a call costs
 * O(window) rather than O(points). Call ::map_route_reset_match after a jump
 * (cold start, manual re-centre) to force a full scan on the next call.
 *
 * @param tolerance_m Cross-track distance beyond which @c on_route becomes false
 * @return true if a match was produced (even an off-route one), false on bad input.
 */
bool map_route_match(map_route_handle_t route, double lat, double lon,
                     double tolerance_m, map_route_match_t *out);

/** @brief Forget the windowed search state; the next match scans the whole route. */
void map_route_reset_match(map_route_handle_t route);

/** @brief Number of segments the windowed search looks at either side. Default 64. */
void map_route_set_search_window(map_route_handle_t route, int window);

/** @brief Arrival radius in metres used to set @c match.arrived. Default 25 m. */
void map_route_set_arrival_radius(map_route_handle_t route, double radius_m);

/* ------------------------------------------------------------- utilities */

/** @brief Short human label for a maneuver, e.g. "Turn right". */
const char *map_maneuver_type_name(map_maneuver_type_t type);

/**
 * @brief LVGL symbol string that best represents @p type.
 *
 * Returns one of the built-in LV_SYMBOL_* strings so no extra font is needed.
 */
const char *map_maneuver_type_symbol(map_maneuver_type_t type);

#ifdef __cplusplus
}
#endif
