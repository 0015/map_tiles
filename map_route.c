#include "map_route.h"
#include "map_geo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "lvgl.h"

static const char *TAG = "map_route";

#define DEFAULT_SEARCH_WINDOW   64
#define DEFAULT_ARRIVAL_RADIUS  25.0

/** How far round the current lap the rider has genuinely been. */
enum {
    LAP_STAGE_START = 0,
    LAP_STAGE_MIDDLE,
    LAP_STAGE_END,
};

/*
 * When to abandon continuity and rescan the whole route.
 *
 * A fix that is merely off route is still next to the leg we were on, and
 * snapping it elsewhere is worse than admitting we are off. Only a fix that is
 * nowhere near the window - a cold start, a GPS jump, a rider who left and
 * rejoined somewhere else entirely - is worth a full rescan.
 */
#define RESCAN_TOLERANCE_MULTIPLE   5.0
#define RESCAN_MIN_DISTANCE_M       200.0

/* Lap counting zones, as fractions of the loop length. See map_route_match(). */
#define LAP_ZONE_START_MAX      0.25
#define LAP_ZONE_MID_LO         0.40
#define LAP_ZONE_MID_HI         0.60
#define LAP_ZONE_END_MIN        0.75

/* On-disk records. Every field is naturally aligned inside the struct and the
 * file is written little-endian, which matches every target this component
 * supports, so the arrays can be used in place after a single read. */
typedef struct __attribute__((packed)) {
    int32_t  lat_e7;
    int32_t  lon_e7;
    uint32_t cum_dist_m;
    uint8_t  min_zoom;
    uint8_t  flags;
    uint16_t bearing_cd;
} route_point_rec_t;

typedef struct __attribute__((packed)) {
    uint32_t point_index;
    uint32_t dist_from_start_m;
    uint32_t name_off;
    uint32_t instr_off;
    uint8_t  type;
    uint8_t  modifier;
    uint16_t bearing_after_cd;
} route_maneuver_rec_t;

struct map_route_t {
    route_point_rec_t    *points;
    route_maneuver_rec_t *maneuvers;
    char                 *strings;

    uint32_t point_count;
    uint32_t maneuver_count;
    uint32_t string_bytes;
    uint32_t total_distance_m;
    uint32_t total_duration_s;
    uint32_t name_off;
    uint32_t tile_folder_off;
    uint16_t flags;
    uint8_t  kind;

    double min_lat, min_lon, max_lat, max_lon;

    /* Windowed matcher state */
    int      last_segment;      /**< -1 = no previous match, scan everything */
    uint8_t  lap_stage;         /**< Furthest zone reached this lap; see match() */

    /* Where we were when last genuinely on the route. Progress is reported
     * from here while the rider is away from it. */
    bool     have_tracked;
    int      tracked_segment;
    double   tracked_t;
    int    search_window;
    double arrival_radius_m;
};

_Static_assert(sizeof(route_point_rec_t) == MAP_ROUTE_POINT_SIZE,
               "route point record must stay 16 bytes");
_Static_assert(sizeof(route_maneuver_rec_t) == MAP_ROUTE_MANEUVER_SIZE,
               "route maneuver record must stay 20 bytes");

static void *route_alloc(size_t size, bool use_spiram)
{
    if (size == 0) return NULL;
    void *p = NULL;
    if (use_spiram) {
        p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!p) {
        p = malloc(size);
    }
    return p;
}

static inline double e7_to_deg(int32_t e7)
{
    return (double)e7 * 1e-7;
}

static uint16_t rd_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int32_t rd_i32(const uint8_t *p) { return (int32_t)rd_u32(p); }

