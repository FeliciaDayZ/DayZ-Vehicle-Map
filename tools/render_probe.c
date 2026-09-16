#include "../src/map_view.h"

#include <gdiplus/gdiplus.h>
#include <stdio.h>
#include <windows.h>

static BOOL save_bmp(const wchar_t *path, const void *pixels, int width, int height)
{
    BITMAPFILEHEADER file_header;
    BITMAPINFOHEADER image_header;
    HANDLE file;
    DWORD written;
    DWORD image_size = (DWORD)((size_t)width * (size_t)height * 4u);
    ZeroMemory(&file_header, sizeof(file_header));
    ZeroMemory(&image_header, sizeof(image_header));
    file_header.bfType = 0x4d42u;
    file_header.bfOffBits = sizeof(file_header) + sizeof(image_header);
    file_header.bfSize = file_header.bfOffBits + image_size;
    image_header.biSize = sizeof(image_header);
    image_header.biWidth = width;
    image_header.biHeight = -height;
    image_header.biPlanes = 1;
    image_header.biBitCount = 32;
    image_header.biCompression = BI_RGB;
    image_header.biSizeImage = image_size;
    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    if (!WriteFile(file, &file_header, sizeof(file_header), &written, NULL) ||
        written != sizeof(file_header) ||
        !WriteFile(file, &image_header, sizeof(image_header), &written, NULL) ||
        written != sizeof(image_header) ||
        !WriteFile(file, pixels, image_size, &written, NULL) ||
        written != image_size) {
        CloseHandle(file);
        return FALSE;
    }
    CloseHandle(file);
    return TRUE;
}

int main(void)
{
    GdiplusStartupInput input;
    ULONG_PTR token = 0;
    wchar_t base[MAX_PATH];
    int map_index;
    ZeroMemory(&input, sizeof(input));
    input.GdiplusVersion = 1;
    if (!GetCurrentDirectoryW(MAX_PATH, base) ||
        GdiplusStartup(&token, &input, NULL) != Ok) return 1;
    for (map_index = 0; map_index < DVM_MAP_COUNT; ++map_index) {
        const MapInfo *map = dvm_map_info(map_index);
        MapView view;
        wchar_t error[512];
        wchar_t output[MAX_PATH];
        BITMAPINFO info;
        void *pixels = NULL;
        HBITMAP bitmap;
        HGDIOBJ old_bitmap;
        HDC dc = CreateCompatibleDC(NULL);
        RECT rect = { 0, 0, 1000, 760 };
        ULONGLONG started;
        int frame;
        if (!dvm_map_view_load(&view, base, map_index, error, 512u)) {
            fwprintf(stderr, L"%ls\n", error);
            return 2;
        }
        dvm_map_view_resize(&view, 1000, 760);
        dvm_map_view_zoom_at(&view, 8.0, 500.0, 380.0);
        ZeroMemory(&info, sizeof(info));
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = 1000;
        info.bmiHeader.biHeight = -760;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, NULL, 0);
        if (!dc || !bitmap || !pixels) return 3;
        old_bitmap = SelectObject(dc, bitmap);
        started = GetTickCount64();
        for (frame = 0; frame < 5; ++frame)
            dvm_map_view_draw(&view, dc, &rect, FALSE);
        wprintf(L"%ls high-quality %.1f ms/frame, asphalt=%u dirt=%u\n",
                map->display_name, (double)(GetTickCount64() - started) / 5.0,
                view.asphalt_segment_count, view.dirt_segment_count);
        started = GetTickCount64();
        for (frame = 0; frame < 20; ++frame) {
            dvm_map_view_pan(&view, frame & 1 ? 2.0 : -2.0, 0.0);
            dvm_map_view_draw(&view, dc, &rect, TRUE);
        }
        wprintf(L"%ls draft-pan %.1f ms/frame\n", map->display_name,
                (double)(GetTickCount64() - started) / 20.0);
        dvm_map_view_draw(&view, dc, &rect, FALSE);
        _snwprintf(output, MAX_PATH, L"build\\road_probe_%S.bmp", map->id);
        output[MAX_PATH - 1] = L'\0';
        if (!save_bmp(output, pixels, 1000, 760)) return 4;
        SelectObject(dc, old_bitmap);
        DeleteObject(bitmap);
        DeleteDC(dc);
        dvm_map_view_destroy(&view);
    }
    GdiplusShutdown(token);
    return 0;
}
