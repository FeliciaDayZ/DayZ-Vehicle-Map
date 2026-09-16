#include "map_view.h"

#include "util.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#define DVM_ROAD_HEADER_SIZE 16u
#define DVM_ROAD_RECORD_SIZE 20u
#define DVM_ROAD_FILE_LIMIT (32u * 1024u * 1024u)

static uint32_t read_u32_le(const BYTE *source)
{
    return (uint32_t)source[0] |
           (uint32_t)source[1] << 8 |
           (uint32_t)source[2] << 16 |
           (uint32_t)source[3] << 24;
}

static float read_f32_le(const BYTE *source)
{
    uint32_t bits = read_u32_le(source);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static BOOL load_road_paths(MapView *view, const wchar_t *base_directory,
                            wchar_t *error, size_t error_capacity)
{
    wchar_t path[DVM_PATH_CAP];
    HANDLE file = INVALID_HANDLE_VALUE;
    LARGE_INTEGER file_size;
    BYTE *data = NULL;
    DWORD bytes_read = 0;
    uint32_t segment_count;
    float file_world_size;
    uint32_t index;
    if (!view || !view->map || !view->map->road_relative_path) return FALSE;
    if (!dvm_join_path(path, DVM_PATH_CAP, base_directory,
                       view->map->road_relative_path)) {
        dvm_set_error(error, error_capacity, L"Road-data path is too long for %ls.",
                      view->map->display_name);
        return FALSE;
    }
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE || !GetFileSizeEx(file, &file_size) ||
        file_size.QuadPart < (LONGLONG)DVM_ROAD_HEADER_SIZE ||
        file_size.QuadPart > (LONGLONG)DVM_ROAD_FILE_LIMIT) {
        dvm_set_error(error, error_capacity,
                      L"Cannot load required %ls road data:\n%ls",
                      view->map->display_name, path);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        return FALSE;
    }
    data = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)file_size.QuadPart);
    if (!data || !ReadFile(file, data, (DWORD)file_size.QuadPart,
                           &bytes_read, NULL) ||
        bytes_read != (DWORD)file_size.QuadPart) {
        dvm_set_error(error, error_capacity,
                      L"Could not read %ls road data:\n%ls",
                      view->map->display_name, path);
        if (data) HeapFree(GetProcessHeap(), 0, data);
        CloseHandle(file);
        return FALSE;
    }
    CloseHandle(file);
    if (memcmp(data, "DVMROAD1", 8u) != 0) {
        dvm_set_error(error, error_capacity,
                      L"%ls road data has an invalid file signature.",
                      view->map->display_name);
        HeapFree(GetProcessHeap(), 0, data);
        return FALSE;
    }
    segment_count = read_u32_le(data + 8u);
    file_world_size = read_f32_le(data + 12u);
    if (segment_count == 0u ||
        (uint64_t)DVM_ROAD_HEADER_SIZE +
            (uint64_t)segment_count * DVM_ROAD_RECORD_SIZE !=
            (uint64_t)file_size.QuadPart ||
        !isfinite(file_world_size) ||
        fabs((double)file_world_size - view->map->world_size) > 1.0) {
        dvm_set_error(error, error_capacity,
                      L"%ls road data has an invalid header or length.",
                      view->map->display_name);
        HeapFree(GetProcessHeap(), 0, data);
        return FALSE;
    }
    view->road_segments = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
        (SIZE_T)segment_count * sizeof(*view->road_segments));
    if (!view->road_segments) {
        dvm_set_error(error, error_capacity,
                      L"Not enough memory for %ls road data.",
                      view->map->display_name);
        HeapFree(GetProcessHeap(), 0, data);
        return FALSE;
    }
    view->road_segment_count = segment_count;
    for (index = 0u; index < segment_count; ++index) {
        const BYTE *record = data + DVM_ROAD_HEADER_SIZE +
                             (SIZE_T)index * DVM_ROAD_RECORD_SIZE;
        float x1 = read_f32_le(record);
        float z1 = read_f32_le(record + 4u);
        float x2 = read_f32_le(record + 8u);
        float z2 = read_f32_le(record + 12u);
        BYTE kind = record[16u];
        MapRoadSegment *target = &view->road_segments[index];
        double world_size = view->map->world_size;
        if (!isfinite(x1) || !isfinite(z1) || !isfinite(x2) || !isfinite(z2) ||
            x1 < -2.0f || z1 < -2.0f || x2 < -2.0f || z2 < -2.0f ||
            x1 > file_world_size + 2.0f || z1 > file_world_size + 2.0f ||
            x2 > file_world_size + 2.0f || z2 > file_world_size + 2.0f ||
            kind > 1u) {
            dvm_set_error(error, error_capacity,
                          L"%ls road data contains an invalid segment at record %u.",
                          view->map->display_name, index + 1u);
            HeapFree(GetProcessHeap(), 0, data);
            return FALSE;
        }
        target->x1 = (REAL)((double)x1 / world_size * (double)view->image_width);
        target->y1 = (REAL)((1.0 - (double)z1 / world_size) *
                            (double)view->image_height);
        target->x2 = (REAL)((double)x2 / world_size * (double)view->image_width);
        target->y2 = (REAL)((1.0 - (double)z2 / world_size) *
                            (double)view->image_height);
        target->kind = kind;
        if (kind == 0u) ++view->asphalt_segment_count;
        else ++view->dirt_segment_count;
    }
    HeapFree(GetProcessHeap(), 0, data);
    return TRUE;
}

