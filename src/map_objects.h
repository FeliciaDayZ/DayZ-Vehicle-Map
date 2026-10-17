#ifndef DVM_MAP_OBJECTS_H
#define DVM_MAP_OBJECTS_H

#include <windows.h>
#include <gdiplus/gdiplus.h>
#include <stddef.h>

typedef struct MapObjectLayer MapObjectLayer;

BOOL dvm_map_objects_load(MapObjectLayer **result, const wchar_t *base,
                          const wchar_t *relative_path, double world_size,
                          wchar_t *error, size_t error_capacity);
void dvm_map_objects_destroy(MapObjectLayer *layer);
UINT dvm_map_objects_count(const MapObjectLayer *layer);
// With a notification target, new tiles render on a worker and completed tiles
// post message with this layer in lParam. NULL pauses queued work; caches stay.
BOOL dvm_map_objects_set_async(MapObjectLayer *layer, HWND window, UINT message);
typedef struct MapObjectCacheStats {
    UINT rendered_tiles, picture_loads, pending_tiles;
    SIZE_T tile_bytes;
} MapObjectCacheStats;
void dvm_map_objects_cache_stats(MapObjectLayer *layer, MapObjectCacheStats *stats);
void dvm_map_objects_draw(MapObjectLayer *layer, GpGraphics *graphics,
                          double pixels_per_metre, double pan_x, double pan_y,
                          int width, int height, BOOL draft);

#endif