map_route_handle_t map_route_load(const char *path, bool use_spiram)
{
    if (!path) {
        ESP_LOGE(TAG, "No path given");
        return NULL;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open route file: %s", path);
        return NULL;
    }

    uint8_t hdr[MAP_ROUTE_HEADER_SIZE];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        ESP_LOGE(TAG, "Route file too short: %s", path);
        fclose(f);
        return NULL;
    }

    if (memcmp(hdr, MAP_ROUTE_MAGIC, 4) != 0) {
        ESP_LOGE(TAG, "Bad magic in %s (expected %s)", path, MAP_ROUTE_MAGIC);
        fclose(f);
        return NULL;
    }

    uint16_t version     = rd_u16(hdr + 4);
    uint16_t header_size = rd_u16(hdr + 6);
    if (version != MAP_ROUTE_FORMAT_VERSION) {
        ESP_LOGE(TAG, "Unsupported route version %u (this build reads %d)",
                 (unsigned)version, MAP_ROUTE_FORMAT_VERSION);
        fclose(f);
        return NULL;
    }
    if (header_size != MAP_ROUTE_HEADER_SIZE) {
        ESP_LOGE(TAG, "Unexpected header size %u", (unsigned)header_size);
        fclose(f);
        return NULL;
    }

    struct map_route_t *r = (struct map_route_t *)calloc(1, sizeof(struct map_route_t));
    if (!r) {
        ESP_LOGE(TAG, "Out of memory for route handle");
        fclose(f);
        return NULL;
    }

    r->point_count      = rd_u32(hdr + 8);
    r->maneuver_count   = rd_u32(hdr + 12);
    r->string_bytes     = rd_u32(hdr + 16);
    r->total_distance_m = rd_u32(hdr + 20);
    r->total_duration_s = rd_u32(hdr + 24);
    r->min_lat          = e7_to_deg(rd_i32(hdr + 28));
    r->min_lon          = e7_to_deg(rd_i32(hdr + 32));
    r->max_lat          = e7_to_deg(rd_i32(hdr + 36));
    r->max_lon          = e7_to_deg(rd_i32(hdr + 40));
    r->name_off         = rd_u32(hdr + 44);
    r->flags            = rd_u16(hdr + 50);
    r->kind             = hdr[52];
    r->tile_folder_off  = rd_u32(hdr + 56);

    r->last_segment      = -1;
    r->search_window     = DEFAULT_SEARCH_WINDOW;
    r->arrival_radius_m  = DEFAULT_ARRIVAL_RADIUS;

    if (r->point_count < 2) {
        ESP_LOGE(TAG, "Route needs at least 2 points, has %u", (unsigned)r->point_count);
        goto fail;
    }

    r->points = (route_point_rec_t *)route_alloc(
        (size_t)r->point_count * sizeof(route_point_rec_t), use_spiram);
    if (!r->points) {
        ESP_LOGE(TAG, "Out of memory for %u route points", (unsigned)r->point_count);
        goto fail;
    }
    if (fread(r->points, sizeof(route_point_rec_t), r->point_count, f) != r->point_count) {
        ESP_LOGE(TAG, "Truncated point array");
        goto fail;
    }

    if (r->maneuver_count) {
        r->maneuvers = (route_maneuver_rec_t *)route_alloc(
            (size_t)r->maneuver_count * sizeof(route_maneuver_rec_t), use_spiram);
        if (!r->maneuvers) {
            ESP_LOGE(TAG, "Out of memory for %u maneuvers", (unsigned)r->maneuver_count);
            goto fail;
        }
        if (fread(r->maneuvers, sizeof(route_maneuver_rec_t), r->maneuver_count, f)
            != r->maneuver_count) {
            ESP_LOGE(TAG, "Truncated maneuver array");
            goto fail;
        }
    }

    if (r->string_bytes) {
        r->strings = (char *)route_alloc(r->string_bytes + 1, use_spiram);
        if (!r->strings) {
            ESP_LOGE(TAG, "Out of memory for %u string bytes", (unsigned)r->string_bytes);
            goto fail;
        }
        if (fread(r->strings, 1, r->string_bytes, f) != r->string_bytes) {
            ESP_LOGE(TAG, "Truncated string pool");
            goto fail;
        }
        /* Guarantee termination even if the exporter wrote a ragged pool. */
        r->strings[r->string_bytes] = '\0';
    }

    fclose(f);

    ESP_LOGI(TAG, "Loaded %s: %u points, %u maneuvers, %.1f km, ~%u s, %s%s",
             path, (unsigned)r->point_count, (unsigned)r->maneuver_count,
             r->total_distance_m / 1000.0, (unsigned)r->total_duration_s,
             (r->kind == MAP_ROUTE_KIND_EXERCISE) ? "exercise" : "drive",
             (r->flags & MAP_ROUTE_FLAG_LOOP) ? ", loop" : "");

    return r;

fail:
    fclose(f);
    map_route_free(r);
    return NULL;
}

void map_route_free(map_route_handle_t route)
{
    if (!route) return;
    free(route->points);
    free(route->maneuvers);
    free(route->strings);
    free(route);
}

/* ---------------------------------------------------------------- queries */

