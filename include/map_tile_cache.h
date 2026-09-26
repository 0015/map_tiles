/**
 * @file map_tile_cache.h
 * @brief Non-blocking LRU cache of decoded map tiles, backed by a loader task.
 *
 * Added in map_tiles v2.0.0 (the navigation layer).
 *
 * The original ::map_tiles_load_tile API reads a tile synchronously into a fixed
 * grid slot, so panning or re-centring meant re-reading the whole grid from the
 * SD card on the UI thread. That is fine for a viewer you drag by hand and
 * fatal for navigation, where the map has to keep up with a moving position.
 *
 * This cache inverts the relationship: the renderer asks for whatever tile it
 * needs right now and gets either the pixels (a cache hit, costing a lookup) or
 * NULL plus a queued background read. Nothing blocks the UI thread, and tiles
 * already on screen are never re-read.
 *
 * Thread safety: ::map_tile_cache_get must be called from the LVGL thread. It
 * is the only place slots are allocated or evicted, so the loader task can
 * never pull pixels out from under a draw in progress.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAP_TILE_CACHE_TILE_SIZE     256
#define MAP_TILE_CACHE_BYTES_PER_PX  2
#define MAP_TILE_CACHE_TILE_BYTES \
    (MAP_TILE_CACHE_TILE_SIZE * MAP_TILE_CACHE_TILE_SIZE * MAP_TILE_CACHE_BYTES_PER_PX)

typedef struct map_tile_cache_t *map_tile_cache_handle_t;

typedef struct {
    const char *base_path;          /**< e.g. "/sdcard" */
    const char *folder;             /**< Tile set folder, e.g. "tiles1" */
    int         capacity;           /**< Number of tile slots; each costs 128 KB */
    bool        use_spiram;         /**< Allocate tile buffers in SPIRAM */
    int         loader_stack_size;  /**< Loader task stack; 0 for the default (4096) */
    int         loader_priority;    /**< Loader task priority; 0 for the default (3) */
    int         queue_length;       /**< Pending load requests; 0 for the default (32) */
} map_tile_cache_config_t;

typedef struct {
    int      capacity;
    int      resident;      /**< Slots holding usable pixels */
    int      loading;       /**< Slots with a read in flight */
    int      missing;       /**< Slots remembering a tile that is not on the card */
    uint32_t hits;
    uint32_t misses;
    uint32_t loads;         /**< Completed background reads */
    uint32_t evictions;
} map_tile_cache_stats_t;

/** @brief Create a cache and start its loader task. */
map_tile_cache_handle_t map_tile_cache_create(const map_tile_cache_config_t *config);

/** @brief Stop the loader task and free every buffer. */
void map_tile_cache_destroy(map_tile_cache_handle_t cache);

/**
 * @brief Switch tile sets (street/satellite/...) and drop everything cached.
 */
void map_tile_cache_set_folder(map_tile_cache_handle_t cache, const char *folder);

/**
 * @brief Mark the start of a render pass.
 *
 * Tiles touched since the last call are protected from eviction, which stops a
 * view larger than the cache from evicting the tile it is about to draw.
 */
void map_tile_cache_begin_frame(map_tile_cache_handle_t cache);

/**
 * @brief Look up a tile, queueing a background read when it is not resident.
 *
 * @param request_if_missing Queue a read on a miss. Pass false for speculative
 *                           look-ups where you only want what is already there.
 * @return Image descriptor ready for ::lv_draw_image, or NULL if not resident.
 *         The pointer stays valid until the next ::map_tile_cache_begin_frame.
 */
lv_image_dsc_t *map_tile_cache_get(map_tile_cache_handle_t cache,
                                   int zoom, int tile_x, int tile_y,
                                   bool request_if_missing);

/**
 * @brief Queue a tile for loading without needing it yet.
 *
 * Used to pull in the ring of tiles just outside the viewport so panning finds
 * them already warm.
 */
void map_tile_cache_prefetch(map_tile_cache_handle_t cache, int zoom, int tile_x, int tile_y);

/**
 * @brief Counter bumped every time a background read completes.
 *
 * Poll this from an ::lv_timer and invalidate the map when it changes; that
 * keeps all LVGL calls on the LVGL thread without needing a lock.
 */
uint32_t map_tile_cache_get_version(map_tile_cache_handle_t cache);

/** @brief Drop every cached tile (after a zoom change, for instance). */
void map_tile_cache_clear(map_tile_cache_handle_t cache);

/** @brief Snapshot of cache counters, for the on-screen debug overlay. */
void map_tile_cache_get_stats(map_tile_cache_handle_t cache, map_tile_cache_stats_t *out);

/**
 * @brief Find which zoom levels a tile set on disk actually holds.
 *
 * A tile set downloaded as a corridor around a route covers a few zoom levels
 * and nothing else. Asking the map for a zoom outside that range is not an
 * error anywhere in the stack - it just draws placeholders - so the result
 * looks like a broken map rather than a missing download. Probe the card at
 * start-up and clamp the view to what is really there.
 *
 * @param base_path e.g. "/sdcard"
 * @param folder    e.g. "tiles1"
 * @param min_zoom  Lowest zoom directory found (may be NULL)
 * @param max_zoom  Highest zoom directory found (may be NULL)
 * @return Number of zoom levels found; 0 if the folder is missing or empty.
 */
int map_tile_cache_probe_zooms(const char *base_path, const char *folder,
                               int *min_zoom, int *max_zoom);

#ifdef __cplusplus
}
#endif
