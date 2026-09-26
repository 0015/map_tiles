/**
 * @file map_view.h
 * @brief Continuous-coordinate map widget: tiles, route, position marker.
 *
 * Added in map_tiles v2.0.0 (the navigation layer).
 *
 * A single LVGL object that renders the whole map itself instead of building a
 * widget per tile. The view centre is a floating-point world-pixel coordinate,
 * not an integer tile index, so panning and follow-the-vehicle motion are
 * smooth and there is no grid to re-anchor. Tiles come from
 * ::map_tile_cache_handle_t, so a miss costs a placeholder rectangle for a
 * frame or two rather than a blocking read.
 *
 * The widget is a plain ::lv_obj_t carrying its state in the user data, which
 * keeps it independent of LVGL's widget-class machinery and therefore portable
 * across 9.x point releases.
 *
 * ## Typical use
 * @code
 * lv_obj_t *map = map_view_create(lv_screen_active());
 * lv_obj_set_size(map, 720, 720);
 * map_view_set_tile_source(map, "/sdcard", "tiles1", 40, true);
 * map_view_set_zoom(map, 16);
 * map_view_set_route(map, route);
 * map_view_set_follow(map, true);
 * // then, per GPS fix:
 * map_view_set_position(map, lat, lon, heading, true);
 * @endcode
 *
 * All functions must be called with the LVGL lock held, like any other widget.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"
#include "map_route.h"
#include "map_tile_cache.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Colours and metrics for the overlay. Pass NULL fields to keep defaults. */
typedef struct {
    lv_color_t route_color;         /**< Road ahead */
    lv_color_t route_done_color;    /**< Part already driven */
    lv_color_t route_casing_color;  /**< Outline drawn under the route */
    lv_color_t marker_color;        /**< Position marker while on route */
    lv_color_t marker_off_color;    /**< Position marker while off route */
    lv_color_t return_color;        /**< The tether back to the route */
    lv_color_t destination_color;
    int32_t    route_width;         /**< Core line width in pixels */
    int32_t    casing_width;        /**< Outline width; 0 disables the casing pass */
    int32_t    marker_radius;
} map_view_style_t;

/**
 * @brief Create a map view.
 *
 * The object starts empty; give it a tile source before it can draw anything.
 */
lv_obj_t *map_view_create(lv_obj_t *parent);

/**
 * @brief Attach a tile cache built from @p base_path / @p folder.
 *
 * @param cache_capacity Tile slots, 128 KB each. A 720x720 view needs 16 on
 *                       screen, so 32-48 leaves room for panning ahead.
 * @return false if the cache could not be created.
 */
bool map_view_set_tile_source(lv_obj_t *obj, const char *base_path, const char *folder,
                              int cache_capacity, bool use_spiram);

/** @brief Switch tile sets (street, satellite, ...) at runtime. */
void map_view_set_tile_folder(lv_obj_t *obj, const char *folder);

/** @brief The underlying cache, for statistics or prefetching. */
map_tile_cache_handle_t map_view_get_tile_cache(lv_obj_t *obj);

/* ------------------------------------------------------------------ view */

void map_view_set_zoom(lv_obj_t *obj, int zoom);
int  map_view_get_zoom(lv_obj_t *obj);
void map_view_set_zoom_range(lv_obj_t *obj, int min_zoom, int max_zoom);

/** @brief Zoom one step, keeping the centre (or the follow target) fixed. */
void map_view_zoom_in(lv_obj_t *obj);
void map_view_zoom_out(lv_obj_t *obj);

void map_view_set_center(lv_obj_t *obj, double lat, double lon);
void map_view_get_center(lv_obj_t *obj, double *lat, double *lon);

/** @brief Pick the highest zoom at which the whole box fits, and centre on it. */
void map_view_fit_bounds(lv_obj_t *obj,
                         double min_lat, double min_lon,
                         double max_lat, double max_lon,
                         int padding_px);

/* --------------------------------------------------------------- content */

/**
 * @brief Set the route to draw. Pass NULL to clear.
 *
 * The widget does not take ownership; keep the handle alive while it is set.
 */
void map_view_set_route(lv_obj_t *obj, map_route_handle_t route);

/** @brief Distance already covered, used to grey out the road behind you. */
void map_view_set_progress(lv_obj_t *obj, uint32_t traveled_m);

/**
 * @brief Update the vehicle position.
 *
 * @param heading_deg Direction of travel, degrees clockwise from north
 * @param valid       false hides the marker (no fix yet)
 */
void map_view_set_position(lv_obj_t *obj, double lat, double lon,
                           double heading_deg, bool valid);

/** @brief Colour the marker as off-route. */
void map_view_set_off_route(lv_obj_t *obj, bool off_route);

/**
 * @brief Show the way back to the route.
 *
 * Draws a dashed tether from the vehicle to @p lat / @p lon - the nearest point
 * on the route - and, when that point is off screen, an arrow at the edge
 * pointing at it. Telling a rider they are off route without showing them which
 * way the road is leaves them to guess.
 *
 * @param active false hides both. Pass the match's return point and
 *               `!on_route`.
 */
void map_view_set_return_target(lv_obj_t *obj, double lat, double lon, bool active);

/* ---------------------------------------------------------- follow mode */

/**
 * @brief Keep the map centred on the vehicle.
 *
 * The centre eases toward the position rather than snapping, so a noisy fix
 * does not make the map jitter. Any drag by the user turns follow off and
 * sends ::LV_EVENT_VALUE_CHANGED so the app can offer a "re-centre" button.
 */
void map_view_set_follow(lv_obj_t *obj, bool follow);
bool map_view_get_follow(lv_obj_t *obj);

/**
 * @brief Push the marker below the centre so more of the road ahead is visible.
 *
 * @param offset_px Pixels below the widget centre. 0 centres the marker.
 */
void map_view_set_follow_offset(lv_obj_t *obj, int32_t offset_px);

/* ----------------------------------------------------------------- style */

void map_view_set_style(lv_obj_t *obj, const map_view_style_t *style);
void map_view_get_style(lv_obj_t *obj, map_view_style_t *out);

/** @brief Draw a small cache/segment-count overlay in the corner. */
void map_view_set_debug_overlay(lv_obj_t *obj, bool enabled);

/* ------------------------------------------------------------ geometry */

/** @brief Screen (absolute LVGL) coordinates to geographic coordinates. */
void map_view_screen_to_latlon(lv_obj_t *obj, int32_t x, int32_t y, double *lat, double *lon);

/**
 * @brief Geographic coordinates to screen coordinates.
 * @return false when the point is outside the widget.
 */
bool map_view_latlon_to_screen(lv_obj_t *obj, double lat, double lon, int32_t *x, int32_t *y);

#ifdef __cplusplus
}
#endif