int map_route_get_point_count(map_route_handle_t route)
{
    return route ? (int)route->point_count : 0;
}

static double bearing_from_cd(uint16_t cd)
{
    if (cd == MAP_ROUTE_NO_BEARING) return 0.0;
    return (double)cd / 100.0;
}

bool map_route_get_point(map_route_handle_t route, int index, map_route_point_t *out)
{
    if (!route || !out || index < 0 || (uint32_t)index >= route->point_count) return false;

    const route_point_rec_t *p = &route->points[index];
    out->lat         = e7_to_deg(p->lat_e7);
    out->lon         = e7_to_deg(p->lon_e7);
    out->cum_dist_m  = p->cum_dist_m;
    out->min_zoom    = p->min_zoom;
    out->flags       = p->flags;
    out->bearing_deg = bearing_from_cd(p->bearing_cd);
    return true;
}

int map_route_get_maneuver_count(map_route_handle_t route)
{
    return route ? (int)route->maneuver_count : 0;
}

static const char *route_string(map_route_handle_t route, uint32_t off)
{
    if (!route->strings || off == MAP_ROUTE_NO_STRING || off >= route->string_bytes) return "";
    return route->strings + off;
}

bool map_route_get_maneuver(map_route_handle_t route, int index, map_maneuver_t *out)
{
    if (!route || !out || index < 0 || (uint32_t)index >= route->maneuver_count) return false;

    const route_maneuver_rec_t *m = &route->maneuvers[index];
    out->point_index       = m->point_index;
    out->dist_from_start_m = m->dist_from_start_m;
    out->type              = (m->type < MAP_MANEUVER_MAX)
                             ? (map_maneuver_type_t)m->type : MAP_MANEUVER_NONE;
    out->modifier          = m->modifier;
    out->bearing_after_deg = bearing_from_cd(m->bearing_after_cd);
    out->street            = route_string(route, m->name_off);
    out->instruction       = route_string(route, m->instr_off);
    return true;
}

uint32_t map_route_get_total_distance_m(map_route_handle_t route)
{
    return route ? route->total_distance_m : 0;
}

uint32_t map_route_get_total_duration_s(map_route_handle_t route)
{
    return route ? route->total_duration_s : 0;
}

const char *map_route_get_name(map_route_handle_t route)
{
    return route ? route_string(route, route->name_off) : "";
}

void map_route_get_bounds(map_route_handle_t route,
                          double *min_lat, double *min_lon,
                          double *max_lat, double *max_lon)
{
    if (!route) return;
    if (min_lat) *min_lat = route->min_lat;
    if (min_lon) *min_lon = route->min_lon;
    if (max_lat) *max_lat = route->max_lat;
    if (max_lon) *max_lon = route->max_lon;
}

bool map_route_get_start(map_route_handle_t route, double *lat, double *lon)
{
    if (!route || route->point_count == 0) return false;
    if (lat) *lat = e7_to_deg(route->points[0].lat_e7);
    if (lon) *lon = e7_to_deg(route->points[0].lon_e7);
    return true;
}

bool map_route_is_loop(map_route_handle_t route)
{
    return route && (route->flags & MAP_ROUTE_FLAG_LOOP);
}

map_route_kind_t map_route_get_kind(map_route_handle_t route)
{
    if (!route) return MAP_ROUTE_KIND_DRIVE;
    return (route->kind == MAP_ROUTE_KIND_EXERCISE)
           ? MAP_ROUTE_KIND_EXERCISE : MAP_ROUTE_KIND_DRIVE;
}

const char *map_route_get_tile_folder(map_route_handle_t route)
{
    return route ? route_string(route, route->tile_folder_off) : "";
}

bool map_route_get_destination(map_route_handle_t route, double *lat, double *lon)
{
    if (!route || route->point_count == 0) return false;
    const route_point_rec_t *p = &route->points[route->point_count - 1];
    if (lat) *lat = e7_to_deg(p->lat_e7);
    if (lon) *lon = e7_to_deg(p->lon_e7);
    return true;
}

int map_route_next_lod_point(map_route_handle_t route, int from_index, int zoom)
{
    if (!route) return -1;
    for (int i = from_index; i < (int)route->point_count; i++) {
        if (route->points[i].min_zoom <= zoom) return i;
    }
    return -1;
}

/* ---------------------------------------------------------- map matching */

