#define _CRT_SECURE_NO_WARNINGS
#include "map_objects.h"
#include "util.h"

#include <math.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define OBJECT_HEADER_BYTES 28u
#define OBJECT_CLASS_BYTES 60u
#define OBJECT_FILE_LIMIT (128u * 1024u * 1024u)
#define OBJECT_IMAGE_BUDGET (96u * 1024u * 1024u)
#define OBJECT_WRECK 1u
#define OBJECT_STATIC 2u
#define OBJECT_PICTURE 4u
#define OBJECT_GRID 64u
#define OBJECT_SCALE_UNITS 1048576.0
#define OBJECT_TILE_SIZE 256
#define OBJECT_TILE_PAD 2
#define OBJECT_TILE_PIXELS (OBJECT_TILE_SIZE + 2 * OBJECT_TILE_PAD)
#define OBJECT_TILE_COUNT 480u
#define OBJECT_REQUEST_COUNT 256u

typedef struct ObjectTileKey {
    int scale_key, x, y;
} ObjectTileKey;

typedef struct ObjectTile {
    ObjectTileKey key;
    GpBitmap *image;
    ULONGLONG used;
    BOOL valid;
} ObjectTile;

typedef struct ObjectRequest {
    ObjectTileKey key;
    double priority;
} ObjectRequest;

typedef struct ObjectModel {
    GpPath *outline;
    GpImage *image;
    UINT flags, width, height;
    REAL top[6];
    REAL bounds[4];
    ULONGLONG last_used;
    BOOL image_failed;
} ObjectModel;

typedef struct ObjectPlacement {
    UINT model;
    REAL x, z, cosine, sine, scale;
    REAL bounds[4];
    REAL area;
} ObjectPlacement;

struct MapObjectLayer {
    ObjectModel *models;
    ObjectPlacement *items;
    ObjectPlacement **visible;
    UINT *grid_offsets, *grid_items, *seen, query;
    UINT model_count, count;
    double world_size;
    wchar_t picture_directory[DVM_PATH_CAP];
    SIZE_T image_bytes;
    ULONGLONG clock;
    // Only the renderer thread touches models, pictures and the query scratch
    // space. Published tiles are immutable; the UI holds the lock while using
    // them, and rendering/file decoding never runs under that lock.
    CRITICAL_SECTION cache_lock;
    HANDLE worker, wake;
    HWND notify_window;
    UINT notify_message;
    LONG stopping, picture_loads;
    ObjectTile tiles[OBJECT_TILE_COUNT];
    ObjectRequest requests[OBJECT_REQUEST_COUNT];
    UINT request_count, rendered_tiles;
    BOOL busy;
    ULONGLONG tile_clock;
};

