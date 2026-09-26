/**
 * @file navigation_minimal.c
 * @brief The v2 navigation layer, end to end, in one file.
 *
 * Added in map_tiles v2.0.0.
 *
 * What this shows: load a route off the SD card, draw it on a map that follows
 * the vehicle, and turn each GPS fix into what a driver needs to see - distance
 * to the next turn, distance remaining, and which way the road is when they
 * have left it.
 *
 * What it leaves out: where the fixes come from. Feed nav_on_fix() from an NMEA
 * driver, and call it from the LVGL thread - every function here touches
 * widgets. A complete, buildable project, including the GPS driver and a route
 * simulator to exercise this without one, is at
 * https://github.com/0015/map_tiles_projects/tree/main/v2
 */

#include <stdio.h>

#include "lvgl.h"
#include "esp_log.h"

#include "map_geo.h"
#include "map_route.h"
#include "map_tile_cache.h"
#include "map_view.h"

static const char *TAG = "nav_example";

#define MOUNT_POINT     "/sdcard"
#define TILE_FOLDER     "tiles1"
#define ROUTE_PATH      MOUNT_POINT "/routes/route.bin"

/* 128 KB per slot. Size it from the view: map_view only warms the ring around
 * the visible tiles when the cache can hold the ring as well, which is 25 for
 * a 466 px view and 48 for a 720 px one. */
#define CACHE_SLOTS     28

/* Cross-track distance beyond which the fix counts as off route. */
#define OFF_ROUTE_M     35.0

static map_route_handle_t s_route;
static lv_obj_t          *s_map;

/* ------------------------------------------------------------------ set-up */

/**
 * @brief Build the map and put a route on it.
 *
 * @return false if the route or the tile folder could not be opened.
 */
bool nav_example_start(lv_obj_t *parent, int32_t size_px)
{
    /* Only the header is read here, so listing a card full of routes is cheap.
     * A route also names the tile folder it belongs to, which is how one card
     * carries routes in different regions. */
    map_route_info_t info;
    if (!map_route_peek(ROUTE_PATH, &info)) {
        ESP_LOGE(TAG, "%s is not a route file", ROUTE_PATH);
        return false;
    }
    ESP_LOGI(TAG, "%s: %.2f km, %s%s, tiles '%s', z%u-%u",
             info.name, info.distance_m / 1000.0,
             info.kind == MAP_ROUTE_KIND_EXERCISE ? "exercise" : "drive",
             info.loop ? " loop" : "",
             info.tile_folder[0] ? info.tile_folder : TILE_FOLDER,
             (unsigned)info.min_zoom, (unsigned)info.max_zoom);

    s_route = map_route_load(ROUTE_PATH, true);
    if (!s_route) return false;

    s_map = map_view_create(parent);
    if (!s_map) {
        map_route_free(s_route);
        s_route = NULL;
        return false;
    }
    lv_obj_set_size(s_map, size_px, size_px);
    lv_obj_center(s_map);

    const char *folder = map_route_get_tile_folder(s_route);
    if (!folder || folder[0] == '\0') folder = TILE_FOLDER;

    if (!map_view_set_tile_source(s_map, MOUNT_POINT, folder, CACHE_SLOTS, true)) {
        ESP_LOGE(TAG, "No tile cache at %s/%s", MOUNT_POINT, folder);
        return false;
    }

    /* Clamp the view to what the card actually holds. A corridor download
     * covers a few zoom levels and nothing else, and asking for another one is
     * not an error anywhere in the stack - it just draws placeholders, which
     * looks like a broken map rather than a missing download. */
    int card_min = 0, card_max = 0;
    if (map_tile_cache_probe_zooms(MOUNT_POINT, folder, &card_min, &card_max) > 0) {
        ESP_LOGI(TAG, "Card holds z%d to z%d", card_min, card_max);
        map_view_set_zoom_range(s_map, card_min, card_max);
        map_view_set_zoom(s_map, card_max);
    }

    map_view_set_route(s_map, s_route);
    map_view_set_follow(s_map, true);

    /* Open where the route starts, so the first frame is not the equator. */
    double lat, lon;
    if (map_route_get_start(s_route, &lat, &lon)) {
        map_view_set_center(s_map, lat, lon);
    }

    /* Arrival is judged against this radius; laps are counted instead on a
     * route that closes on itself. */
    map_route_set_arrival_radius(s_route, 25.0);
    return true;
}

/* --------------------------------------------------------------- per fix */

/**
 * @brief Fold one GPS fix into the map and report what to show.
 *
 * Call from the LVGL thread. @p heading_deg is the course over ground, which
 * is meaningless when stopped - hold the last good one rather than believing a
 * receiver that reports zero at a red light.
 */
void nav_example_on_fix(double lat, double lon, double heading_deg)
{
    if (!s_route || !s_map) return;

    map_route_match_t m;
    if (!map_route_match(s_route, lat, lon, OFF_ROUTE_M, &m)) return;

    /* The map: where you are, how far along, and - when you have left it - the
     * dashed tether back to the road and an arrow at the edge of the screen
     * pointing at it. */
    map_view_set_position(s_map, lat, lon, heading_deg, true);
    map_view_set_progress(s_map, m.traveled_m);
    map_view_set_off_route(s_map, !m.on_route);
    map_view_set_return_target(s_map, m.return_lat, m.return_lon, !m.on_route);

    if (!m.on_route) {
        /* Which way the road is, said the way a person can act on it: relative
         * to where they are pointing, not as a compass bearing. */
        double rel = map_geo_angle_diff_deg(heading_deg, m.return_bearing_deg);
        const char *side = (rel < -25.0) ? "to your left"
                         : (rel >  25.0) ? "to your right"
                         : "straight ahead";

        char gap[32];
        map_geo_format_distance(m.cross_track_m, false, gap, sizeof(gap));
        ESP_LOGW(TAG, "Off route: the road is %s %s", gap, side);

        /* Progress is frozen while off route, on purpose: a route that doubles
         * back passes within metres of itself, so snapping to the globally
         * nearest segment would jump you onto the leg you rode an hour ago. */
        return;
    }

    if (m.arrived) {
        ESP_LOGI(TAG, "Arrived");
        return;
    }
    if (m.lapped) {
        ESP_LOGI(TAG, "Lap complete");
    }

    char remaining[32];
    map_geo_format_distance(m.remaining_m, false, remaining, sizeof(remaining));

    if (m.next_maneuver >= 0) {
        map_maneuver_t turn;
        if (map_route_get_maneuver(s_route, m.next_maneuver, &turn)) {
            char to_turn[32];
            map_geo_format_distance(m.dist_to_maneuver_m, false, to_turn, sizeof(to_turn));
            ESP_LOGI(TAG, "%s in %s onto %s  (%s left)",
                     map_maneuver_type_name(turn.type), to_turn,
                     turn.street[0] ? turn.street : "the road ahead", remaining);
            /* map_maneuver_type_symbol(turn.type) is an LV_SYMBOL_* string,
             * ready for a label, with no extra font needed. */
        }
    } else {
        ESP_LOGI(TAG, "Continue  (%s left)", remaining);
    }
}

/* -------------------------------------------------------------- shutdown */

void nav_example_stop(void)
{
    /* Deleting the view destroys its tile cache and stops the loader task. The
     * route is ours, so it is freed after the view that was drawing it. */
    if (s_map) {
        lv_obj_delete(s_map);
        s_map = NULL;
    }
    if (s_route) {
        map_route_free(s_route);
        s_route = NULL;
    }
}