void map_route_reset_match(map_route_handle_t route)
{
    if (!route) return;
    route->last_segment = -1;
    route->lap_stage    = LAP_STAGE_START;
    route->have_tracked = false;
}

void map_route_set_search_window(map_route_handle_t route, int window)
{
    if (route && window > 0) route->search_window = window;
}

void map_route_set_arrival_radius(map_route_handle_t route, double radius_m)
{
    if (route && radius_m > 0.0) route->arrival_radius_m = radius_m;
}

/** Find the maneuver whose point index is at or after @p point_index. */
static int find_next_maneuver(map_route_handle_t route, uint32_t traveled_m)
{
    for (uint32_t i = 0; i < route->maneuver_count; i++) {
        /* DEPART sits at the very start and is never "upcoming". */
        if (route->maneuvers[i].type == MAP_MANEUVER_DEPART) continue;
        if (route->maneuvers[i].dist_from_start_m >= traveled_m) return (int)i;
    }
    return -1;
}

bool map_route_match(map_route_handle_t route, double lat, double lon,
                     double tolerance_m, map_route_match_t *out)
{
    if (!route || !out || route->point_count < 2) return false;

    int seg_count = (int)route->point_count - 1;

    bool loop = (route->flags & MAP_ROUTE_FLAG_LOOP) != 0;

    int lo = 0;
    int hi = seg_count - 1;
    if (route->last_segment >= 0) {
        lo = route->last_segment - route->search_window;
        hi = route->last_segment + route->search_window;
        if (loop) {
            /* Leave the window unclamped and wrap the index instead, so a rider
             * crossing the start line is matched on the segments just past it
             * rather than being dragged back down the array. */
            if (hi - lo + 1 > seg_count) {
                lo = 0;
                hi = seg_count - 1;
            }
        } else {
            if (lo < 0) lo = 0;
            if (hi > seg_count - 1) hi = seg_count - 1;
        }
    }

    double best_dist = 1e18;
    int    best_seg  = lo;
    double best_t    = 0.0;
    double best_lat  = lat;
    double best_lon  = lon;

    for (int pass = 0; pass < 2; pass++) {
        for (int i = lo; i <= hi; i++) {
            int idx = i;
            if (loop) {
                idx %= seg_count;
                if (idx < 0) idx += seg_count;
            }

            const route_point_rec_t *a = &route->points[idx];
            const route_point_rec_t *b = &route->points[idx + 1];

            double t, clat, clon;
            double d = map_geo_point_segment_dist_m(
                lat, lon,
                e7_to_deg(a->lat_e7), e7_to_deg(a->lon_e7),
                e7_to_deg(b->lat_e7), e7_to_deg(b->lon_e7),
                &t, &clat, &clon);

            if (d < best_dist) {
                best_dist = d;
                best_seg  = idx;
                best_t    = t;
                best_lat  = clat;
                best_lon  = clon;
            }
        }

        /* The window is a bet that we are still near where we were. Straying
         * off the road does not lose that bet - a route that doubles back
         * passes within metres of itself, so rescanning on every off-route fix
         * is how you end up matched to the leg you rode an hour ago. Only give
         * up when the fix is nowhere near anything in the window. */
        double rescan_at = tolerance_m * RESCAN_TOLERANCE_MULTIPLE;
        if (rescan_at < RESCAN_MIN_DISTANCE_M) rescan_at = RESCAN_MIN_DISTANCE_M;

        bool window_was_partial = (hi - lo + 1) < seg_count;
        if (pass == 0 && window_was_partial && best_dist > rescan_at) {
            lo = 0;
            hi = seg_count - 1;
            best_dist = 1e18;
            continue;
        }
        break;
    }

    bool on_route = (best_dist <= tolerance_m);

    /* The nearest point on the route is always worth reporting: on route it is
     * simply where you are, off route it is the road to aim for. */
    out->return_lat         = best_lat;
    out->return_lon         = best_lon;
    out->return_bearing_deg = map_geo_bearing_deg(lat, lon, best_lat, best_lon);

    /* Only a fix that is actually on the route may move the tracking window.
     * Otherwise a rider who strays near a leg they have already ridden - which
     * an out-and-back or a loop guarantees - drags the match onto it, and the
     * distance remaining, the next turn and the way home all jump with it. */
    int    prog_seg = best_seg;
    double prog_t   = best_t;

    if (on_route) {
        route->last_segment    = best_seg;
        route->tracked_segment = best_seg;
        route->tracked_t       = best_t;
        route->have_tracked    = true;
    } else if (route->have_tracked) {
        prog_seg = route->tracked_segment;
        prog_t   = route->tracked_t;
    }

    const route_point_rec_t *a = &route->points[prog_seg];
    const route_point_rec_t *b = &route->points[prog_seg + 1];

    uint32_t seg_span = (b->cum_dist_m > a->cum_dist_m) ? (b->cum_dist_m - a->cum_dist_m) : 0;
    uint32_t traveled = a->cum_dist_m + (uint32_t)(seg_span * prog_t + 0.5);
    if (traveled > route->total_distance_m) traveled = route->total_distance_m;

    out->on_route      = on_route;
    out->segment_index = best_seg;
    out->t             = best_t;
    out->snapped_lat   = best_lat;
    out->snapped_lon   = best_lon;
    out->cross_track_m = best_dist;
    out->traveled_m    = traveled;
    out->remaining_m   = route->total_distance_m - traveled;

    out->route_bearing_deg = (a->bearing_cd != MAP_ROUTE_NO_BEARING)
        ? bearing_from_cd(a->bearing_cd)
        : map_geo_bearing_deg(e7_to_deg(a->lat_e7), e7_to_deg(a->lon_e7),
                              e7_to_deg(b->lat_e7), e7_to_deg(b->lon_e7));

    int nm = find_next_maneuver(route, traveled);
    out->next_maneuver = nm;
    out->dist_to_maneuver_m = (nm >= 0)
        ? (route->maneuvers[nm].dist_from_start_m - traveled)
        : out->remaining_m;

    /*
     * Counting laps.
     *
     * The start line of a closed loop is also its finish line, so a rider
     * sitting on it is equally close to the first segment and the last, and
     * successive fixes flip between "just started" and "nearly done". Any rule
     * that watches for that transition counts a lap on every flip - and counts
     * several before the rider has even set off.
     *
     * So require the route to have actually been ridden: a lap is START, then
     * the MIDDLE of the loop, then the END, then START again. Wobble at the
     * line only ever flips between END and START, never through the middle, so
     * it cannot complete the sequence.
     */
    out->lapped = false;
    if (loop && on_route && route->total_distance_m > 0) {
        double progress = (double)traveled / route->total_distance_m;

        if (progress >= LAP_ZONE_END_MIN) {
            if (route->lap_stage == LAP_STAGE_MIDDLE) route->lap_stage = LAP_STAGE_END;
        } else if (progress >= LAP_ZONE_MID_LO && progress <= LAP_ZONE_MID_HI) {
            if (route->lap_stage == LAP_STAGE_START) route->lap_stage = LAP_STAGE_MIDDLE;
        } else if (progress <= LAP_ZONE_START_MAX) {
            if (route->lap_stage == LAP_STAGE_END) {
                route->lap_stage = LAP_STAGE_START;
                out->lapped = true;
            }
        }
    }

    /* You never arrive at a lap; you just come round again. */
    if (loop) {
        out->arrived = false;
    } else {
        double dest_lat = lat, dest_lon = lon;
        out->arrived = map_route_get_destination(route, &dest_lat, &dest_lon) &&
                       (map_geo_distance_m(lat, lon, dest_lat, dest_lon)
                        <= route->arrival_radius_m);
    }

    return true;
}

