#include "map_tile_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "map_tile_cache";

#define DEFAULT_STACK_SIZE   4096
#define DEFAULT_PRIORITY     3
#define DEFAULT_QUEUE_LEN    32
#define DEFAULT_CAPACITY     32
#define MAX_CAPACITY         256
#define TILE_HEADER_BYTES    12      /* LVGL v9 binary image header */
#define MAX_FOLDER_LEN       48
#define MAX_ABSENT_WARNINGS  4       /* loud about the first few misses only */
#define MAX_BASE_PATH_LEN    64

typedef enum {
    SLOT_EMPTY = 0,     /**< Unused, or evicted */
    SLOT_LOADING,       /**< Owned by the loader task */
    SLOT_READY,         /**< Holds pixels */
    SLOT_MISSING,       /**< The card has no such tile; remembered so we stop asking */
} slot_state_t;

typedef struct {
    slot_state_t   state;
    int            zoom;
    int            tile_x;
    int            tile_y;
    uint32_t       generation;  /**< Bumped on every re-key; stale loads detect it */
    uint32_t       last_used;   /**< Frame counter for LRU */
    uint8_t       *buf;
    lv_image_dsc_t img;
} tile_slot_t;

typedef struct {
    int      slot;
    int      zoom;
    int      tile_x;
    int      tile_y;
    uint32_t generation;
    char     folder[MAX_FOLDER_LEN];
} load_request_t;

struct map_tile_cache_t {
    char base_path[MAX_BASE_PATH_LEN];
    char folder[MAX_FOLDER_LEN];

    tile_slot_t *slots;
    int          capacity;
    bool         use_spiram;

    uint32_t frame;
    uint32_t version;
    uint32_t absent_seen;   /* tiles the card did not have, for log throttling */

    SemaphoreHandle_t lock;
    QueueHandle_t     queue;
    TaskHandle_t      task;
    volatile bool     running;

    map_tile_cache_stats_t stats;
};

/* --------------------------------------------------------------- helpers */

static inline void cache_lock(map_tile_cache_handle_t c)
{
    xSemaphoreTake(c->lock, portMAX_DELAY);
}

static inline void cache_unlock(map_tile_cache_handle_t c)
{
    xSemaphoreGive(c->lock);
}

static void slot_release(map_tile_cache_handle_t c, tile_slot_t *s)
{
    if (s->state == SLOT_READY)   c->stats.resident--;
    if (s->state == SLOT_MISSING) c->stats.missing--;
    if (s->state == SLOT_LOADING) c->stats.loading--;
    s->state = SLOT_EMPTY;
    s->generation++;
}

static void slot_fill_dsc(tile_slot_t *s)
{
    s->img.header.magic  = LV_IMAGE_HEADER_MAGIC;
    s->img.header.w      = MAP_TILE_CACHE_TILE_SIZE;
    s->img.header.h      = MAP_TILE_CACHE_TILE_SIZE;
    s->img.header.cf     = LV_COLOR_FORMAT_RGB565;
    s->img.header.stride = MAP_TILE_CACHE_TILE_SIZE * MAP_TILE_CACHE_BYTES_PER_PX;
    s->img.data          = (const uint8_t *)s->buf;
    s->img.data_size     = MAP_TILE_CACHE_TILE_BYTES;
    s->img.reserved      = NULL;
    s->img.reserved_2    = NULL;
}

/* ----------------------------------------------------------- loader task */

