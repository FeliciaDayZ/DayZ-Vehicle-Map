// Real Nasdara workload: report UI render latency separately from background
// preparation, and verify warm reuse, eviction, cancellation and idle behavior.
#include "../src/map_view.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define CACHE_MESSAGE (WM_APP + 91)

static double milliseconds(void)
{
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    return (double)now.QuadPart * 1000.0 / (double)frequency.QuadPart;
}

static int compare_times(const void *left, const void *right)
{
    double a = *(const double *)left, b = *(const double *)right;
    return a < b ? -1 : a > b;
}

static void report(const char *name, double *times, UINT count)
{
    double total = 0.0;
    UINT i;
    for (i = 0; i < count; ++i) total += times[i];
    qsort(times, count, sizeof(*times), compare_times);
    printf("%s: mean %.2f ms, p95 %.2f ms, max %.2f ms\n", name,
        total / count, times[(count - 1u) * 95u / 100u], times[count - 1u]);
}

#ifndef DVM_CACHE_LEGACY
static BOOL wait_for_tiles(MapObjectLayer *layer, HWND window)
{
    double deadline = milliseconds() + 15000.0;
    MapObjectCacheStats stats;
    MSG message;
    do {
        while (PeekMessageW(&message, window, CACHE_MESSAGE, CACHE_MESSAGE, PM_REMOVE)) {}
        dvm_map_objects_cache_stats(layer, &stats);
        if (!stats.pending_tiles) return TRUE;
        Sleep(1);
    } while (milliseconds() < deadline);
    fprintf(stderr, "Timed out waiting for %u tile requests\n", stats.pending_tiles);
    return FALSE;
}
#endif

int main(int argc, char **argv)
{
    GdiplusStartupInput input = {0};
    ULONG_PTR token = 0;
    MapView view;
    wchar_t base[4096], error[512];
    HDC dc;
    HBITMAP bitmap;
    HGDIOBJ old;
    BITMAPINFO info = {0};
    void *pixels = NULL;
    RECT rect = {0, 0, 1920, 1080};
    double times[80], started, pan_x, pan_y, zoom;
    UINT i;
    BOOL valid = TRUE;
#ifndef DVM_CACHE_LEGACY
    HWND window;
    MapObjectCacheStats before, after;
#endif
    if (argc == 3) { rect.right = atoi(argv[1]); rect.bottom = atoi(argv[2]); }
    if (rect.right < 256 || rect.bottom < 256 || rect.right > 8192 || rect.bottom > 8192) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
    input.GdiplusVersion = 1;
    if (!GetCurrentDirectoryW(4096, base) || GdiplusStartup(&token, &input, NULL) != Ok) return 2;
    if (!dvm_map_view_load(&view, base, 7, error, 512)) {
        fwprintf(stderr, L"%ls\n", error);
        return 2;
    }
    dvm_map_view_resize(&view, rect.right, rect.bottom);
    view.zoom = zoom = 10.5;
    view.pan_x = pan_x = rect.right * 0.5 - 10104.0 / 4.0 * zoom;
    view.pan_y = pan_y = rect.bottom * 0.5 - (16384.0 - 8228.0) / 4.0 * zoom;
    dc = CreateCompatibleDC(NULL);
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = rect.right;
    info.bmiHeader.biHeight = -rect.bottom;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, NULL, 0);
    if (!dc || !bitmap || !pixels) return 2;
    old = SelectObject(dc, bitmap);
#ifndef DVM_CACHE_LEGACY
    window = CreateWindowExW(0, L"STATIC", L"Nasdara cache benchmark", 0,
        0, 0, 0, 0, HWND_MESSAGE, NULL, GetModuleHandleW(NULL), NULL);
    if (!window || !dvm_map_objects_set_async(view.objects, window, CACHE_MESSAGE)) return 2;