static double clamp_double(double value, double minimum, double maximum)
{
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

static void clamp_pan(MapView *view)
{
    double draw_width;
    double draw_height;
    if (!view || view->client_width <= 0 || view->client_height <= 0) return;
    draw_width = (double)view->image_width * view->zoom;
    draw_height = (double)view->image_height * view->zoom;
    if (draw_width <= (double)view->client_width) {
        view->pan_x = ((double)view->client_width - draw_width) * 0.5;
    } else {
        view->pan_x = clamp_double(view->pan_x,
                                   (double)view->client_width - draw_width, 0.0);
    }
    if (draw_height <= (double)view->client_height) {
        view->pan_y = ((double)view->client_height - draw_height) * 0.5;
    } else {
        view->pan_y = clamp_double(view->pan_y,
                                   (double)view->client_height - draw_height, 0.0);
    }
}

static void create_display_bitmaps(MapView *view)
{
    GpBitmap *display_bitmap = NULL;
    GpBitmap *overview_bitmap = NULL;
    GpGraphics *graphics = NULL;
    GpStatus status;
    double overview_scale;
    UINT overview_width;
    UINT overview_height;
    if (!view || !view->image) return;

    status = GdipCreateBitmapFromScan0((INT)view->image_width,
                                       (INT)view->image_height, 0,
                                       PixelFormat32bppPARGB, NULL,
                                       &display_bitmap);
    if (status == Ok && display_bitmap &&
        GdipGetImageGraphicsContext(display_bitmap, &graphics) == Ok) {
        GdipSetCompositingMode(graphics, CompositingModeSourceCopy);
        GdipSetCompositingQuality(graphics, CompositingQualityHighSpeed);
        GdipSetInterpolationMode(graphics, InterpolationModeBilinear);
        GdipSetPixelOffsetMode(graphics, PixelOffsetModeHalf);
        status = GdipDrawImageRectRectI(graphics, view->image,
            0, 0, (INT)view->image_width, (INT)view->image_height,
            0, 0, (INT)view->image_width, (INT)view->image_height,
            UnitPixel, NULL, NULL, NULL);
        GdipDeleteGraphics(graphics);
        graphics = NULL;
        if (status == Ok) {
            GdipDisposeImage(view->image);
            view->image = (GpImage *)display_bitmap;
            display_bitmap = NULL;
        }
    }
    if (graphics) GdipDeleteGraphics(graphics);
    if (display_bitmap) GdipDisposeImage((GpImage *)display_bitmap);

    overview_scale = fmin(1024.0 / (double)view->image_width,
                          1024.0 / (double)view->image_height);
    if (!(overview_scale < 1.0)) return;
    overview_width = (UINT)fmax(1.0, floor((double)view->image_width * overview_scale + 0.5));
    overview_height = (UINT)fmax(1.0, floor((double)view->image_height * overview_scale + 0.5));
    status = GdipCreateBitmapFromScan0((INT)overview_width, (INT)overview_height, 0,
                                       PixelFormat32bppPARGB, NULL, &overview_bitmap);
    if (status == Ok && overview_bitmap &&
        GdipGetImageGraphicsContext(overview_bitmap, &graphics) == Ok) {
        GdipSetCompositingMode(graphics, CompositingModeSourceCopy);
        GdipSetCompositingQuality(graphics, CompositingQualityHighSpeed);
        GdipSetInterpolationMode(graphics, InterpolationModeHighQualityBilinear);
        GdipSetPixelOffsetMode(graphics, PixelOffsetModeHalf);
        status = GdipDrawImageRectRectI(graphics, view->image,
            0, 0, (INT)overview_width, (INT)overview_height,
            0, 0, (INT)view->image_width, (INT)view->image_height,
            UnitPixel, NULL, NULL, NULL);
        GdipDeleteGraphics(graphics);
        graphics = NULL;
        if (status == Ok) {
            view->overview_image = (GpImage *)overview_bitmap;
            view->overview_width = overview_width;
            view->overview_height = overview_height;
            overview_bitmap = NULL;
        }
    }
    if (graphics) GdipDeleteGraphics(graphics);
    if (overview_bitmap) GdipDisposeImage((GpImage *)overview_bitmap);
}

BOOL dvm_map_view_load(MapView *view, const wchar_t *base_directory,
                       int map_index, wchar_t *error, size_t error_capacity)
{
    const MapInfo *map = dvm_map_info(map_index);
    wchar_t path[DVM_PATH_CAP];
    GpStatus status;
    if (!view || !base_directory || !map) return FALSE;
    ZeroMemory(view, sizeof(*view));
    view->map = map;
    if (!dvm_join_path(path, DVM_PATH_CAP, base_directory, map->image_relative_path)) {
        dvm_set_error(error, error_capacity, L"Map image path is too long for %ls.",
                      map->display_name);
        return FALSE;
    }
    status = GdipLoadImageFromFile(path, &view->image);
    if (status != Ok || !view->image) {
        dvm_set_error(error, error_capacity, L"Cannot load required %ls map image:\n%ls",
                      map->display_name, path);
        return FALSE;
    }
    if (GdipGetImageWidth(view->image, &view->image_width) != Ok ||
        GdipGetImageHeight(view->image, &view->image_height) != Ok ||
        view->image_width != map->expected_image_width ||
        view->image_height != map->expected_image_height) {
        dvm_set_error(error, error_capacity,
                      L"%ls map image dimensions are %u x %u; expected %u x %u. "
                      L"The coordinate calibration cannot be used.",
                      map->display_name, view->image_width, view->image_height,
                      map->expected_image_width, map->expected_image_height);
        dvm_map_view_destroy(view);
        return FALSE;
    }
    if (!load_road_paths(view, base_directory, error, error_capacity)) {
        dvm_map_view_destroy(view);
        return FALSE;
    }
    create_display_bitmaps(view);
    view->image_loaded = TRUE;
    if (error && error_capacity) error[0] = L'\0';
    return TRUE;
}

void dvm_map_view_destroy(MapView *view)
{
    if (!view) return;
    if (view->road_segments)
        HeapFree(GetProcessHeap(), 0, view->road_segments);
    if (view->overview_image) GdipDisposeImage(view->overview_image);
    if (view->image) GdipDisposeImage(view->image);
    ZeroMemory(view, sizeof(*view));
}

void dvm_map_view_resize(MapView *view, int width, int height)
{
    BOOL had_size;
    double center_image_x = 0.0;
    double center_image_y = 0.0;
    double fit;
    if (!view || width <= 0 || height <= 0 || view->image_width == 0 ||
        view->image_height == 0) return;
    had_size = view->client_width > 0 && view->client_height > 0 && view->zoom > 0.0;
    if (had_size) {
        center_image_x = ((double)view->client_width * 0.5 - view->pan_x) / view->zoom;
        center_image_y = ((double)view->client_height * 0.5 - view->pan_y) / view->zoom;
    }
    view->client_width = width;
    view->client_height = height;
    fit = fmin((double)width / (double)view->image_width,
               (double)height / (double)view->image_height);
    if (!(fit > 0.0)) fit = 1.0;
    view->fit_zoom = fit;
    view->min_zoom = fit;
    view->max_zoom = fit * 32.0;
    if (!had_size) {
        dvm_map_view_reset(view);
        return;
    }
    view->zoom = clamp_double(view->zoom, view->min_zoom, view->max_zoom);
    view->pan_x = (double)width * 0.5 - center_image_x * view->zoom;
    view->pan_y = (double)height * 0.5 - center_image_y * view->zoom;
    clamp_pan(view);
}

void dvm_map_view_reset(MapView *view)
{
    if (!view || view->client_width <= 0 || view->client_height <= 0) return;
    view->zoom = view->fit_zoom > 0.0 ? view->fit_zoom : 1.0;
    view->pan_x = ((double)view->client_width -
                   (double)view->image_width * view->zoom) * 0.5;
    view->pan_y = ((double)view->client_height -
                   (double)view->image_height * view->zoom) * 0.5;
    clamp_pan(view);
}

void dvm_map_view_pan(MapView *view, double delta_x, double delta_y)
{
    if (!view) return;
    view->pan_x += delta_x;
    view->pan_y += delta_y;
    clamp_pan(view);
}

void dvm_map_view_zoom_at(MapView *view, double factor,
                          double screen_x, double screen_y)
{
    double image_x;
    double image_y;
    double new_zoom;
    if (!view || !(view->zoom > 0.0) || !(factor > 0.0)) return;
    image_x = (screen_x - view->pan_x) / view->zoom;
    image_y = (screen_y - view->pan_y) / view->zoom;
    new_zoom = clamp_double(view->zoom * factor, view->min_zoom, view->max_zoom);
    view->zoom = new_zoom;
    view->pan_x = screen_x - image_x * new_zoom;
    view->pan_y = screen_y - image_y * new_zoom;
    clamp_pan(view);
}

BOOL dvm_map_world_to_screen(const MapView *view, double x, double z,
                             double *screen_x, double *screen_y)
{
    double image_x;
    double image_y;
    if (!view || !view->map || !screen_x || !screen_y ||
        !isfinite(x) || !isfinite(z) || x < 0.0 || z < 0.0 ||
        x > view->map->world_size || z > view->map->world_size ||
        !(view->zoom > 0.0)) return FALSE;
    image_x = x / view->map->world_size * (double)view->image_width;
    image_y = (1.0 - z / view->map->world_size) * (double)view->image_height;
    *screen_x = view->pan_x + image_x * view->zoom;
    *screen_y = view->pan_y + image_y * view->zoom;
    return TRUE;
}

BOOL dvm_map_screen_to_world(const MapView *view, double screen_x, double screen_y,
                             double *x, double *z)
{
    double image_x;
    double image_y;
    if (!view || !view->map || !x || !z || !(view->zoom > 0.0)) return FALSE;
    image_x = (screen_x - view->pan_x) / view->zoom;
    image_y = (screen_y - view->pan_y) / view->zoom;
    if (image_x < 0.0 || image_y < 0.0 ||
        image_x > (double)view->image_width || image_y > (double)view->image_height)
        return FALSE;
    *x = image_x / (double)view->image_width * view->map->world_size;
    *z = (1.0 - image_y / (double)view->image_height) * view->map->world_size;
    return TRUE;
}

static void configure_road_pen(GpPen *pen)
{
    if (!pen) return;
    GdipSetPenStartCap(pen, LineCapRound);
    GdipSetPenEndCap(pen, LineCapRound);
    GdipSetPenLineJoin(pen, LineJoinRound);
}

static void draw_road_paths(const MapView *view, GpGraphics *graphics,
                            BOOL draft_quality)
{
    GpPath *asphalt_path = NULL;
    GpPath *dirt_path = NULL;
    GpMatrix *matrix = NULL;
    GpPen *asphalt_outline = NULL;
    GpPen *asphalt_fill = NULL;
    GpPen *dirt_outline = NULL;
    GpPen *dirt_fill = NULL;
    double visible_left;
    double visible_top;
    double visible_right;
    double visible_bottom;
    UINT visible_asphalt = 0u;
    UINT visible_dirt = 0u;
    UINT index;
    if (!view || !graphics || !view->road_segments ||
        view->road_segment_count == 0u || view->zoom < 1.25) return;
    visible_left = -view->pan_x / view->zoom - 5.0;
    visible_top = -view->pan_y / view->zoom - 5.0;
    visible_right = ((double)view->client_width - view->pan_x) / view->zoom + 5.0;
    visible_bottom = ((double)view->client_height - view->pan_y) / view->zoom + 5.0;
    if (GdipCreatePath(FillModeAlternate, &asphalt_path) != Ok ||
        GdipCreatePath(FillModeAlternate, &dirt_path) != Ok ||
        !asphalt_path || !dirt_path) goto cleanup;
    for (index = 0u; index < view->road_segment_count; ++index) {
        const MapRoadSegment *segment = &view->road_segments[index];
        GpPath *target;
        if ((segment->x1 < visible_left && segment->x2 < visible_left) ||
            (segment->x1 > visible_right && segment->x2 > visible_right) ||
            (segment->y1 < visible_top && segment->y2 < visible_top) ||
            (segment->y1 > visible_bottom && segment->y2 > visible_bottom)) continue;
        target = segment->kind == 0u ? asphalt_path : dirt_path;
        if (GdipStartPathFigure(target) != Ok ||
            GdipAddPathLine(target, segment->x1, segment->y1,
                            segment->x2, segment->y2) != Ok) goto cleanup;
        if (segment->kind == 0u) ++visible_asphalt;
        else ++visible_dirt;
    }
    if (visible_asphalt == 0u && visible_dirt == 0u) goto cleanup;
    if (GdipCreateMatrix2((REAL)view->zoom, 0.0f, 0.0f, (REAL)view->zoom,
                          (REAL)view->pan_x, (REAL)view->pan_y, &matrix) != Ok ||
        !matrix) goto cleanup;
    GdipSetWorldTransform(graphics, matrix);
    GdipSetCompositingMode(graphics, CompositingModeSourceOver);
    GdipSetCompositingQuality(graphics, draft_quality
        ? CompositingQualityHighSpeed : CompositingQualityHighQuality);
    GdipSetSmoothingMode(graphics, draft_quality
        ? SmoothingModeHighSpeed : SmoothingModeAntiAlias);
    GdipSetPixelOffsetMode(graphics, PixelOffsetModeHalf);

    GdipCreatePen1(0xff292724u, 3.5f / 3.0f, UnitWorld, &asphalt_outline);
    GdipCreatePen1(0xffffda78u, draft_quality ? 3.2f / 3.0f : 1.7f / 3.0f,
                   UnitWorld, &asphalt_fill);
    GdipCreatePen1(0xff302d2cu, 3.5f / 3.0f, UnitWorld, &dirt_outline);
    GdipCreatePen1(0xffded5d1u, draft_quality ? 3.2f / 3.0f : 1.7f / 3.0f,
                   UnitWorld, &dirt_fill);
    configure_road_pen(asphalt_outline);
    configure_road_pen(asphalt_fill);
    configure_road_pen(dirt_outline);
    configure_road_pen(dirt_fill);

    if (!draft_quality && dirt_outline && visible_dirt)
        GdipDrawPath(graphics, dirt_outline, dirt_path);
    if (!draft_quality && asphalt_outline && visible_asphalt)
        GdipDrawPath(graphics, asphalt_outline, asphalt_path);
    if (dirt_fill && visible_dirt)
        GdipDrawPath(graphics, dirt_fill, dirt_path);
    if (asphalt_fill && visible_asphalt)
        GdipDrawPath(graphics, asphalt_fill, asphalt_path);

cleanup:
    if (dirt_fill) GdipDeletePen(dirt_fill);
    if (dirt_outline) GdipDeletePen(dirt_outline);
    if (asphalt_fill) GdipDeletePen(asphalt_fill);
    if (asphalt_outline) GdipDeletePen(asphalt_outline);
    if (matrix) {
        GdipResetWorldTransform(graphics);
        GdipDeleteMatrix(matrix);
    }
    if (dirt_path) GdipDeletePath(dirt_path);
    if (asphalt_path) GdipDeletePath(asphalt_path);
}

void dvm_map_view_draw(const MapView *view, HDC dc, const RECT *client,
                       BOOL draft_quality)
{
    GpGraphics *graphics = NULL;
    double draw_width;
    double draw_height;
    double destination_left;
    double destination_top;
    double destination_right;
    double destination_bottom;
    GpImage *raster;
    double raster_width;
    double raster_height;
    if (!view || !dc || !client) return;
    SetDCBrushColor(dc, RGB(28, 32, 35));
    FillRect(dc, client, (HBRUSH)GetStockObject(DC_BRUSH));
    if (!view->image_loaded || !view->image || !(view->zoom > 0.0)) return;
    draw_width = (double)view->image_width * view->zoom;
    draw_height = (double)view->image_height * view->zoom;
    destination_left = fmax((double)client->left, view->pan_x);
    destination_top = fmax((double)client->top, view->pan_y);
    destination_right = fmin((double)client->right, view->pan_x + draw_width);
    destination_bottom = fmin((double)client->bottom, view->pan_y + draw_height);
    if (!(destination_right > destination_left && destination_bottom > destination_top)) return;

    raster = view->image;
    raster_width = (double)view->image_width;
    raster_height = (double)view->image_height;
    if (view->overview_image &&
        view->zoom <= (double)view->overview_width / (double)view->image_width) {
        raster = view->overview_image;
        raster_width = (double)view->overview_width;
        raster_height = (double)view->overview_height;
    }
    if (GdipCreateFromHDC(dc, &graphics) == Ok) {
        REAL source_left = (REAL)((destination_left - view->pan_x) / draw_width * raster_width);
        REAL source_top = (REAL)((destination_top - view->pan_y) / draw_height * raster_height);
        REAL source_width = (REAL)((destination_right - destination_left) / draw_width * raster_width);
        REAL source_height = (REAL)((destination_bottom - destination_top) / draw_height * raster_height);
        GdipSetCompositingMode(graphics, CompositingModeSourceCopy);
        GdipSetCompositingQuality(graphics, CompositingQualityHighSpeed);
        GdipSetInterpolationMode(graphics, draft_quality
            ? InterpolationModeNearestNeighbor : InterpolationModeBilinear);
        GdipSetPixelOffsetMode(graphics, PixelOffsetModeHalf);
        GdipDrawImageRectRect(graphics, raster,
            (REAL)destination_left, (REAL)destination_top,
            (REAL)(destination_right - destination_left),
            (REAL)(destination_bottom - destination_top),
            source_left, source_top, source_width, source_height,
            UnitPixel, NULL, NULL, NULL);
        draw_road_paths(view, graphics, draft_quality);
        GdipDeleteGraphics(graphics);
    }
}