static uint32_t object_u32(const BYTE *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static float object_f32(const BYTE *p)
{
    uint32_t bits = object_u32(p);
    float result;
    memcpy(&result, &bits, sizeof(result));
    return result;
}

static BOOL picture_path(const MapObjectLayer *layer, UINT model, wchar_t *path)
{
    wchar_t name[32];
    _snwprintf(name, 32, L"%04u.png", model);
    return dvm_join_path(path, DVM_PATH_CAP, layer->picture_directory, name);
}

static int compare_placements(const void *left, const void *right)
{
    const ObjectPlacement *a = left, *b = right;
    if (a->area != b->area) return a->area > b->area ? -1 : 1;
    if (a->model != b->model) return a->model < b->model ? -1 : 1;
    if (a->x != b->x) return a->x < b->x ? -1 : 1;
    return a->z < b->z ? -1 : a->z > b->z;
}

void dvm_map_objects_destroy(MapObjectLayer *layer)
{
    UINT i;
    if (!layer) return;
    InterlockedExchange(&layer->stopping, 1);
    if (layer->worker) {
        SetEvent(layer->wake);
        WaitForSingleObject(layer->worker, INFINITE);
        CloseHandle(layer->worker);
    }
    if (layer->wake) CloseHandle(layer->wake);
    for (i = 0; i < OBJECT_TILE_COUNT; ++i)
        if (layer->tiles[i].image) GdipDisposeImage((GpImage *)layer->tiles[i].image);
    for (i = 0; i < layer->model_count; ++i) {
        if (layer->models[i].outline) GdipDeletePath(layer->models[i].outline);
        if (layer->models[i].image) GdipDisposeImage(layer->models[i].image);
    }
    if (layer->models) HeapFree(GetProcessHeap(), 0, layer->models);
    if (layer->items) HeapFree(GetProcessHeap(), 0, layer->items);
    if (layer->visible) HeapFree(GetProcessHeap(), 0, layer->visible);
    if (layer->grid_offsets) HeapFree(GetProcessHeap(), 0, layer->grid_offsets);
    if (layer->grid_items) HeapFree(GetProcessHeap(), 0, layer->grid_items);
    if (layer->seen) HeapFree(GetProcessHeap(), 0, layer->seen);
    DeleteCriticalSection(&layer->cache_lock);
    HeapFree(GetProcessHeap(), 0, layer);
}

static UINT grid_cell(const MapObjectLayer *layer, double coordinate)
{
    return (UINT)fmax(0.0, fmin(OBJECT_GRID - 1.0,
                               floor(coordinate * OBJECT_GRID / layer->world_size)));
}

static BOOL build_object_grid(MapObjectLayer *layer)
{
    UINT counts[OBJECT_GRID * OBJECT_GRID] = {0};
    UINT i, x, z, total = 0;
    layer->grid_offsets = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
        (OBJECT_GRID * OBJECT_GRID + 1u) * sizeof(UINT));
    layer->seen = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, layer->count * sizeof(UINT));
    if (!layer->grid_offsets || !layer->seen) return FALSE;
    for (i = 0; i < layer->count; ++i) {
        const REAL *b = layer->items[i].bounds;
        for (z = grid_cell(layer, b[1]); z <= grid_cell(layer, b[3]); ++z)
            for (x = grid_cell(layer, b[0]); x <= grid_cell(layer, b[2]); ++x) {
                if (++total > 16000000u) return FALSE;
                ++counts[z * OBJECT_GRID + x];
            }
    }
    for (i = 0; i < OBJECT_GRID * OBJECT_GRID; ++i)
        layer->grid_offsets[i + 1u] = layer->grid_offsets[i] + counts[i];
    layer->grid_items = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)total * sizeof(UINT));
    if (!layer->grid_items) return FALSE;
    ZeroMemory(counts, sizeof(counts));
    for (i = 0; i < layer->count; ++i) {
        const REAL *b = layer->items[i].bounds;
        for (z = grid_cell(layer, b[1]); z <= grid_cell(layer, b[3]); ++z)
            for (x = grid_cell(layer, b[0]); x <= grid_cell(layer, b[2]); ++x) {
                UINT cell = z * OBJECT_GRID + x;
                layer->grid_items[layer->grid_offsets[cell] + counts[cell]++] = i;
            }
    }
    return TRUE;
}

static int compare_visible(const void *left, const void *right)
{
    const ObjectPlacement *a = *(ObjectPlacement *const *)left;
    const ObjectPlacement *b = *(ObjectPlacement *const *)right;
    // Placements were sorted by area at load time. Restore that draw order
    // after querying several cells, including objects crossing cell edges.
    return a < b ? -1 : a > b;
}

UINT dvm_map_objects_count(const MapObjectLayer *layer)
{
    return layer ? layer->count : 0u;
}