#endif
    started = milliseconds();
    dvm_map_view_draw(&view, dc, &rect, TRUE);
    printf("Nasdara %ld x %ld, %u objects; cold UI frame %.2f ms\n",
        rect.right, rect.bottom, dvm_map_objects_count(view.objects), milliseconds() - started);
#ifndef DVM_CACHE_LEGACY
    valid &= wait_for_tiles(view.objects, window);
    printf("Initial background preparation %.2f ms\n", milliseconds() - started);
    dvm_map_objects_cache_stats(view.objects, &before);
#endif
    for (i = 0; i < 80u; ++i) {
        view.pan_x = pan_x + (int)(i % 9u) * 4 - 16;
        view.pan_y = pan_y + (int)(i % 7u) * 4 - 12;
        started = milliseconds();
        dvm_map_view_draw(&view, dc, &rect, TRUE);
        times[i] = milliseconds() - started;
    }
    report("Warm pan", times, 80);
#ifndef DVM_CACHE_LEGACY
    valid &= wait_for_tiles(view.objects, window);
    dvm_map_objects_cache_stats(view.objects, &after);
    valid &= before.rendered_tiles == after.rendered_tiles &&
             before.picture_loads == after.picture_loads;
#endif
    for (i = 0; i < 20u; ++i) {
        started = milliseconds();
        dvm_map_view_draw(&view, dc, &rect, FALSE);
        times[i] = milliseconds() - started;
    }
    report("Warm detailed frame", times, 20);
    for (i = 0; i < 80u; ++i) {
        view.zoom = zoom * pow(1.22, (int)(i % 9u) - 4);
        view.pan_x = rect.right * 0.5 - 10104.0 / 4.0 * view.zoom;
        view.pan_y = rect.bottom * 0.5 - (16384.0 - 8228.0) / 4.0 * view.zoom;
        started = milliseconds();
        dvm_map_view_draw(&view, dc, &rect, TRUE);
        times[i] = milliseconds() - started;
        Sleep(10);
    }
    report("Changing zoom (including uncached levels)", times, 80);
#ifndef DVM_CACHE_LEGACY
    valid &= wait_for_tiles(view.objects, window);
    // Visit enough disjoint areas to exceed the cache capacity, then return.
    // This also catches workers stuck on obsolete requests after rapid zooms.
    for (i = 0; i < 14u; ++i) {
        view.zoom = zoom;
        view.pan_x = rect.right * 0.5 - (1600.0 + (i % 5u) * 2800.0) / 4.0 * zoom;
        view.pan_y = rect.bottom * 0.5 - (1600.0 + (i / 5u) * 4000.0) / 4.0 * zoom;
        dvm_map_view_draw(&view, dc, &rect, TRUE);
        valid &= wait_for_tiles(view.objects, window);
    }
    view.pan_x = pan_x; view.pan_y = pan_y;
    dvm_map_view_draw(&view, dc, &rect, FALSE);
    valid &= wait_for_tiles(view.objects, window);
    dvm_map_objects_cache_stats(view.objects, &before);
    valid &= before.rendered_tiles > 480u && before.tile_bytes <= 128u * 1024u * 1024u;
    Sleep(100);
    dvm_map_objects_cache_stats(view.objects, &after);
    valid &= after.rendered_tiles == before.rendered_tiles && !after.pending_tiles;
    printf("Cache: %u tiles prepared, %.1f MiB resident, %u picture loads; idle pending %u\n",
        after.rendered_tiles, after.tile_bytes / (1024.0 * 1024.0), after.picture_loads, after.pending_tiles);
    // Cancel queued work and join an active worker during shutdown.
    view.zoom *= 2.0;
    dvm_map_view_draw(&view, dc, &rect, TRUE);
    dvm_map_objects_set_async(view.objects, NULL, 0);
    DestroyWindow(window);
#endif
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
    dvm_map_view_destroy(&view);
    GdiplusShutdown(token);
    puts(valid ? "PASS: real-map cache workload" : "FAIL: real-map cache workload");
    return valid ? 0 : 1;
}