static void loader_task(void *arg)
{
    map_tile_cache_handle_t c = (map_tile_cache_handle_t)arg;
    load_request_t req;
    char path[256];

    while (c->running) {
        if (xQueueReceive(c->queue, &req, pdMS_TO_TICKS(200)) != pdTRUE) continue;
        if (!c->running) break;

        /* Confirm the slot still wants this tile, and take its buffer. The slot
         * stays in SLOT_LOADING for the whole read, and the UI thread never
         * evicts a loading slot, so writing to buf outside the lock is safe. */
        cache_lock(c);
        tile_slot_t *s = &c->slots[req.slot];
        bool still_wanted = (s->state == SLOT_LOADING && s->generation == req.generation);
        uint8_t *buf = still_wanted ? s->buf : NULL;
        cache_unlock(c);

        if (!buf) continue;

        snprintf(path, sizeof(path), "%s/%s/%d/%d/%d.bin",
                 c->base_path, req.folder, req.zoom, req.tile_x, req.tile_y);

        bool ok = false;
        FILE *f = fopen(path, "rb");
        if (f) {
            if (fseek(f, TILE_HEADER_BYTES, SEEK_SET) == 0) {
                size_t got = fread(buf, 1, MAP_TILE_CACHE_TILE_BYTES, f);
                if (got == MAP_TILE_CACHE_TILE_BYTES) {
                    ok = true;
                } else {
                    /* A short read means a truncated file. Blank the tail so a
                     * partial tile shows as flat colour instead of garbage. */
                    memset(buf + got, 0, MAP_TILE_CACHE_TILE_BYTES - got);
                    ok = true;
                    ESP_LOGW(TAG, "Short tile %s (%u of %d bytes)",
                             path, (unsigned)got, MAP_TILE_CACHE_TILE_BYTES);
                }
            }
            fclose(f);
        } else {
            /* A whole zoom level missing from the card looks exactly like a
             * blank map and used to say nothing at all. Name the first few
             * so the cause is in the log, then fall silent and let the
             * `missing` counter carry the rest. */
            uint32_t n = ++c->absent_seen;
            if (n <= MAX_ABSENT_WARNINGS) {
                ESP_LOGW(TAG, "Tile not on card: %s%s", path,
                         (n == MAX_ABSENT_WARNINGS) ? " (further misses logged at DEBUG)" : "");
            } else {
                ESP_LOGD(TAG, "Tile not on card: %s", path);
            }
        }

        cache_lock(c);
        s = &c->slots[req.slot];
        if (s->state == SLOT_LOADING && s->generation == req.generation) {
            c->stats.loading--;
            if (ok) {
                slot_fill_dsc(s);
                s->state = SLOT_READY;
                c->stats.resident++;
                c->stats.loads++;
            } else {
                s->state = SLOT_MISSING;
                c->stats.missing++;
            }
            c->version++;
        }
        cache_unlock(c);
    }

    c->task = NULL;
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------- lifecycle */

map_tile_cache_handle_t map_tile_cache_create(const map_tile_cache_config_t *config)
{
    if (!config || !config->base_path || !config->folder) {
        ESP_LOGE(TAG, "base_path and folder are required");
        return NULL;
    }

    int capacity = config->capacity > 0 ? config->capacity : DEFAULT_CAPACITY;
    if (capacity > MAX_CAPACITY) capacity = MAX_CAPACITY;

    map_tile_cache_handle_t c =
        (map_tile_cache_handle_t)calloc(1, sizeof(struct map_tile_cache_t));
    if (!c) {
        ESP_LOGE(TAG, "Out of memory for cache handle");
        return NULL;
    }

    strncpy(c->base_path, config->base_path, sizeof(c->base_path) - 1);
    c->base_path[sizeof(c->base_path) - 1] = '\0';
    strncpy(c->folder, config->folder, sizeof(c->folder) - 1);
    c->folder[sizeof(c->folder) - 1] = '\0';
    c->capacity   = capacity;
    c->use_spiram = config->use_spiram;
    c->stats.capacity = capacity;

    c->slots = (tile_slot_t *)calloc(capacity, sizeof(tile_slot_t));
    if (!c->slots) {
        ESP_LOGE(TAG, "Out of memory for %d slots", capacity);
        free(c);
        return NULL;
    }

    c->lock = xSemaphoreCreateMutex();
    if (!c->lock) {
        ESP_LOGE(TAG, "Cannot create cache mutex");
        goto fail;
    }

    int qlen = config->queue_length > 0 ? config->queue_length : DEFAULT_QUEUE_LEN;
    c->queue = xQueueCreate(qlen, sizeof(load_request_t));
    if (!c->queue) {
        ESP_LOGE(TAG, "Cannot create load queue");
        goto fail;
    }

    c->running = true;
    int stack = config->loader_stack_size > 0 ? config->loader_stack_size : DEFAULT_STACK_SIZE;
    int prio  = config->loader_priority   > 0 ? config->loader_priority   : DEFAULT_PRIORITY;
    if (xTaskCreate(loader_task, "map_tile_ld", stack, c, prio, &c->task) != pdPASS) {
        ESP_LOGE(TAG, "Cannot start loader task");
        c->running = false;
        goto fail;
    }

    ESP_LOGI(TAG, "Cache ready: %d slots (%d KB) from %s/%s",
             capacity, capacity * MAP_TILE_CACHE_TILE_BYTES / 1024,
             c->base_path, c->folder);
    return c;

fail:
    if (c->queue) vQueueDelete(c->queue);
    if (c->lock)  vSemaphoreDelete(c->lock);
    free(c->slots);
    free(c);
    return NULL;
}

void map_tile_cache_destroy(map_tile_cache_handle_t cache)
{
    if (!cache) return;

    cache->running = false;
    /* The loader blocks on the queue with a timeout, so it exits within one
     * poll period. Wait for it before freeing anything it may still touch. */
    for (int i = 0; i < 50 && cache->task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (cache->task) {
        ESP_LOGW(TAG, "Loader task did not exit; forcing deletion");
        vTaskDelete(cache->task);
        cache->task = NULL;
    }

    for (int i = 0; i < cache->capacity; i++) {
        free(cache->slots[i].buf);
    }
    free(cache->slots);

    if (cache->queue) vQueueDelete(cache->queue);
    if (cache->lock)  vSemaphoreDelete(cache->lock);
    free(cache);
}

/* ---------------------------------------------------------------- lookup */

static int find_slot(map_tile_cache_handle_t c, int zoom, int x, int y)
{
    for (int i = 0; i < c->capacity; i++) {
        tile_slot_t *s = &c->slots[i];
        if (s->state != SLOT_EMPTY && s->zoom == zoom && s->tile_x == x && s->tile_y == y) {
            return i;
        }
    }
    return -1;
}

/**
 * Pick a slot to reuse: a free one if there is one, otherwise the least
 * recently drawn tile that was not touched during the current frame. Slots in
 * SLOT_LOADING are never candidates - the loader is writing into them.
 */
static int acquire_slot(map_tile_cache_handle_t c)
{
    int best = -1;
    uint32_t best_used = UINT32_MAX;

    for (int i = 0; i < c->capacity; i++) {
        tile_slot_t *s = &c->slots[i];
        if (s->state == SLOT_EMPTY) return i;
        if (s->state == SLOT_LOADING) continue;
        if (s->last_used == c->frame) continue;  /* needed by the frame being drawn */

        if (s->last_used < best_used) {
            best_used = s->last_used;
            best = i;
        }
    }

    if (best >= 0) {
        slot_release(c, &c->slots[best]);
        c->stats.evictions++;
    }
    return best;
}

static bool queue_load(map_tile_cache_handle_t c, int slot, int zoom, int x, int y)
{
    tile_slot_t *s = &c->slots[slot];

    load_request_t req = {
        .slot       = slot,
        .zoom       = zoom,
        .tile_x     = x,
        .tile_y     = y,
        .generation = s->generation,
    };
    /* Both buffers are MAX_FOLDER_LEN, so copy the whole thing (terminator
     * included) rather than strncpy, which cannot promise termination here. */
    memcpy(req.folder, c->folder, sizeof(req.folder));
    req.folder[sizeof(req.folder) - 1] = '\0';

    /* Never block the UI thread: if the loader is behind, drop the request and
     * let the next frame ask again. */
    return xQueueSend(c->queue, &req, 0) == pdTRUE;
}

static lv_image_dsc_t *cache_lookup(map_tile_cache_handle_t c,
                                    int zoom, int x, int y,
                                    bool request_if_missing, bool count_stats)
{
    lv_image_dsc_t *result = NULL;

    cache_lock(c);

    int idx = find_slot(c, zoom, x, y);
    if (idx >= 0) {
        tile_slot_t *s = &c->slots[idx];
        s->last_used = c->frame;
        if (s->state == SLOT_READY) {
            if (count_stats) c->stats.hits++;
            result = &s->img;
        } else if (count_stats) {
            c->stats.misses++;
        }
        cache_unlock(c);
        return result;
    }

    if (count_stats) c->stats.misses++;

    if (!request_if_missing) {
        cache_unlock(c);
        return NULL;
    }

    idx = acquire_slot(c);
    if (idx < 0) {
        /* Every slot is in use by this frame - the view needs more tiles than
         * the cache holds. Nothing to do but render what we have. */
        cache_unlock(c);
        return NULL;
    }

    tile_slot_t *s = &c->slots[idx];
    if (!s->buf) {
        uint32_t caps = c->use_spiram ? (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : MALLOC_CAP_8BIT;
        s->buf = (uint8_t *)heap_caps_malloc(MAP_TILE_CACHE_TILE_BYTES, caps);
        if (!s->buf) {
            ESP_LOGE(TAG, "Out of memory for a tile buffer");
            cache_unlock(c);
            return NULL;
        }
    }

    s->zoom      = zoom;
    s->tile_x    = x;
    s->tile_y    = y;
    s->last_used = c->frame;
    s->state     = SLOT_LOADING;
    c->stats.loading++;

    bool queued = queue_load(c, idx, zoom, x, y);
    if (!queued) {
        slot_release(c, s);  /* also restores stats.loading */
    }

    cache_unlock(c);
    return NULL;
}

lv_image_dsc_t *map_tile_cache_get(map_tile_cache_handle_t cache,
                                   int zoom, int tile_x, int tile_y,
                                   bool request_if_missing)
{
    if (!cache) return NULL;
    return cache_lookup(cache, zoom, tile_x, tile_y, request_if_missing, true);
}

void map_tile_cache_prefetch(map_tile_cache_handle_t cache, int zoom, int tile_x, int tile_y)
{
    if (!cache) return;
    cache_lookup(cache, zoom, tile_x, tile_y, true, false);
}

void map_tile_cache_begin_frame(map_tile_cache_handle_t cache)
{
    if (!cache) return;
    cache_lock(cache);
    cache->frame++;
    cache_unlock(cache);
}

uint32_t map_tile_cache_get_version(map_tile_cache_handle_t cache)
{
    if (!cache) return 0;
    cache_lock(cache);
    uint32_t v = cache->version;
    cache_unlock(cache);
    return v;
}

void map_tile_cache_clear(map_tile_cache_handle_t cache)
{
    if (!cache) return;

    cache_lock(cache);
    for (int i = 0; i < cache->capacity; i++) {
        /* slot_release bumps the generation, which makes any in-flight read
         * for that slot discard its result instead of publishing it. */
        if (cache->slots[i].state != SLOT_EMPTY) {
            slot_release(cache, &cache->slots[i]);
        }
    }
    cache->version++;
    cache_unlock(cache);
}

void map_tile_cache_set_folder(map_tile_cache_handle_t cache, const char *folder)
{
    if (!cache || !folder) return;

    cache_lock(cache);
    bool changed = strncmp(cache->folder, folder, sizeof(cache->folder)) != 0;
    if (changed) {
        strncpy(cache->folder, folder, sizeof(cache->folder) - 1);
        cache->folder[sizeof(cache->folder) - 1] = '\0';
    }
    cache_unlock(cache);

    if (changed) map_tile_cache_clear(cache);
}

void map_tile_cache_get_stats(map_tile_cache_handle_t cache, map_tile_cache_stats_t *out)
{
    if (!cache || !out) return;
    cache_lock(cache);
    *out = cache->stats;
    cache_unlock(cache);
}

int map_tile_cache_probe_zooms(const char *base_path, const char *folder,
                               int *min_zoom, int *max_zoom)
{
    if (min_zoom) *min_zoom = -1;
    if (max_zoom) *max_zoom = -1;
    if (!base_path || !folder) return 0;

    char path[160];
    snprintf(path, sizeof(path), "%s/%s", base_path, folder);

    DIR *dir = opendir(path);
    if (!dir) {
        ESP_LOGE(TAG, "Tile folder not found: %s", path);
        return 0;
    }

    int found = 0, lo = 99, hi = -1;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        /* Zoom directories are plain decimal numbers; skip everything else so
         * stray files on the card do not widen the range. */
        char *end = NULL;
        long z = strtol(e->d_name, &end, 10);
        if (end == e->d_name || *end != '\0') continue;
        if (z < 0 || z > 22) continue;

        found++;
        if (z < lo) lo = (int)z;
        if (z > hi) hi = (int)z;
    }
    closedir(dir);

    if (found == 0) {
        ESP_LOGE(TAG, "No zoom level directories under %s", path);
        return 0;
    }

    if (min_zoom) *min_zoom = lo;
    if (max_zoom) *max_zoom = hi;
    ESP_LOGI(TAG, "%s holds %d zoom level(s): z%d to z%d", path, found, lo, hi);
    return found;
}