BOOL dvm_map_objects_load(MapObjectLayer **result, const wchar_t *base,
                          const wchar_t *relative_path, double world_size,
                          wchar_t *error, size_t error_capacity)
{
    wchar_t path[DVM_PATH_CAP], directory[DVM_PATH_CAP], *slash;
    HANDLE file = INVALID_HANDLE_VALUE;
    LARGE_INTEGER size;
    DWORD read = 0;
    BYTE *data = NULL;
    const BYTE *rings, *points, *placements;
    MapObjectLayer *layer = NULL;
    UINT class_count, count, ring_count, point_count, i;
    uint64_t expected;
    BOOL success = FALSE;
    if (!result) return FALSE;
    *result = NULL;
    if (!relative_path) return TRUE;
    if (!dvm_join_path(path, DVM_PATH_CAP, base, relative_path)) goto failed;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE || !GetFileSizeEx(file, &size) ||
        size.QuadPart < OBJECT_HEADER_BYTES || size.QuadPart > OBJECT_FILE_LIMIT) goto failed;
    data = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)size.QuadPart);
    if (!data || !ReadFile(file, data, (DWORD)size.QuadPart, &read, NULL) ||
        read != (DWORD)size.QuadPart || memcmp(data, "DVMOBJ1\0", 8u) != 0 ||
        !isfinite(object_f32(data + 8u)) ||
        fabs(object_f32(data + 8u) - world_size) > 0.01) goto failed;
    class_count = object_u32(data + 12u);
    count = object_u32(data + 16u);
    ring_count = object_u32(data + 20u);
    point_count = object_u32(data + 24u);
    expected = OBJECT_HEADER_BYTES + (uint64_t)class_count * OBJECT_CLASS_BYTES +
               (uint64_t)ring_count * 8u + (uint64_t)point_count * 8u + (uint64_t)count * 20u;
    if (!class_count || class_count > 8192u || !count || count > 2000000u ||
        !ring_count || ring_count > 200000u || !point_count || point_count > 4000000u ||
        expected != (uint64_t)size.QuadPart) goto failed;
    rings = data + OBJECT_HEADER_BYTES + (SIZE_T)class_count * OBJECT_CLASS_BYTES;
    points = rings + (SIZE_T)ring_count * 8u;
    placements = points + (SIZE_T)point_count * 8u;
    layer = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*layer));
    if (!layer) goto failed;
    InitializeCriticalSection(&layer->cache_lock);
    layer->world_size = world_size;
    layer->models = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (SIZE_T)class_count * sizeof(*layer->models));
    layer->items = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (SIZE_T)count * sizeof(*layer->items));
    layer->visible = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)count * sizeof(*layer->visible));
    if (!layer->models || !layer->items || !layer->visible) goto failed;
    layer->model_count = class_count;
    layer->count = count;
    wcscpy(directory, path);
    slash = wcsrchr(directory, L'\\');
    if (!slash) goto failed;
    *slash = L'\0';
    if (!dvm_join_path(layer->picture_directory, DVM_PATH_CAP, directory, L"pictures")) goto failed;
    for (i = 0; i < class_count; ++i) {
        const BYTE *record = data + OBJECT_HEADER_BYTES + (SIZE_T)i * OBJECT_CLASS_BYTES;
        ObjectModel *model = &layer->models[i];
        UINT first = object_u32(record), length = object_u32(record + 4u), j;
        model->flags = object_u32(record + 8u);
        model->width = object_u32(record + 12u);
        model->height = object_u32(record + 16u);
        if (!length || first > ring_count || length > ring_count - first ||
            model->flags > 7u || model->width > 4096u || model->height > 4096u) goto failed;
        for (j = 0; j < 10u; ++j) {
            float value = object_f32(record + 20u + j * 4u);
            if (!isfinite(value) || fabs(value) > 10000.0) goto failed;
            if (j < 6u) model->top[j] = value;
            else model->bounds[j - 6u] = value;
        }
        if (model->bounds[0] > model->bounds[2] || model->bounds[1] > model->bounds[3]) goto failed;
        if (model->flags & OBJECT_PICTURE) {
            if (!model->width || !model->height || !picture_path(layer, i, path) ||
                GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) goto failed;
        }
        if (GdipCreatePath(FillModeWinding, &model->outline) != Ok) goto failed;
        for (j = 0; j < length; ++j) {
            const BYTE *ring = rings + (SIZE_T)(first + j) * 8u;
            UINT start = object_u32(ring), n = object_u32(ring + 4u), k;
            GpPointF *polygon;
            GpStatus status;
            if (n < 3u || n > 65536u || start > point_count || n > point_count - start) goto failed;
            polygon = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)n * sizeof(*polygon));
            if (!polygon) goto failed;
            for (k = 0; k < n; ++k) {
                polygon[k].X = object_f32(points + (SIZE_T)(start + k) * 8u);
                polygon[k].Y = object_f32(points + (SIZE_T)(start + k) * 8u + 4u);
                if (!isfinite(polygon[k].X) || !isfinite(polygon[k].Y) ||
                    fabs(polygon[k].X) > 10000.0 || fabs(polygon[k].Y) > 10000.0) break;
            }
            status = k == n ? GdipAddPathPolygon(model->outline, polygon, (INT)n) : InvalidParameter;
            HeapFree(GetProcessHeap(), 0, polygon);
            if (status != Ok) goto failed;
        }
    }
    for (i = 0; i < count; ++i) {
        const BYTE *record = placements + (SIZE_T)i * 20u;
        ObjectPlacement *item = &layer->items[i];
        const ObjectModel *model;
        float angle;
        UINT corner;
        item->model = object_u32(record);
        item->x = object_f32(record + 4u);
        item->z = object_f32(record + 8u);
        angle = object_f32(record + 12u);
        item->scale = object_f32(record + 16u);
        if (item->model >= class_count || !isfinite(item->x) || !isfinite(item->z) ||
            !isfinite(angle) || !isfinite(item->scale) || fabs(angle) > 36000.0 ||
            item->x < -512.0 || item->z < -512.0 || item->x > world_size + 512.0 ||
            item->z > world_size + 512.0 || item->scale <= 0.0f || item->scale > 100.0f) goto failed;
        item->cosine = (REAL)(cos(angle * 0.017453292519943295) * item->scale);
        item->sine = (REAL)(sin(angle * 0.017453292519943295) * item->scale);
        model = &layer->models[item->model];
        for (corner = 0; corner < 4u; ++corner) {
            REAL x = model->bounds[(corner & 1u) ? 2u : 0u];
            REAL z = model->bounds[(corner & 2u) ? 3u : 1u];
            REAL wx = item->x + x * item->cosine - z * item->sine;
            REAL wz = item->z + x * item->sine + z * item->cosine;
            if (!corner) {
                item->bounds[0] = item->bounds[2] = wx;
                item->bounds[1] = item->bounds[3] = wz;
            } else {
                item->bounds[0] = fminf(item->bounds[0], wx);
                item->bounds[1] = fminf(item->bounds[1], wz);
                item->bounds[2] = fmaxf(item->bounds[2], wx);
                item->bounds[3] = fmaxf(item->bounds[3], wz);
            }
        }
        item->area = (model->bounds[2] - model->bounds[0]) *
                     (model->bounds[3] - model->bounds[1]) * item->scale * item->scale;
    }
    qsort(layer->items, count, sizeof(*layer->items), compare_placements);
    if (!build_object_grid(layer)) goto failed;
    *result = layer;
    layer = NULL;
    success = TRUE;