/* ------------------------------------------------------------- utilities */

const char *map_maneuver_type_name(map_maneuver_type_t type)
{
    switch (type) {
        case MAP_MANEUVER_DEPART:            return "Start";
        case MAP_MANEUVER_ARRIVE:            return "Arrive";
        case MAP_MANEUVER_CONTINUE:          return "Continue";
        case MAP_MANEUVER_TURN_SLIGHT_LEFT:  return "Slight left";
        case MAP_MANEUVER_TURN_LEFT:         return "Turn left";
        case MAP_MANEUVER_TURN_SHARP_LEFT:   return "Sharp left";
        case MAP_MANEUVER_TURN_SLIGHT_RIGHT: return "Slight right";
        case MAP_MANEUVER_TURN_RIGHT:        return "Turn right";
        case MAP_MANEUVER_TURN_SHARP_RIGHT:  return "Sharp right";
        case MAP_MANEUVER_UTURN:             return "Make a U-turn";
        case MAP_MANEUVER_ROUNDABOUT:        return "Roundabout";
        case MAP_MANEUVER_MERGE:             return "Merge";
        case MAP_MANEUVER_FORK_LEFT:         return "Keep left";
        case MAP_MANEUVER_FORK_RIGHT:        return "Keep right";
        case MAP_MANEUVER_EXIT_LEFT:         return "Exit left";
        case MAP_MANEUVER_EXIT_RIGHT:        return "Exit right";
        default:                             return "";
    }
}

