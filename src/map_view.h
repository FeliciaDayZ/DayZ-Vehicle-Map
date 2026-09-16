#ifndef DAYZ_VEHICLE_MAP_MAP_VIEW_H
#define DAYZ_VEHICLE_MAP_MAP_VIEW_H

#include "app_data.h"

#include <gdiplus/gdiplus.h>
#include <stddef.h>
#include <windows.h>

typedef struct MapRoadSegment {
    REAL x1;
    REAL y1;
    REAL x2;
    REAL y2;
    BYTE kind;
} MapRoadSegment;

typedef struct MapView {
    const MapInfo *map;
    GpImage *image;
    GpImage *overview_image;
    MapRoadSegment *road_segments;
    UINT road_segment_count;
    UINT asphalt_segment_count;
    UINT dirt_segment_count;
    UINT image_width;
    UINT image_height;
    UINT overview_width;
    UINT overview_height;
    double zoom;
    double fit_zoom;
    double min_zoom;
    double max_zoom;
    double pan_x;
    double pan_y;
    int client_width;
    int client_height;
    BOOL image_loaded;
} MapView;

BOOL dvm_map_view_load(MapView *view, const wchar_t *base_directory,
                       int map_index, wchar_t *error, size_t error_capacity);
void dvm_map_view_destroy(MapView *view);
void dvm_map_view_resize(MapView *view, int width, int height);
void dvm_map_view_reset(MapView *view);
void dvm_map_view_pan(MapView *view, double delta_x, double delta_y);
void dvm_map_view_zoom_at(MapView *view, double factor,
                          double screen_x, double screen_y);
BOOL dvm_map_world_to_screen(const MapView *view, double x, double z,
                             double *screen_x, double *screen_y);
BOOL dvm_map_screen_to_world(const MapView *view, double screen_x, double screen_y,
                             double *x, double *z);
void dvm_map_view_draw(const MapView *view, HDC dc, const RECT *client,
                       BOOL draft_quality);

#endif