failed:
    if (!success) dvm_set_error(error, error_capacity,
        L"Cannot load structure data or model pictures:\n%ls", relative_path);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (data) HeapFree(GetProcessHeap(), 0, data);
    dvm_map_objects_destroy(layer);
    return success;
}

static GpImage *model_picture(MapObjectLayer *layer, UINT index)
{
    ObjectModel *model = &layer->models[index];
    wchar_t path[DVM_PATH_CAP];
    SIZE_T bytes = (SIZE_T)model->width * model->height * 4u;
    UINT width, height;
    GpImage *source = NULL;
    GpBitmap *decoded = NULL;
    GpGraphics *graphics = NULL;
    GpStatus status = GenericError;
    model->last_used = ++layer->clock;
    if (model->image || model->image_failed) return model->image;
    while (layer->image_bytes + bytes > OBJECT_IMAGE_BUDGET) {
        ObjectModel *oldest = NULL;
        UINT i;
        for (i = 0; i < layer->model_count; ++i) {
            ObjectModel *candidate = &layer->models[i];
            if (candidate->image && (!oldest || candidate->last_used < oldest->last_used)) oldest = candidate;
        }
        if (!oldest) break;
        GdipDisposeImage(oldest->image);
        oldest->image = NULL;
        layer->image_bytes -= (SIZE_T)oldest->width * oldest->height * 4u;
    }
    InterlockedIncrement(&layer->picture_loads);
    if (picture_path(layer, index, path) && GdipLoadImageFromFile(path, &source) == Ok &&
        source && GdipGetImageWidth(source, &width) == Ok &&
        GdipGetImageHeight(source, &height) == Ok &&
        width == model->width && height == model->height &&
        GdipCreateBitmapFromScan0((INT)width, (INT)height, 0,
            PixelFormat32bppPARGB, NULL, &decoded) == Ok &&
        GdipGetImageGraphicsContext(decoded, &graphics) == Ok) {
        GdipSetCompositingMode(graphics, CompositingModeSourceCopy);
        status = GdipDrawImageRectRectI(graphics, source, 0, 0, width, height,
            0, 0, width, height, UnitPixel, NULL, NULL, NULL);
    }
    if (graphics) GdipDeleteGraphics(graphics);
    if (source) GdipDisposeImage(source);
    if (status == Ok) {
        model->image = (GpImage *)decoded;
        layer->image_bytes += bytes;
    } else {
        if (decoded) GdipDisposeImage((GpImage *)decoded);
        model->image_failed = TRUE;
    }
    return model->image;
}

