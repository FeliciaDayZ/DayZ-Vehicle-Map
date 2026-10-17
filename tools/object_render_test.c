#include "../src/map_objects.h"
#include <stdio.h>
#include <string.h>

static BOOL check_pixel(HDC dc, int x, int y, COLORREF expected)
{
    COLORREF actual = GetPixel(dc, x, y);
    if (actual == expected) return TRUE;
    fprintf(stderr, "pixel %d,%d: got %06lx, expected %06lx\n",
            x, y, (unsigned long)actual, (unsigned long)expected);
    return FALSE;
}

int main(int argc, char **argv)
{
    GdiplusStartupInput input;
    ULONG_PTR token = 0;
    MapObjectLayer *layer = NULL;
    wchar_t base[4096], error[512];
    HDC dc;
    HBITMAP bitmap;
    HGDIOBJ old;
    BITMAPINFO info;
    void *pixels = NULL;
    GpGraphics *graphics = NULL;
    BOOL loaded, valid = TRUE;
    MapObjectCacheStats cached, current;
    ZeroMemory(&input, sizeof(input));
    input.GdiplusVersion = 1;
    if (!GetCurrentDirectoryW(4096, base) || GdiplusStartup(&token, &input, NULL) != Ok) return 2;
    loaded = dvm_map_objects_load(&layer, base, L"nasdara.objects", 64.0, error, 512u);
    if (argc == 2 && strcmp(argv[1], "--reject") == 0) {
        valid = !loaded && !layer && error[0];
        dvm_map_objects_destroy(layer);
        GdiplusShutdown(token);
        return valid ? 0 : 3;
    }
    if (!loaded) { fwprintf(stderr, L"%ls\n", error); GdiplusShutdown(token); return 4; }
    if (dvm_map_objects_count(layer) != 1u) valid = FALSE;
    dc = CreateCompatibleDC(NULL);
    ZeroMemory(&info, sizeof(info));
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = 256;
    info.bmiHeader.biHeight = -256;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, NULL, 0);
    if (!dc || !bitmap || !pixels) return 5;
    old = SelectObject(dc, bitmap);
    if (GdipCreateFromHDC(dc, &graphics) != Ok) return 6;
    GdipGraphicsClear(graphics, 0xff000000u);
    dvm_map_objects_draw(layer, graphics, 2.0, 0.0, 0.0, 256, 256, TRUE);
    GdipFlush(graphics, FlushIntentionSync);
    // An 8 x 4 m model at (20,20), rotated 90 degrees and scaled 2x.
    // Its four coloured corners must keep their orientation, without mirroring.
    valid &= check_pixel(dc, 36, 84, RGB(255,0,0));
    valid &= check_pixel(dc, 36, 60, RGB(0,255,0));
    valid &= check_pixel(dc, 28, 84, RGB(0,0,255));
    valid &= check_pixel(dc, 28, 60, RGB(255,255,0));
    valid &= check_pixel(dc, 20, 70, RGB(0,0,0));
    valid &= check_pixel(dc, 44, 70, RGB(0,0,0));
    dvm_map_objects_cache_stats(layer, &cached);
    GdipGraphicsClear(graphics, 0xff000000u);
    dvm_map_objects_draw(layer, graphics, 2.0, 5.0, 3.0, 256, 256, TRUE);
    GdipFlush(graphics, FlushIntentionSync);
    valid &= check_pixel(dc, 41, 87, RGB(255,0,0));
    dvm_map_objects_cache_stats(layer, &current);
    valid &= current.rendered_tiles == cached.rendered_tiles &&
             current.picture_loads == cached.picture_loads;
    GdipGraphicsClear(graphics, 0xff000000u);
    dvm_map_objects_draw(layer, graphics, 4.0, 0.0, 0.0, 256, 256, FALSE);
    GdipFlush(graphics, FlushIntentionSync);
    valid &= check_pixel(dc, 72, 168, RGB(255,0,0));
    valid &= check_pixel(dc, 72, 120, RGB(0,255,0));
    valid &= check_pixel(dc, 56, 168, RGB(0,0,255));
    valid &= check_pixel(dc, 56, 120, RGB(255,255,0));
    GdipGraphicsClear(graphics, 0xff000000u);
    dvm_map_objects_draw(layer, graphics, 2.0, 240.0, 0.0, 256, 256, FALSE);
    GdipFlush(graphics, FlushIntentionSync);
    valid &= check_pixel(dc, 36, 84, RGB(0,0,0));
    // Return to an earlier zoom: no geometry rendering or picture reads.
    dvm_map_objects_cache_stats(layer, &cached);
    dvm_map_objects_draw(layer, graphics, 4.0, 0.0, 0.0, 256, 256, FALSE);
    dvm_map_objects_cache_stats(layer, &current);
    valid &= current.rendered_tiles == cached.rendered_tiles &&
             current.picture_loads == cached.picture_loads;

    // A large rotated object crosses several tile and spatial-index edges.
    GdipGraphicsClear(graphics, 0xff000000u);
    dvm_map_objects_draw(layer, graphics, 32.0, -384.0, -1152.0, 256, 256, FALSE);
    GdipFlush(graphics, FlushIntentionSync);
    valid &= check_pixel(dc, 64, 127, RGB(0,0,255));
    valid &= check_pixel(dc, 64, 128, RGB(0,0,255));
    valid &= check_pixel(dc, 64, 129, RGB(0,0,255));
    valid &= check_pixel(dc, 192, 127, RGB(255,0,0));
    valid &= check_pixel(dc, 192, 128, RGB(255,0,0));
    valid &= check_pixel(dc, 192, 129, RGB(255,0,0));
    {
        HWND window = CreateWindowExW(0, L"STATIC", L"Object cache test", 0,
            0, 0, 0, 0, HWND_MESSAGE, NULL, GetModuleHandleW(NULL), NULL);
        ULONGLONG deadline;
        MSG message;
        UINT notifications = 0;
        valid &= window && dvm_map_objects_set_async(layer, window, WM_APP + 77);
        GdipGraphicsClear(graphics, 0xff000000u);
        dvm_map_objects_draw(layer, graphics, 8.0, -32.0, -128.0, 256, 256, TRUE);
        GdipFlush(graphics, FlushIntentionSync);
        // The cached previous level stays visible while new detail is queued.
        valid &= check_pixel(dc, 112, 208, RGB(255,0,0));
        deadline = GetTickCount64() + 5000;
        do {
            dvm_map_objects_cache_stats(layer, &current);
            if (!current.pending_tiles) break;
            Sleep(1);
        } while (GetTickCount64() < deadline);
        valid &= current.pending_tiles == 0;
        while (PeekMessageW(&message, window, WM_APP + 77, WM_APP + 77, PM_REMOVE))
            ++notifications;
        valid &= notifications > 0;
        GdipGraphicsClear(graphics, 0xff000000u);
        dvm_map_objects_draw(layer, graphics, 8.0, -32.0, -128.0, 256, 256, FALSE);
        GdipFlush(graphics, FlushIntentionSync);
        valid &= check_pixel(dc, 112, 208, RGB(255,0,0));
        valid &= check_pixel(dc, 80, 208, RGB(0,0,255));
        // Stop with new work queued, as when hiding/switching/closing the map.
        dvm_map_objects_draw(layer, graphics, 16.0, -64.0, -256.0, 256, 256, FALSE);
        dvm_map_objects_set_async(layer, NULL, 0);
        DestroyWindow(window);
    }
    GdipDeleteGraphics(graphics);
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
    dvm_map_objects_destroy(layer);
    GdiplusShutdown(token);
    if (valid) puts("PASS: transforms, culling, tile reuse, seams, asynchronous detail and shutdown");
    return valid ? 0 : 7;
}
