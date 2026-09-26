/**
 * @file map_geo.h
 * @brief Geodesic and Web-Mercator helpers shared by the navigation layer.
 *
 * Added in map_tiles v2.0.0 (the navigation layer). Pure C, no LVGL and no
 * ESP-IDF dependency, so the same code can be unit-tested on a host.
 *
 * "World pixels" are the standard slippy-map projection: at zoom @c z the whole
 * world is a square of `256 << z` pixels with (0,0) at the north-west corner.
 * Tile (x,y) therefore covers world pixels [x*256, (x+1)*256) horizontally.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAP_GEO_TILE_SIZE       256
#define MAP_GEO_EARTH_RADIUS_M  6371008.8   /**< IUGG mean earth radius */

/** @brief Width/height of the whole world in pixels at @p zoom. */
double map_geo_world_size_px(int zoom);

/** @brief Longitude -> world pixel X. */
double map_geo_lon_to_world_px(double lon, int zoom);

/** @brief Latitude -> world pixel Y (clamped to the Mercator limit). */
double map_geo_lat_to_world_py(double lat, int zoom);

/** @brief World pixel X -> longitude. */
double map_geo_world_px_to_lon(double px, int zoom);

/** @brief World pixel Y -> latitude. */
double map_geo_world_py_to_lat(double py, int zoom);

/** @brief Ground resolution in metres per pixel at @p lat / @p zoom. */
double map_geo_meters_per_pixel(double lat, int zoom);

/** @brief Great-circle distance in metres (haversine). */
double map_geo_distance_m(double lat1, double lon1, double lat2, double lon2);

/** @brief Initial bearing from point 1 to point 2, degrees clockwise from north. */
double map_geo_bearing_deg(double lat1, double lon1, double lat2, double lon2);

/** @brief Wrap an angle into [0, 360). */
double map_geo_normalize_deg(double deg);

/** @brief Signed smallest difference @p to - @p from, in (-180, 180]. */
double map_geo_angle_diff_deg(double from, double to);

/**
 * @brief Perpendicular distance from a point to the segment A-B.
 *
 * Uses a local equirectangular approximation, which is accurate well below a
 * metre for the segment lengths found in road geometry.
 *
 * @param out_t   Receives the clamped projection parameter in [0,1] (may be NULL)
 * @param out_lat Receives the latitude of the closest point (may be NULL)
 * @param out_lon Receives the longitude of the closest point (may be NULL)
 * @return Distance in metres from (plat,plon) to the closest point on A-B.
 */
double map_geo_point_segment_dist_m(double plat, double plon,
                                    double alat, double alon,
                                    double blat, double blon,
                                    double *out_t, double *out_lat, double *out_lon);

/**
 * @brief Format a distance for a navigation banner ("80 m", "1.4 km", "350 ft").
 *
 * Rounds to the step a driver expects: below 1000 m to the nearest 10 m, above
 * that to one decimal of a kilometre (or the imperial equivalent).
 */
void map_geo_format_distance(double meters, bool imperial, char *buf, size_t buf_size);

/** @brief Format a duration in seconds as "45 s", "12 min" or "1 h 05 min". */
void map_geo_format_duration(uint32_t seconds, char *buf, size_t buf_size);

#ifdef __cplusplus
}
#endif