static void draw_objects_uncached(MapObjectLayer *layer, GpGraphics *graphics,
                          double k, double pan_x, double pan_y,
                          int width, int height, BOOL draft)
{
    GpMatrix *matrix = NULL;
    GpPen *edge = NULL;
    GpSolidFill *plain = NULL, *wreck = NULL;
    double x0, z0, x1, z1;
    UINT i, visible_count = 0, pass, gx, gz;
    if (!layer || !graphics || k < 0.125) return;
    x0 = -pan_x / k; x1 = (width - pan_x) / k;
    z1 = layer->world_size + pan_y / k;
    z0 = layer->world_size - (height - pan_y) / k;
    if (++layer->query == 0u) {
        ZeroMemory(layer->seen, layer->count * sizeof(UINT));
        ++layer->query;
    }
    for (gz = grid_cell(layer, z0); gz <= grid_cell(layer, z1); ++gz)
    for (gx = grid_cell(layer, x0); gx <= grid_cell(layer, x1); ++gx) {
      UINT cell = gz * OBJECT_GRID + gx, entry;
      for (entry = layer->grid_offsets[cell]; entry < layer->grid_offsets[cell + 1u]; ++entry) {
        i = layer->grid_items[entry];
        if (layer->seen[i] == layer->query) continue;
        layer->seen[i] = layer->query;
        ObjectPlacement *item = &layer->items[i];
        const ObjectModel *model = &layer->models[item->model];
        if ((model->flags & OBJECT_STATIC) && k < 1.0) continue;
        if (item->bounds[2] < x0 || item->bounds[0] > x1 ||
            item->bounds[3] < z0 || item->bounds[1] > z1) continue;
        if (fmax(item->bounds[2] - item->bounds[0], item->bounds[3] - item->bounds[1]) * k < 0.75) continue;
        layer->visible[visible_count++] = item;
      }
    }
    qsort(layer->visible, visible_count, sizeof(*layer->visible), compare_visible);
    if (!visible_count || GdipCreateMatrix(&matrix) != Ok ||
        GdipCreatePen1(0xe6101010u, 1.0f, UnitWorld, &edge) != Ok ||
        GdipCreateSolidFill(0xeb5c5c5cu, &plain) != Ok ||
        GdipCreateSolidFill(0xeb784637u, &wreck) != Ok) goto cleanup;
    GdipSetPenLineJoin(edge, LineJoinRound);
    GdipSetCompositingMode(graphics, CompositingModeSourceOver);
    GdipSetCompositingQuality(graphics, CompositingQualityHighSpeed);
    GdipSetInterpolationMode(graphics, draft ? InterpolationModeNearestNeighbor : InterpolationModeBilinear);
    GdipSetSmoothingMode(graphics, draft ? SmoothingModeHighSpeed : SmoothingModeAntiAlias);
    GdipSetPixelOffsetMode(graphics, PixelOffsetModeHalf);
    // Outlines first; then largest pictures first, so tents and containers sit
    // over their concrete platforms. All geometry uses the same metre scale.
    for (pass = 0; pass < 2u; ++pass) for (i = 0; i < visible_count; ++i) {
        if ((i & 31u) == 0u && InterlockedCompareExchange(&layer->stopping, 0, 0)) goto cleanup;
        const ObjectPlacement *item = layer->visible[i];
        ObjectModel *model = &layer->models[item->model];
        BOOL picture = k >= 0.5 && (model->flags & OBJECT_PICTURE) &&
                       !model->image_failed;
        if (pass == 0u && picture && (model->flags & OBJECT_STATIC)) continue;
        if (pass == 1u && !picture) continue;
        GdipSetMatrixElements(matrix, (REAL)(k * item->cosine), (REAL)(-k * item->sine),
            (REAL)(-k * item->sine), (REAL)(-k * item->cosine),
            (REAL)(pan_x + k * item->x), (REAL)(pan_y + k * (layer->world_size - item->z)));
        GdipSetWorldTransform(graphics, matrix);
        if (pass == 0u) {
            REAL stroke = (REAL)((picture ? 1.5 : fmax(0.5, fmin(2.5, 0.9 * k))) / (k * item->scale));
            GdipSetPenWidth(edge, stroke);
            GdipDrawPath(graphics, edge, model->outline);
            if (!picture) GdipFillPath(graphics, (GpBrush *)((model->flags & OBJECT_WRECK) ? wreck : plain), model->outline);
        } else {
            GpImage *img = model_picture(layer, item->model);
            if (img) {
                const REAL *a = model->top;
                GpPointF target[3] = {
                    {a[4], a[5]},
                    {a[4] + a[0] * model->width, a[5] + a[1] * model->width},
                    {a[4] + a[2] * model->height, a[5] + a[3] * model->height}
                };
                GdipDrawImagePointsRect(graphics, img, target, 3, 0.0f, 0.0f,
                    (REAL)model->width, (REAL)model->height, UnitPixel, NULL, NULL, NULL);
            } else GdipFillPath(graphics, (GpBrush *)plain, model->outline);
        }
    }
cleanup:
    GdipResetWorldTransform(graphics);
    if (matrix) GdipDeleteMatrix(matrix);
    if (edge) GdipDeletePen(edge);
    if (plain) GdipDeleteBrush((GpBrush *)plain);
    if (wreck) GdipDeleteBrush((GpBrush *)wreck);
}