const char *map_maneuver_type_symbol(map_maneuver_type_t type)
{
    /* Built-in Montserrat glyphs only, so a project gets turn icons without
     * shipping an extra font. Swap these for artwork when you have room. */
    switch (type) {
        case MAP_MANEUVER_DEPART:            return LV_SYMBOL_GPS;
        case MAP_MANEUVER_ARRIVE:            return LV_SYMBOL_OK;
        case MAP_MANEUVER_CONTINUE:
        case MAP_MANEUVER_MERGE:             return LV_SYMBOL_UP;
        case MAP_MANEUVER_TURN_SLIGHT_LEFT:
        case MAP_MANEUVER_TURN_LEFT:
        case MAP_MANEUVER_TURN_SHARP_LEFT:
        case MAP_MANEUVER_FORK_LEFT:
        case MAP_MANEUVER_EXIT_LEFT:         return LV_SYMBOL_LEFT;
        case MAP_MANEUVER_TURN_SLIGHT_RIGHT:
        case MAP_MANEUVER_TURN_RIGHT:
        case MAP_MANEUVER_TURN_SHARP_RIGHT:
        case MAP_MANEUVER_FORK_RIGHT:
        case MAP_MANEUVER_EXIT_RIGHT:        return LV_SYMBOL_RIGHT;
        case MAP_MANEUVER_UTURN:
        case MAP_MANEUVER_ROUNDABOUT:        return LV_SYMBOL_REFRESH;
        default:                             return LV_SYMBOL_UP;
    }
}

/* ----------------------------------------------------------------- peeking */

/** Copy one NUL-terminated string out of a file's pool, bounded. */
static void peek_string(FILE *f, long pool_start, uint32_t pool_bytes,
                        uint32_t off, char *dst, size_t dst_size)
{
    dst[0] = '\0';
    if (off == MAP_ROUTE_NO_STRING || off >= pool_bytes) return;
    if (fseek(f, pool_start + (long)off, SEEK_SET) != 0) return;

    size_t room = dst_size - 1;
    uint32_t avail = pool_bytes - off;
    if (room > avail) room = avail;

    size_t got = fread(dst, 1, room, f);
    dst[got] = '\0';
    dst[dst_size - 1] = '\0';   /* a pool with no terminator must not run on */
}

bool map_route_peek(const char *path, map_route_info_t *out)
{
    if (!path || !out) return false;
    memset(out, 0, sizeof(*out));

    FILE *f = fopen(path, "rb");
    if (!f) return false;

    uint8_t hdr[MAP_ROUTE_HEADER_SIZE];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) ||
        memcmp(hdr, MAP_ROUTE_MAGIC, 4) != 0 ||
        rd_u16(hdr + 4) != MAP_ROUTE_FORMAT_VERSION) {
        fclose(f);
        return false;
    }

    out->point_count    = rd_u32(hdr + 8);
    out->maneuver_count = rd_u32(hdr + 12);
    out->distance_m     = rd_u32(hdr + 20);
    out->duration_s     = rd_u32(hdr + 24);
    out->min_zoom       = hdr[48];
    out->max_zoom       = hdr[49];
    out->loop           = (rd_u16(hdr + 50) & MAP_ROUTE_FLAG_LOOP) != 0;
    out->kind           = (hdr[52] == MAP_ROUTE_KIND_EXERCISE)
                          ? MAP_ROUTE_KIND_EXERCISE : MAP_ROUTE_KIND_DRIVE;

    uint32_t string_bytes = rd_u32(hdr + 16);
    long pool_start = (long)MAP_ROUTE_HEADER_SIZE
                      + (long)out->point_count * MAP_ROUTE_POINT_SIZE
                      + (long)out->maneuver_count * MAP_ROUTE_MANEUVER_SIZE;

    peek_string(f, pool_start, string_bytes, rd_u32(hdr + 44),
                out->name, sizeof(out->name));
    peek_string(f, pool_start, string_bytes, rd_u32(hdr + 56),
                out->tile_folder, sizeof(out->tile_folder));

    fclose(f);
    return true;
}