static BOOL same_tile(ObjectTileKey a, ObjectTileKey b)
{
    return a.scale_key == b.scale_key && a.x == b.x && a.y == b.y;
}

static ObjectTile *find_tile(MapObjectLayer *layer, ObjectTileKey key)
{
    UINT i;
    for (i = 0; i < OBJECT_TILE_COUNT; ++i)
        if (layer->tiles[i].valid && same_tile(layer->tiles[i].key, key))
            return &layer->tiles[i];
    return NULL;
}

static GpBitmap *render_tile(MapObjectLayer *layer, ObjectTileKey key)
{
    GpBitmap *image = NULL;
    GpGraphics *graphics = NULL;
    if (GdipCreateBitmapFromScan0(OBJECT_TILE_PIXELS, OBJECT_TILE_PIXELS, 0,
        PixelFormat32bppPARGB, NULL, &image) != Ok) return NULL;
    if (GdipGetImageGraphicsContext(image, &graphics) != Ok) {
        GdipDisposeImage((GpImage *)image);
        return NULL;
    }
    GdipGraphicsClear(graphics, 0u);
    draw_objects_uncached(layer, graphics, (key.scale_key / OBJECT_SCALE_UNITS),
        -(double)key.x * OBJECT_TILE_SIZE + OBJECT_TILE_PAD,
        -(double)key.y * OBJECT_TILE_SIZE + OBJECT_TILE_PAD,
        OBJECT_TILE_PIXELS, OBJECT_TILE_PIXELS, FALSE);
    GdipDeleteGraphics(graphics);
    return image;
}

static void publish_tile(MapObjectLayer *layer, ObjectTileKey key, GpBitmap *image)
{
    ObjectTile *target = &layer->tiles[0];
    UINT i;
    for (i = 0; i < OBJECT_TILE_COUNT; ++i) {
        ObjectTile *candidate = &layer->tiles[i];
        if (!candidate->valid) { target = candidate; break; }
        if (candidate->used < target->used) target = candidate;
    }
    if (target->image) GdipDisposeImage((GpImage *)target->image);
    target->key = key;
    target->image = image;
    target->valid = TRUE;
    target->used = ++layer->tile_clock;
    ++layer->rendered_tiles;
}

static DWORD WINAPI object_worker(void *context)
{
    MapObjectLayer *layer = context;
    for (;;) {
        ObjectTileKey key = {0, 0, 0};
        GpBitmap *image;
        BOOL found = FALSE;
        UINT i;
        EnterCriticalSection(&layer->cache_lock);
        if (!InterlockedCompareExchange(&layer->stopping, 0, 0)) {
            for (i = 0; i < layer->request_count; ++i) {
                if (!find_tile(layer, layer->requests[i].key)) {
                    key = layer->requests[i].key;
                    found = TRUE;
                    break;
                }
            }
        }
        layer->busy = found;
        LeaveCriticalSection(&layer->cache_lock);
        if (InterlockedCompareExchange(&layer->stopping, 0, 0)) break;
        if (!found) { WaitForSingleObject(layer->wake, INFINITE); continue; }

        image = render_tile(layer, key);
        EnterCriticalSection(&layer->cache_lock);
        publish_tile(layer, key, image);
        layer->busy = FALSE;
        if (layer->notify_window && !InterlockedCompareExchange(&layer->stopping, 0, 0))
            PostMessageW(layer->notify_window, layer->notify_message, 0, (LPARAM)layer);
        LeaveCriticalSection(&layer->cache_lock);
    }
    return 0;
}

BOOL dvm_map_objects_set_async(MapObjectLayer *layer, HWND window, UINT message)
{
    if (!layer) return TRUE;
    EnterCriticalSection(&layer->cache_lock);
    layer->notify_window = window;
    layer->notify_message = message;
    if (!window) layer->request_count = 0;
    LeaveCriticalSection(&layer->cache_lock);
    if (window && !layer->worker) {
        if (!layer->wake) layer->wake = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (!layer->wake) return FALSE;
        layer->worker = CreateThread(NULL, 0, object_worker, layer, 0, NULL);
        if (!layer->worker) return FALSE;
        SetThreadPriority(layer->worker, THREAD_PRIORITY_BELOW_NORMAL);
    }
    return TRUE;
}

void dvm_map_objects_cache_stats(MapObjectLayer *layer, MapObjectCacheStats *stats)
{
    UINT i;
    ZeroMemory(stats, sizeof(*stats));
    if (!layer) return;
    EnterCriticalSection(&layer->cache_lock);
    stats->rendered_tiles = layer->rendered_tiles;
    stats->picture_loads = (UINT)InterlockedCompareExchange(&layer->picture_loads, 0, 0);
    stats->pending_tiles = layer->busy ? 1u : 0u;
    for (i = 0; i < layer->request_count; ++i)
        if (!find_tile(layer, layer->requests[i].key)) ++stats->pending_tiles;
    for (i = 0; i < OBJECT_TILE_COUNT; ++i)
        if (layer->tiles[i].image)
            stats->tile_bytes += (SIZE_T)OBJECT_TILE_PIXELS * OBJECT_TILE_PIXELS * 4u;
    LeaveCriticalSection(&layer->cache_lock);
}

static int compare_requests(const void *left, const void *right)
{
    const ObjectRequest *a = left, *b = right;
    return a->priority < b->priority ? -1 : a->priority > b->priority;
}

static void draw_tile(MapObjectLayer *layer, GpGraphics *graphics, ObjectTile *tile,
                      double k, double pan_x, double pan_y,
                      double left, double top, double right, double bottom)
{
    double scale = k / (tile->key.scale_key / OBJECT_SCALE_UNITS);
    double x = pan_x + tile->key.x * OBJECT_TILE_SIZE * scale;
    double y = pan_y + tile->key.y * OBJECT_TILE_SIZE * scale;
    double l = fmax(x, left), t = fmax(y, top);
    double r = fmin(x + OBJECT_TILE_SIZE * scale, right);
    double b = fmin(y + OBJECT_TILE_SIZE * scale, bottom);
    if (r <= l || b <= t) return;
    tile->used = ++layer->tile_clock;
    if (tile->image) GdipDrawImageRectRect(graphics, (GpImage *)tile->image,
        (REAL)l, (REAL)t, (REAL)(r - l), (REAL)(b - t),
        (REAL)(OBJECT_TILE_PAD + (l - x) / scale),
        (REAL)(OBJECT_TILE_PAD + (t - y) / scale),
        (REAL)((r - l) / scale), (REAL)((b - t) / scale), UnitPixel, NULL, NULL, NULL);
}

static void draw_fallback(MapObjectLayer *layer, GpGraphics *graphics,
                          ObjectTileKey key, double k, double pan_x, double pan_y,
                          double left, double top, double right, double bottom)
{
    int best = -1, distance = INT_MAX;
    UINT i;
    for (i = 0; i < OBJECT_TILE_COUNT; ++i) {
        ObjectTile *tile = &layer->tiles[i];
        double scale, x, y;
        if (!tile->valid || abs(tile->key.scale_key - key.scale_key) >= distance) continue;
        scale = k / (tile->key.scale_key / OBJECT_SCALE_UNITS);
        x = pan_x + tile->key.x * OBJECT_TILE_SIZE * scale;
        y = pan_y + tile->key.y * OBJECT_TILE_SIZE * scale;
        if (x >= right || y >= bottom || x + OBJECT_TILE_SIZE * scale <= left ||
            y + OBJECT_TILE_SIZE * scale <= top) continue;
        distance = abs(tile->key.scale_key - key.scale_key);
        best = tile->key.scale_key;
    }
    // Crop fallback tiles to the missing tile's rectangle. Drawing a complete
    // old layer underneath the new layer would double-blend translucent roofs.
    for (i = 0; i < OBJECT_TILE_COUNT; ++i)
        if (layer->tiles[i].valid && layer->tiles[i].key.scale_key == best)
            draw_tile(layer, graphics, &layer->tiles[i], k, pan_x, pan_y,
                       left, top, right, bottom);
}

void dvm_map_objects_draw(MapObjectLayer *layer, GpGraphics *graphics,
                          double k, double pan_x, double pan_y,
                          int width, int height, BOOL draft)
{
    int scale_key, x0, y0, x1, y1, limit, x, y;
    double scale, center_x, center_y;
    GraphicsState state;
    UINT count = 0;
    ObjectRequest requests[OBJECT_REQUEST_COUNT];
    if (!layer || !graphics) return;
    if (k < 0.125 || width <= 0 || height <= 0) {
        EnterCriticalSection(&layer->cache_lock);
        layer->request_count = 0;
        LeaveCriticalSection(&layer->cache_lock);
        return;
    }
    scale_key = max(1, (int)floor(k * OBJECT_SCALE_UNITS + 0.5));
    // Bake at the requested zoom so ready tiles need only a fast pixel copy.
    // Fixed-point keys absorb floating-point drift when revisiting a zoom.
    // Extremely large windows use a coarser scale to bound the working set.
    do {
        scale = k / (scale_key / OBJECT_SCALE_UNITS);
        limit = (int)ceil(layer->world_size * (scale_key / OBJECT_SCALE_UNITS) / OBJECT_TILE_SIZE) - 1;
        x0 = max(0, (int)floor(-pan_x / (OBJECT_TILE_SIZE * scale)) - 1);
        y0 = max(0, (int)floor(-pan_y / (OBJECT_TILE_SIZE * scale)) - 1);
        x1 = min(limit, (int)floor((width - pan_x) / (OBJECT_TILE_SIZE * scale)) + 1);
        y1 = min(limit, (int)floor((height - pan_y) / (OBJECT_TILE_SIZE * scale)) + 1);
        if ((double)(x1 - x0 + 1) * (y1 - y0 + 1) <= OBJECT_REQUEST_COUNT) break;
        scale_key = max(1, scale_key / 2);
    } while (scale_key > 1);
    center_x = (width * 0.5 - pan_x) / (OBJECT_TILE_SIZE * scale);
    center_y = (height * 0.5 - pan_y) / (OBJECT_TILE_SIZE * scale);
    GdipSaveGraphics(graphics, &state);
    GdipSetCompositingMode(graphics, CompositingModeSourceOver);
    GdipSetCompositingQuality(graphics, CompositingQualityHighSpeed);
    (void)draft;
    GdipSetInterpolationMode(graphics, InterpolationModeNearestNeighbor);
    GdipSetPixelOffsetMode(graphics, PixelOffsetModeHalf);
    EnterCriticalSection(&layer->cache_lock);
    for (y = y0; y <= y1; ++y) for (x = x0; x <= x1; ++x) {
        ObjectTileKey key = {scale_key, x, y};
        ObjectTile *tile = find_tile(layer, key);
        double left = fmax(0.0, pan_x + x * OBJECT_TILE_SIZE * scale);
        double top = fmax(0.0, pan_y + y * OBJECT_TILE_SIZE * scale);
        double right = fmin(width, pan_x + (x + 1) * OBJECT_TILE_SIZE * scale);
        double bottom = fmin(height, pan_y + (y + 1) * OBJECT_TILE_SIZE * scale);
        BOOL visible = right > left && bottom > top;
        requests[count].key = key;
        requests[count++].priority = (x + 0.5 - center_x) * (x + 0.5 - center_x) +
            (y + 0.5 - center_y) * (y + 0.5 - center_y) + (visible ? 0.0 : 1000000.0);
        if (tile) tile->used = ++layer->tile_clock;
        // Standalone/offline rendering is synchronous until a worker is enabled.
        if (!tile && !layer->worker && visible) {
            GpBitmap *image = render_tile(layer, key);
            publish_tile(layer, key, image);
            tile = find_tile(layer, key);
        }
        if (!visible) continue;
        if (tile) draw_tile(layer, graphics, tile, k, pan_x, pan_y, left, top, right, bottom);
        else draw_fallback(layer, graphics, key, k, pan_x, pan_y, left, top, right, bottom);
    }
    if (layer->worker && layer->notify_window) {
        qsort(requests, count, sizeof(*requests), compare_requests);
        memcpy(layer->requests, requests, count * sizeof(*requests));
        layer->request_count = count;
        SetEvent(layer->wake);
    }
    LeaveCriticalSection(&layer->cache_lock);
    GdipRestoreGraphics(graphics, state);
}
