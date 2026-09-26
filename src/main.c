#define _CRT_SECURE_NO_WARNINGS

#include "app_data.h"
#include "map_view.h"
#include "state.h"
#include "util.h"

#include "../res/resource.h"

#include <commctrl.h>
#include <gdiplus/gdiplus.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <windows.h>
#include <windowsx.h>
#include <wctype.h>

#define MAIN_CLASS_NAME L"DayZVehicleMapMainWindow"
#define VIEW_CLASS_NAME L"DayZVehicleMapViewport"

#define ID_MAP_LABEL 100
#define ID_MAP_COMBO 101
#define ID_RESTORE_ALL 102
#define ID_FIT_MAP 103
#define ID_TYPE_LABEL 104
#define ID_STATUS 105
#define ID_VIEWPORT 106
#define ID_TYPE_CHECK_BASE 200
#define ID_TYPE_RESTORE_BASE 300

#define ID_STATUS_UPDATE_TIMER 1u
#define STATUS_UPDATE_INTERVAL_MS 40u
#define ID_ZOOM_QUALITY_TIMER 2u
#define ZOOM_SETTLE_INTERVAL_MS 120u
#define ID_VIEWPORT_FRAME_TIMER 3u
#define VIEWPORT_FRAME_INTERVAL_MS 40u

typedef struct App {
    HINSTANCE instance;
    HWND main_window;
    HWND viewport;
    HWND map_combo;
    HWND restore_all_button;
    HWND fit_button;
    HWND type_checks[DVM_VEHICLE_TYPE_COUNT];
    HWND type_restore_buttons[DVM_VEHICLE_TYPE_COUNT];
    HWND status_label;
    HFONT ui_font;
    UINT dpi;
    wchar_t base_directory[DVM_PATH_CAP];
    wchar_t state_path[DVM_PATH_CAP];
    SpawnDataset datasets[DVM_MAP_COUNT];
    MapView views[DVM_MAP_COUNT];
    int active_map;
    GpImage *marker_image;
    GpBitmap *marker_display_image;
    GpBitmap *marker_green_display_image;
    GpBitmap *marker_red_display_image;
    UINT marker_width;
    UINT marker_height;
    int marker_display_size;
    BOOL mouse_down;
    BOOL dragging;
    BOOL zooming;
    BOOL mouse_tracking;
    POINT drag_start;
    POINT drag_last;
    BOOL cursor_world_valid;
    BOOL status_update_pending;
    double cursor_world_x;
    double cursor_world_z;
    HDC viewport_buffer_dc;
    HBITMAP viewport_buffer_bitmap;
    HGDIOBJ viewport_buffer_old_bitmap;
    int viewport_buffer_width;
    int viewport_buffer_height;
    BOOL viewport_buffer_valid;
    BOOL viewport_dirty;
    BOOL viewport_frame_pending;
    LARGE_INTEGER viewport_clock_frequency;
    LARGE_INTEGER viewport_frame_finished;
} App;

static LRESULT CALLBACK main_window_proc(HWND window, UINT message,
                                         WPARAM w_param, LPARAM l_param);
static LRESULT CALLBACK viewport_proc(HWND window, UINT message,
                                      WPARAM w_param, LPARAM l_param);

static int scaled(const App *app, int value)
{
    return MulDiv(value, (int)(app && app->dpi ? app->dpi : 96u), 96);
}

static void enable_dpi_awareness(void)
{
    if (SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return;
    SetProcessDPIAware();
}

static BOOL is_self_test_command(const wchar_t *command_line)
{
    static const wchar_t option[] = L"--self-test";
    size_t option_length = (sizeof(option) / sizeof(option[0])) - 1u;
    if (!command_line) return FALSE;
    while (iswspace((wint_t)*command_line)) ++command_line;
    if (_wcsnicmp(command_line, option, option_length) != 0) return FALSE;
    command_line += option_length;
    while (iswspace((wint_t)*command_line)) ++command_line;
    return *command_line == L'\0';
}

static void set_control_font(HWND control, HFONT font)
{
    if (control && font) SendMessageW(control, WM_SETFONT, (WPARAM)font, TRUE);
}

static void recreate_ui_font(App *app)
{
    NONCLIENTMETRICSW metrics;
    HFONT replacement = NULL;
    if (!app) return;
    ZeroMemory(&metrics, sizeof(metrics));
    metrics.cbSize = sizeof(metrics);
    if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics),
                                   &metrics, 0, app->dpi)) {
        replacement = CreateFontIndirectW(&metrics.lfMessageFont);
    }
    if (!replacement) {
        replacement = CreateFontW(-MulDiv(9, (int)app->dpi, 72), 0, 0, 0,
                                  FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                                  L"Segoe UI");
    }
    if (replacement) {
        HFONT old = app->ui_font;
        int type_index;
        app->ui_font = replacement;
        set_control_font(GetDlgItem(app->main_window, ID_MAP_LABEL), replacement);
        set_control_font(app->map_combo, replacement);
        set_control_font(app->restore_all_button, replacement);
        set_control_font(app->fit_button, replacement);
        set_control_font(GetDlgItem(app->main_window, ID_TYPE_LABEL), replacement);
        set_control_font(app->status_label, replacement);
        for (type_index = 0; type_index < DVM_VEHICLE_TYPE_COUNT; ++type_index) {
            set_control_font(app->type_checks[type_index], replacement);
            set_control_font(app->type_restore_buttons[type_index], replacement);
        }
        if (old) DeleteObject(old);
    }
}

static BOOL load_marker_image(App *app, wchar_t *error, size_t error_capacity)
{
    wchar_t path[DVM_PATH_CAP];
    if (!dvm_join_path(path, DVM_PATH_CAP, app->base_directory, L"data\\marker.png")) {
        dvm_set_error(error, error_capacity, L"Marker image path is too long.");
        return FALSE;
    }
    if (GdipLoadImageFromFile(path, &app->marker_image) != Ok || !app->marker_image) {
        dvm_set_error(error, error_capacity,
                      L"The required screenshot-derived yellow marker is unavailable.\n\n"
                      L"Expected runtime asset:\n%ls\n\n"
                      L"The map remains usable for viewing, filtering, zooming, and panning, "
                      L"but marker drawing/checking is disabled.", path);
        return FALSE;
    }
    if (GdipGetImageWidth(app->marker_image, &app->marker_width) != Ok ||
        GdipGetImageHeight(app->marker_image, &app->marker_height) != Ok ||
        app->marker_width < 4u || app->marker_height < 4u ||
        app->marker_width > 512u || app->marker_height > 512u) {
        GdipDisposeImage(app->marker_image);
        app->marker_image = NULL;
        dvm_set_error(error, error_capacity, L"The yellow marker image is invalid:\n%ls", path);
        return FALSE;
    }
    return TRUE;
}

static int marker_screen_size(const App *app)
{
    return scaled(app, 26);
}

static void destroy_marker_display_image(App *app)
{
    if (!app) return;
    if (app->marker_display_image)
        GdipDisposeImage((GpImage *)app->marker_display_image);
    if (app->marker_green_display_image)
        GdipDisposeImage((GpImage *)app->marker_green_display_image);
    if (app->marker_red_display_image)
        GdipDisposeImage((GpImage *)app->marker_red_display_image);
    app->marker_display_image = NULL;
    app->marker_green_display_image = NULL;
    app->marker_red_display_image = NULL;
    app->marker_display_size = 0;
}

static GpImage *get_marker_display_image(App *app, int size)
{
    GpBitmap *bitmap = NULL;
    GpGraphics *graphics = NULL;
    GpStatus status;
    if (!app || !app->marker_image || size <= 0) return NULL;
    if (app->marker_display_image && app->marker_display_size == size)
        return (GpImage *)app->marker_display_image;
    status = GdipCreateBitmapFromScan0(size, size, 0, PixelFormat32bppPARGB,
                                       NULL, &bitmap);
    if (status == Ok && bitmap &&
        GdipGetImageGraphicsContext(bitmap, &graphics) == Ok) {
        GdipSetCompositingMode(graphics, CompositingModeSourceCopy);
        GdipSetCompositingQuality(graphics, CompositingQualityHighQuality);
        GdipSetInterpolationMode(graphics, InterpolationModeHighQualityBicubic);
        GdipSetPixelOffsetMode(graphics, PixelOffsetModeHalf);
        status = GdipDrawImageRectI(graphics, app->marker_image, 0, 0, size, size);
        GdipDeleteGraphics(graphics);
        graphics = NULL;
        if (status == Ok) {
            destroy_marker_display_image(app);
            app->marker_display_image = bitmap;
            app->marker_display_size = size;
            return (GpImage *)bitmap;
        }
    }
    if (graphics) GdipDeleteGraphics(graphics);
    if (bitmap) GdipDisposeImage((GpImage *)bitmap);
    return app->marker_image;
}

static BYTE unpremultiply_channel(BYTE channel, BYTE alpha)
{
    if (alpha == 0u) return 0u;
    return (BYTE)((channel * 255u + alpha / 2u) / alpha > 255u
        ? 255u : (channel * 255u + alpha / 2u) / alpha);
}

static BYTE premultiply_channel(BYTE channel, BYTE alpha)
{
    return (BYTE)((channel * alpha + 127u) / 255u);
}

static BOOL recolor_marker_bitmap(GpBitmap *bitmap, int size,
                                  MarkerColorState color_state)
{
    GpRect rectangle;
    BitmapData locked;
    int y;
    if (!bitmap || size <= 0 ||
        (color_state != MARKER_COLOR_GREEN && color_state != MARKER_COLOR_RED))
        return FALSE;
    rectangle.X = 0;
    rectangle.Y = 0;
    rectangle.Width = size;
    rectangle.Height = size;
    ZeroMemory(&locked, sizeof(locked));
    if (GdipBitmapLockBits(bitmap, &rectangle, ImageLockModeRead | ImageLockModeWrite,
                           PixelFormat32bppPARGB, &locked) != Ok) {
        return FALSE;
    }
    for (y = 0; y < size; ++y) {
        BYTE *row = (BYTE *)locked.Scan0 + (ptrdiff_t)y * locked.Stride;
        int x;
        for (x = 0; x < size; ++x) {
            BYTE *pixel = row + x * 4;
            BYTE alpha = pixel[3];
            BYTE blue;
            BYTE green;
            BYTE red;
            BYTE maximum;
            BYTE minimum;
            if (alpha == 0u) continue;
            blue = unpremultiply_channel(pixel[0], alpha);
            green = unpremultiply_channel(pixel[1], alpha);
            red = unpremultiply_channel(pixel[2], alpha);
            maximum = red > green ? red : green;
            if (blue > maximum) maximum = blue;
            minimum = red < green ? red : green;
            if (blue < minimum) minimum = blue;
            if (maximum == 0u ||
                (unsigned int)(maximum - minimum) * 100u <
                    (unsigned int)maximum * 20u) continue;
            if (color_state == MARKER_COLOR_GREEN) {
                red = minimum;
                green = maximum;
                blue = minimum;
            } else {
                red = maximum;
                green = minimum;
                blue = minimum;
            }
            pixel[0] = premultiply_channel(blue, alpha);
            pixel[1] = premultiply_channel(green, alpha);
            pixel[2] = premultiply_channel(red, alpha);
        }
    }
    if (GdipBitmapUnlockBits(bitmap, &locked) != Ok) {
        return FALSE;
    }
    return TRUE;
}

static GpImage *get_colored_marker_display_image(App *app, int size,
                                                  MarkerColorState color_state)
{
    GpBitmap **cache;
    GpBitmap *bitmap = NULL;
    GpGraphics *graphics = NULL;
    GpStatus status;
    GpImage *normal = get_marker_display_image(app, size);
    if (!normal || color_state == MARKER_COLOR_NORMAL) return normal;
    if (color_state == MARKER_COLOR_GREEN) cache = &app->marker_green_display_image;
    else if (color_state == MARKER_COLOR_RED) cache = &app->marker_red_display_image;
    else return normal;
    if (*cache) return (GpImage *)*cache;
    status = GdipCreateBitmapFromScan0(size, size, 0, PixelFormat32bppPARGB,
                                       NULL, &bitmap);
    if (status == Ok && bitmap &&
        GdipGetImageGraphicsContext(bitmap, &graphics) == Ok) {
        GdipSetCompositingMode(graphics, CompositingModeSourceCopy);
        GdipSetCompositingQuality(graphics, CompositingQualityHighQuality);
        GdipSetInterpolationMode(graphics, InterpolationModeHighQualityBicubic);
        GdipSetPixelOffsetMode(graphics, PixelOffsetModeHalf);
        status = GdipDrawImageRectI(graphics, app->marker_image, 0, 0, size, size);
        GdipDeleteGraphics(graphics);
        graphics = NULL;
        if (status == Ok && recolor_marker_bitmap(bitmap, size, color_state)) {
            *cache = bitmap;
            return (GpImage *)bitmap;
        }
    }
    if (graphics) GdipDeleteGraphics(graphics);
    if (bitmap) GdipDisposeImage((GpImage *)bitmap);
    return normal;
}

static size_t find_marker_at(const App *app, int x, int y, int marker_size)
{
    const SpawnDataset *dataset;
    const MapView *view;
    size_t index;
    double radius;
    if (!app || marker_size <= 0 || app->active_map < 0 ||
        app->active_map >= DVM_MAP_COUNT) return SIZE_MAX;
    dataset = &app->datasets[app->active_map];
    view = &app->views[app->active_map];
    radius = (double)marker_size * 0.5;
    for (index = dataset->count; index > 0; --index) {
        const VehicleSpawn *spawn = &dataset->items[index - 1];
        double screen_x;
        double screen_y;
        double delta_x;
        double delta_y;
        if (spawn->hidden || spawn->type_index < 0 ||
            spawn->type_index >= DVM_VEHICLE_TYPE_COUNT ||
            !dataset->type_visible[spawn->type_index] ||
            !dvm_map_world_to_screen(view, spawn->x, spawn->z,
                                     &screen_x, &screen_y)) continue;
        delta_x = screen_x - (double)x;
        delta_y = screen_y - (double)y;
        if (delta_x * delta_x + delta_y * delta_y <= radius * radius) return index - 1;
    }
    return SIZE_MAX;
}

static void draw_markers(App *app, HDC dc, const RECT *client)
{
    const SpawnDataset *dataset;
    const MapView *view;
    GpGraphics *graphics = NULL;
    size_t index;
    int size;
    if (!app || !app->marker_image || !dc || !client) return;
    dataset = &app->datasets[app->active_map];
    view = &app->views[app->active_map];
    size = marker_screen_size(app);
    if (!get_marker_display_image(app, size)) return;
    if (GdipCreateFromHDC(dc, &graphics) != Ok) return;
    GdipSetCompositingMode(graphics, CompositingModeSourceOver);
    GdipSetCompositingQuality(graphics, CompositingQualityHighSpeed);
    GdipSetInterpolationMode(graphics, InterpolationModeNearestNeighbor);
    GdipSetPixelOffsetMode(graphics, PixelOffsetModeHalf);
    for (index = 0; index < dataset->count; ++index) {
        const VehicleSpawn *spawn = &dataset->items[index];
        double screen_x;
        double screen_y;
        int left;
        int top;
        GpImage *marker;
        if (spawn->hidden || spawn->type_index < 0 ||
            spawn->type_index >= DVM_VEHICLE_TYPE_COUNT ||
            !dataset->type_visible[spawn->type_index] ||
            !dvm_map_world_to_screen(view, spawn->x, spawn->z,
                                     &screen_x, &screen_y)) continue;
        left = (int)lround(screen_x) - size / 2;
        top = (int)lround(screen_y) - size / 2;
        if (left >= client->right || top >= client->bottom ||
            left + size <= client->left || top + size <= client->top) continue;
        marker = get_colored_marker_display_image(app, size, spawn->color_state);
        if (!marker) continue;
        GdipDrawImageRectI(graphics, marker, left, top, size, size);
    }
    GdipDeleteGraphics(graphics);
}

static void destroy_viewport_buffer(App *app)
{
    if (!app) return;
    if (app->viewport_buffer_dc && app->viewport_buffer_old_bitmap)
        SelectObject(app->viewport_buffer_dc, app->viewport_buffer_old_bitmap);
    if (app->viewport_buffer_bitmap) DeleteObject(app->viewport_buffer_bitmap);
    if (app->viewport_buffer_dc) DeleteDC(app->viewport_buffer_dc);
    app->viewport_buffer_dc = NULL;
    app->viewport_buffer_bitmap = NULL;
    app->viewport_buffer_old_bitmap = NULL;
    app->viewport_buffer_width = 0;
    app->viewport_buffer_height = 0;
    app->viewport_buffer_valid = FALSE;
}

static UINT viewport_frame_delay(const App *app)
{
    LARGE_INTEGER now;
    double remaining_ms;
    if (!app->viewport_frame_finished.QuadPart) return 0u;
    QueryPerformanceCounter(&now);
    remaining_ms = VIEWPORT_FRAME_INTERVAL_MS -
        (double)(now.QuadPart - app->viewport_frame_finished.QuadPart) * 1000.0 /
        (double)app->viewport_clock_frequency.QuadPart;
    return remaining_ms > 0.0 ? (UINT)ceil(remaining_ms) : 0u;
}

static void cancel_viewport_frame(App *app)
{
    if (app->viewport_frame_pending)
        KillTimer(app->viewport, ID_VIEWPORT_FRAME_TIMER);
    app->viewport_frame_pending = FALSE;
}

static void schedule_viewport_frame(App *app)
{
    UINT delay;
    if (!app || !app->viewport) return;
    if (!IsWindowVisible(app->viewport) || IsIconic(app->main_window)) {
        cancel_viewport_frame(app);
        return;
    }
    if (!app->viewport_dirty || app->viewport_frame_pending) return;
    delay = viewport_frame_delay(app);
    if (delay == 0u) {
        InvalidateRect(app->viewport, NULL, FALSE);
    } else {
        app->viewport_frame_pending = SetTimer(app->viewport,
            ID_VIEWPORT_FRAME_TIMER, delay, NULL) != 0;
    }
}

static void request_viewport_frame(App *app)
{
    if (!app) return;
    app->viewport_dirty = TRUE;
    schedule_viewport_frame(app);
}

static BOOL ensure_viewport_buffer(App *app, HDC target, int width, int height)
{
    HDC new_dc;
    HBITMAP new_bitmap;
    HGDIOBJ old_bitmap;
    if (!app || !target || width <= 0 || height <= 0) return FALSE;
    if (app->viewport_buffer_dc && app->viewport_buffer_bitmap &&
        app->viewport_buffer_width == width &&
        app->viewport_buffer_height == height) return TRUE;
    new_dc = CreateCompatibleDC(target);
    if (!new_dc) return FALSE;
    new_bitmap = CreateCompatibleBitmap(target, width, height);
    if (!new_bitmap) {
        DeleteDC(new_dc);
        return FALSE;
    }
    old_bitmap = SelectObject(new_dc, new_bitmap);
    if (!old_bitmap || old_bitmap == HGDI_ERROR) {
        DeleteObject(new_bitmap);
        DeleteDC(new_dc);
        return FALSE;
    }
    destroy_viewport_buffer(app);
    app->viewport_buffer_dc = new_dc;
    app->viewport_buffer_bitmap = new_bitmap;
    app->viewport_buffer_old_bitmap = old_bitmap;
    app->viewport_buffer_width = width;
    app->viewport_buffer_height = height;
    return TRUE;
}

static void draw_viewport_content(App *app, HDC target, const RECT *client)
{
    dvm_map_view_draw(&app->views[app->active_map], target,
                      client, app->dragging || app->zooming);
    draw_markers(app, target, client);
    if (!app->marker_image) {
        RECT notice = *client;
        HBRUSH notice_brush = CreateSolidBrush(RGB(74, 60, 18));
        HGDIOBJ old_font = NULL;
        notice.top += scaled(app, 14);
        notice.left += scaled(app, 14);
        notice.right = notice.left + scaled(app, 430);
        notice.bottom = notice.top + scaled(app, 58);
        if (notice_brush) {
            FillRect(target, &notice, notice_brush);
            DeleteObject(notice_brush);
        }
        SetBkMode(target, TRANSPARENT);
        SetTextColor(target, RGB(255, 226, 116));
        if (app->ui_font)
            old_font = SelectObject(target, app->ui_font);
        InflateRect(&notice, -scaled(app, 8), -scaled(app, 6));
        DrawTextW(target,
                  L"Required screenshot-derived marker asset is missing.\n"
                  L"Marker checking is disabled; map controls remain available.",
                  -1, &notice, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
        if (old_font) SelectObject(target, old_font);
    }
}

static void paint_viewport(App *app, HWND window)
{
    PAINTSTRUCT paint;
    RECT client;
    HDC target = BeginPaint(window, &paint);
    BOOL drew_directly = FALSE;
    GetClientRect(window, &client);
    if (!app->viewport_buffer_valid ||
        app->viewport_buffer_width != client.right ||
        app->viewport_buffer_height != client.bottom)
        app->viewport_dirty = TRUE;

    /* Every paint path observes the cap, including forced paints and resizing.
       Keep the last frame until the latest input can be rendered together. */
    if (app->viewport_dirty && client.right > 0 && client.bottom > 0 &&
        IsWindowVisible(window) && !IsIconic(app->main_window) &&
        viewport_frame_delay(app) == 0u) {
        cancel_viewport_frame(app);
        if (ensure_viewport_buffer(app, target, client.right, client.bottom)) {
            draw_viewport_content(app, app->viewport_buffer_dc, &client);
            app->viewport_buffer_valid = TRUE;
        } else {
            draw_viewport_content(app, target, &client);
            drew_directly = TRUE;
        }
        app->viewport_dirty = FALSE;
        /* Leave CPU time between frames even if a frame itself is expensive. */
        QueryPerformanceCounter(&app->viewport_frame_finished);
    }
    if (!drew_directly) {
        if (!app->viewport_buffer_valid ||
            app->viewport_buffer_width < client.right ||
            app->viewport_buffer_height < client.bottom)
            FillRect(target, &client, (HBRUSH)GetStockObject(BLACK_BRUSH));
        if (app->viewport_buffer_valid)
            BitBlt(target, 0, 0,
                   min(client.right, app->viewport_buffer_width),
                   min(client.bottom, app->viewport_buffer_height),
                   app->viewport_buffer_dc, 0, 0, SRCCOPY);
    }
    EndPaint(window, &paint);
    schedule_viewport_frame(app);
}

static void update_status_label(App *app)
{
    const SpawnDataset *dataset;
    const MapInfo *map;
    const MapView *view;
    int visible_types = 0;
    int available_types = 0;
    int type_index;
    wchar_t text[512];
    if (!app || !app->main_window) return;
    dataset = &app->datasets[app->active_map];
    map = dvm_map_info(app->active_map);
    view = &app->views[app->active_map];
    for (type_index = 0; type_index < DVM_VEHICLE_TYPE_COUNT; ++type_index) {
        if (dataset->type_counts[type_index] != 0) {
            ++available_types;
            if (dataset->type_visible[type_index]) ++visible_types;
        }
    }
    _snwprintf(text, sizeof(text) / sizeof(text[0]),
               L"%ls\r\nRemaining: %llu of %llu\r\nVisible types: %d of %d\r\nZoom: %.0f%%%ls",
               map ? map->display_name : L"Map",
               (unsigned long long)dvm_remaining_total(dataset),
               (unsigned long long)dataset->count,
               visible_types, available_types,
               view->fit_zoom > 0.0 ? view->zoom / view->fit_zoom * 100.0 : 100.0,
               app->cursor_world_valid ? L"\r\nCursor: " : L"");
    text[(sizeof(text) / sizeof(text[0])) - 1] = L'\0';
    if (app->cursor_world_valid) {
        size_t used = wcslen(text);
        _snwprintf(text + used, (sizeof(text) / sizeof(text[0])) - used,
                   L"X %.0f, Z %.0f", app->cursor_world_x, app->cursor_world_z);
        text[(sizeof(text) / sizeof(text[0])) - 1] = L'\0';
    }
    if (!app->marker_image) {
        size_t used = wcslen(text);
        _snwprintf(text + used, (sizeof(text) / sizeof(text[0])) - used,
                   L"\r\n\r\nYellow marker asset missing");
        text[(sizeof(text) / sizeof(text[0])) - 1] = L'\0';
    }
    SetWindowTextW(app->status_label, text);
}

static void schedule_status_update(App *app)
{
    if (!app || !app->main_window || app->status_update_pending) return;
    if (SetTimer(app->main_window, ID_STATUS_UPDATE_TIMER,
                 STATUS_UPDATE_INTERVAL_MS, NULL) != 0)
        app->status_update_pending = TRUE;
}

static void update_controls(App *app)
{
    SpawnDataset *dataset;
    int type_index;
    wchar_t text[512];
    if (!app || !app->main_window) return;
    if (app->status_update_pending) {
        KillTimer(app->main_window, ID_STATUS_UPDATE_TIMER);
        app->status_update_pending = FALSE;
    }
    dataset = &app->datasets[app->active_map];
    SendMessageW(app->map_combo, CB_SETCURSEL, (WPARAM)app->active_map, 0);
    for (type_index = 0; type_index < DVM_VEHICLE_TYPE_COUNT; ++type_index) {
        const VehicleTypeInfo *type = dvm_vehicle_type_info(type_index);
        size_t type_remaining = dvm_remaining_for_type(dataset, type_index);
        size_t type_total = dataset->type_counts[type_index];
        BOOL available = type_total != 0;
        _snwprintf(text, sizeof(text) / sizeof(text[0]), L"%ls  %llu/%llu",
                   type ? type->friendly_name : L"Unknown",
                   (unsigned long long)type_remaining,
                   (unsigned long long)type_total);
        text[(sizeof(text) / sizeof(text[0])) - 1] = L'\0';
        SetWindowTextW(app->type_checks[type_index], text);
        SendMessageW(app->type_checks[type_index], BM_SETCHECK,
                     dataset->type_visible[type_index] ? BST_CHECKED : BST_UNCHECKED, 0);
        ShowWindow(app->type_checks[type_index], available ? SW_SHOW : SW_HIDE);
        ShowWindow(app->type_restore_buttons[type_index], available ? SW_SHOW : SW_HIDE);
        EnableWindow(app->type_restore_buttons[type_index],
                     dvm_type_has_restorable_state(dataset, type_index));
    }
    EnableWindow(app->restore_all_button, dvm_has_restorable_state(dataset));
    update_status_label(app);
}

static void layout_controls(App *app, int client_width, int client_height)
{
    int panel_width = scaled(app, 274);
    int margin = scaled(app, 10);
    int content_width = panel_width - margin * 2;
    int button_gap = scaled(app, 6);
    int half_width = (content_width - button_gap) / 2;
    int y = scaled(app, 10);
    int type_index;
    if (!app) return;
    if (panel_width > client_width - scaled(app, 180))
        panel_width = client_width - scaled(app, 180);
    if (panel_width < scaled(app, 220)) panel_width = scaled(app, 220);
    content_width = panel_width - margin * 2;
    half_width = (content_width - button_gap) / 2;
    MoveWindow(GetDlgItem(app->main_window, ID_MAP_LABEL), margin, y,
               content_width, scaled(app, 18), TRUE);
    y += scaled(app, 20);
    MoveWindow(app->map_combo, margin, y, content_width, scaled(app, 220), TRUE);
    y += scaled(app, 36);
    MoveWindow(app->restore_all_button, margin, y, half_width, scaled(app, 30), TRUE);
    MoveWindow(app->fit_button, margin + half_width + button_gap, y,
               half_width, scaled(app, 30), TRUE);
    y += scaled(app, 42);
    MoveWindow(GetDlgItem(app->main_window, ID_TYPE_LABEL), margin, y,
               content_width, scaled(app, 20), TRUE);
    y += scaled(app, 24);
    for (type_index = 0; type_index < DVM_VEHICLE_TYPE_COUNT; ++type_index) {
        int restore_width = scaled(app, 64);
        MoveWindow(app->type_checks[type_index], margin, y,
                   content_width - restore_width - button_gap, scaled(app, 27), TRUE);
        MoveWindow(app->type_restore_buttons[type_index],
                   margin + content_width - restore_width, y,
                   restore_width, scaled(app, 27), TRUE);
        y += scaled(app, 35);
    }
    y += scaled(app, 8);
    MoveWindow(app->status_label, margin, y, content_width,
               client_height - y - margin > scaled(app, 80)
                   ? client_height - y - margin : scaled(app, 80), TRUE);
    MoveWindow(app->viewport, panel_width, 0,
               client_width - panel_width > 0 ? client_width - panel_width : 1,
               client_height > 0 ? client_height : 1, TRUE);
}

static BOOL create_controls(App *app)
{
    int type_index;
    app->map_combo = CreateWindowExW(0, WC_COMBOBOXW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        0, 0, 0, 0, app->main_window, (HMENU)(INT_PTR)ID_MAP_COMBO,
        app->instance, NULL);
    app->restore_all_button = CreateWindowExW(0, WC_BUTTONW, L"Restore All",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, app->main_window, (HMENU)(INT_PTR)ID_RESTORE_ALL,
        app->instance, NULL);
    app->fit_button = CreateWindowExW(0, WC_BUTTONW, L"Fit Map",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 0, 0, app->main_window, (HMENU)(INT_PTR)ID_FIT_MAP,
        app->instance, NULL);
    CreateWindowExW(0, WC_STATICW, L"Map", WS_CHILD | WS_VISIBLE,
        0, 0, 0, 0, app->main_window, (HMENU)(INT_PTR)ID_MAP_LABEL,
        app->instance, NULL);
    CreateWindowExW(0, WC_STATICW, L"Vehicle types (remaining / total)",
        WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, app->main_window,
        (HMENU)(INT_PTR)ID_TYPE_LABEL, app->instance, NULL);
    app->status_label = CreateWindowExW(0, WC_STATICW, L"",
        WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, app->main_window,
        (HMENU)(INT_PTR)ID_STATUS, app->instance, NULL);
    for (type_index = 0; type_index < DVM_VEHICLE_TYPE_COUNT; ++type_index) {
        app->type_checks[type_index] = CreateWindowExW(0, WC_BUTTONW, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            0, 0, 0, 0, app->main_window,
            (HMENU)(INT_PTR)(ID_TYPE_CHECK_BASE + type_index), app->instance, NULL);
        app->type_restore_buttons[type_index] = CreateWindowExW(0, WC_BUTTONW, L"Restore",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0, 0, 0, 0, app->main_window,
            (HMENU)(INT_PTR)(ID_TYPE_RESTORE_BASE + type_index), app->instance, NULL);
    }
    app->viewport = CreateWindowExW(0, VIEW_CLASS_NAME, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPCHILDREN,
        0, 0, 0, 0, app->main_window, (HMENU)(INT_PTR)ID_VIEWPORT,
        app->instance, app);
    if (!app->map_combo || !app->restore_all_button || !app->fit_button ||
        !app->status_label || !app->viewport) return FALSE;
    for (type_index = 0; type_index < DVM_VEHICLE_TYPE_COUNT; ++type_index)
        if (!app->type_checks[type_index] || !app->type_restore_buttons[type_index]) return FALSE;
    for (type_index = 0; type_index < DVM_MAP_COUNT; ++type_index) {
        const MapInfo *map = dvm_map_info(type_index);
        SendMessageW(app->map_combo, CB_ADDSTRING, 0, (LPARAM)map->display_name);
    }
    recreate_ui_font(app);
    return TRUE;
}

static void save_state_or_warn(App *app)
{
    wchar_t error[512];
    if (!dvm_state_save(app->state_path, app->datasets, error,
                        sizeof(error) / sizeof(error[0]))) {
        MessageBoxW(app->main_window, error, L"Could not save progress",
                    MB_OK | MB_ICONWARNING);
    }
}

static void switch_map(App *app, int map_index)
{
    if (!app || map_index < 0 || map_index >= DVM_MAP_COUNT ||
        map_index == app->active_map) return;
    app->active_map = map_index;
    app->cursor_world_valid = FALSE;
    update_controls(app);
    request_viewport_frame(app);
}

static void handle_command(App *app, int control_id, int notification)
{
    int type_index;
    if (!app) return;
    if (control_id == ID_MAP_COMBO && notification == CBN_SELCHANGE) {
        int selection = (int)SendMessageW(app->map_combo, CB_GETCURSEL, 0, 0);
        switch_map(app, selection);
        return;
    }
    if (control_id == ID_RESTORE_ALL && notification == BN_CLICKED) {
        dvm_restore_all(&app->datasets[app->active_map]);
        save_state_or_warn(app);
        update_controls(app);
        request_viewport_frame(app);
        return;
    }
    if (control_id == ID_FIT_MAP && notification == BN_CLICKED) {
        dvm_map_view_reset(&app->views[app->active_map]);
        update_controls(app);
        request_viewport_frame(app);
        return;
    }
    type_index = control_id - ID_TYPE_CHECK_BASE;
    if (type_index >= 0 && type_index < DVM_VEHICLE_TYPE_COUNT &&
        notification == BN_CLICKED) {
        app->datasets[app->active_map].type_visible[type_index] =
            SendMessageW(app->type_checks[type_index], BM_GETCHECK, 0, 0) == BST_CHECKED;
        update_controls(app);
        request_viewport_frame(app);
        return;
    }
    type_index = control_id - ID_TYPE_RESTORE_BASE;
    if (type_index >= 0 && type_index < DVM_VEHICLE_TYPE_COUNT &&
        notification == BN_CLICKED) {
        dvm_restore_type(&app->datasets[app->active_map], type_index);
        save_state_or_warn(app);
        update_controls(app);
        request_viewport_frame(app);
    }
}

static LRESULT CALLBACK main_window_proc(HWND window, UINT message,
                                         WPARAM w_param, LPARAM l_param)
{
    App *app = (App *)GetWindowLongPtrW(window, GWLP_USERDATA);
    switch (message) {
    case WM_NCCREATE:
        app = (App *)((CREATESTRUCTW *)l_param)->lpCreateParams;
        SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)app);
        if (app) app->main_window = window;
        return DefWindowProcW(window, message, w_param, l_param);
    case WM_CREATE:
        if (!app) return -1;
        app->dpi = GetDpiForWindow(window);
        if (!create_controls(app)) return -1;
        update_controls(app);
        return 0;
    case WM_SIZE:
        if (app) {
            if (w_param == SIZE_MINIMIZED) {
                cancel_viewport_frame(app);
                KillTimer(app->viewport, ID_ZOOM_QUALITY_TIMER);
                app->zooming = FALSE;
            } else {
                layout_controls(app, LOWORD(l_param), HIWORD(l_param));
                request_viewport_frame(app);
            }
        }
        return 0;
    case WM_SHOWWINDOW:
        if (app && app->viewport) {
            if (w_param) request_viewport_frame(app);
            else cancel_viewport_frame(app);
        }
        return 0;
    case WM_COMMAND:
        if (app) handle_command(app, LOWORD(w_param), HIWORD(w_param));
        return 0;
    case WM_TIMER:
        if (app && w_param == ID_STATUS_UPDATE_TIMER) {
            KillTimer(window, ID_STATUS_UPDATE_TIMER);
            app->status_update_pending = FALSE;
            update_status_label(app);
            return 0;
        }
        break;
    case WM_DPICHANGED:
        if (app) {
            RECT *suggested = (RECT *)l_param;
            app->dpi = HIWORD(w_param);
            SetWindowPos(window, NULL, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            recreate_ui_font(app);
            {
                RECT client;
                GetClientRect(window, &client);
                layout_controls(app, client.right, client.bottom);
            }
            request_viewport_frame(app);
        }
        return 0;
    case WM_GETMINMAXINFO:
        if (app) {
            MINMAXINFO *limits = (MINMAXINFO *)l_param;
            limits->ptMinTrackSize.x = scaled(app, 760);
            limits->ptMinTrackSize.y = scaled(app, 590);
        }
        return 0;
    case WM_DESTROY:
        if (app) {
            wchar_t ignored[2];
            if (app->status_update_pending)
                KillTimer(window, ID_STATUS_UPDATE_TIMER);
            app->status_update_pending = FALSE;
            dvm_state_save(app->state_path, app->datasets, ignored,
                           sizeof(ignored) / sizeof(ignored[0]));
            destroy_viewport_buffer(app);
            if (app->ui_font) {
                DeleteObject(app->ui_font);
                app->ui_font = NULL;
            }
        }
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, w_param, l_param);
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

static void update_cursor_world(App *app, int x, int y)
{
    BOOL valid = dvm_map_screen_to_world(&app->views[app->active_map],
                                         (double)x, (double)y,
                                         &app->cursor_world_x,
                                         &app->cursor_world_z);
    if (valid != app->cursor_world_valid || valid) {
        app->cursor_world_valid = valid;
        schedule_status_update(app);
    }
}

static void select_marker_color(VehicleSpawn *spawn,
                                MarkerColorState selected_color)
{
    if (!spawn) return;
    spawn->color_state = spawn->color_state == selected_color
        ? MARKER_COLOR_NORMAL : selected_color;
}

static void change_marker_color_at(App *app, int x, int y,
                                   MarkerColorState selected_color)
{
    size_t hit;
    VehicleSpawn *spawn;
    if (!app || !app->marker_image) return;
    hit = find_marker_at(app, x, y, marker_screen_size(app));
    if (hit == SIZE_MAX) return;
    spawn = &app->datasets[app->active_map].items[hit];
    select_marker_color(spawn, selected_color);
    save_state_or_warn(app);
    update_controls(app);
    request_viewport_frame(app);
}

static LRESULT CALLBACK viewport_proc(HWND window, UINT message,
                                      WPARAM w_param, LPARAM l_param)
{
    App *app = (App *)GetWindowLongPtrW(window, GWLP_USERDATA);
    switch (message) {
    case WM_NCCREATE:
        app = (App *)((CREATESTRUCTW *)l_param)->lpCreateParams;
        SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)app);
        if (app) {
            app->viewport = window;
            app->viewport_dirty = TRUE;
            QueryPerformanceFrequency(&app->viewport_clock_frequency);
        }
        return DefWindowProcW(window, message, w_param, l_param);
    case WM_SIZE:
        if (app) {
            int map_index;
            for (map_index = 0; map_index < DVM_MAP_COUNT; ++map_index)
                dvm_map_view_resize(&app->views[map_index],
                                    LOWORD(l_param), HIWORD(l_param));
            schedule_status_update(app);
            request_viewport_frame(app);
        }
        return 0;
    case WM_SHOWWINDOW:
        if (app) {
            if (w_param) request_viewport_frame(app);
            else cancel_viewport_frame(app);
        }
        break;
    case WM_PAINT:
        if (app) paint_viewport(app, window);
        else {
            PAINTSTRUCT paint;
            BeginPaint(window, &paint);
            EndPaint(window, &paint);
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEWHEEL:
        if (app) {
            POINT point = { GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param) };
            double steps = (double)GET_WHEEL_DELTA_WPARAM(w_param) / (double)WHEEL_DELTA;
            ScreenToClient(window, &point);
            dvm_map_view_zoom_at(&app->views[app->active_map], pow(1.22, steps),
                                 (double)point.x, (double)point.y);
            app->zooming = TRUE;
            if (!SetTimer(window, ID_ZOOM_QUALITY_TIMER, ZOOM_SETTLE_INTERVAL_MS, NULL))
                app->zooming = FALSE;
            update_cursor_world(app, point.x, point.y);
            request_viewport_frame(app);
        }
        return 0;
    case WM_TIMER:
        if (app && w_param == ID_VIEWPORT_FRAME_TIMER) {
            cancel_viewport_frame(app);
            schedule_viewport_frame(app);
            return 0;
        }
        if (app && w_param == ID_ZOOM_QUALITY_TIMER) {
            KillTimer(window, ID_ZOOM_QUALITY_TIMER);
            app->zooming = FALSE;
            request_viewport_frame(app);
            return 0;
        }
        break;
    case WM_LBUTTONDOWN:
        if (app) {
            SetFocus(window);
            SetCapture(window);
            app->mouse_down = TRUE;
            app->dragging = FALSE;
            app->drag_start.x = GET_X_LPARAM(l_param);
            app->drag_start.y = GET_Y_LPARAM(l_param);
            app->drag_last = app->drag_start;
        }
        return 0;
    case WM_MOUSEMOVE:
        if (app) {
            int x = GET_X_LPARAM(l_param);
            int y = GET_Y_LPARAM(l_param);
            if (!app->mouse_tracking) {
                TRACKMOUSEEVENT tracking;
                ZeroMemory(&tracking, sizeof(tracking));
                tracking.cbSize = sizeof(tracking);
                tracking.dwFlags = TME_LEAVE;
                tracking.hwndTrack = window;
                if (TrackMouseEvent(&tracking)) app->mouse_tracking = TRUE;
            }
            update_cursor_world(app, x, y);
            if (app->mouse_down && GetCapture() == window) {
                int total_x = x - app->drag_start.x;
                int total_y = y - app->drag_start.y;
                if (!app->dragging && total_x * total_x + total_y * total_y >
                                      scaled(app, 4) * scaled(app, 4))
                    app->dragging = TRUE;
                if (app->dragging) {
                    dvm_map_view_pan(&app->views[app->active_map],
                                     (double)(x - app->drag_last.x),
                                     (double)(y - app->drag_last.y));
                    app->drag_last.x = x;
                    app->drag_last.y = y;
                    request_viewport_frame(app);
                }
            }
        }
        return 0;
    case WM_LBUTTONUP:
        if (app && app->mouse_down) {
            int x = GET_X_LPARAM(l_param);
            int y = GET_Y_LPARAM(l_param);
            BOOL was_dragging = app->dragging;
            app->mouse_down = FALSE;
            app->dragging = FALSE;
            if (GetCapture() == window) ReleaseCapture();
            if (was_dragging) {
                request_viewport_frame(app);
            } else if (app->marker_image) {
                size_t hit = find_marker_at(app, x, y, marker_screen_size(app));
                if (hit != SIZE_MAX) {
                    app->datasets[app->active_map].items[hit].hidden = TRUE;
                    save_state_or_warn(app);
                    update_controls(app);
                    request_viewport_frame(app);
                }
            }
        }
        return 0;
    case WM_RBUTTONDOWN:
        SetFocus(window);
        return 0;
    case WM_RBUTTONUP:
        if (app)
            change_marker_color_at(app, GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param),
                                   MARKER_COLOR_GREEN);
        return 0;
    case WM_MBUTTONDOWN:
        SetFocus(window);
        return 0;
    case WM_MBUTTONUP:
        if (app)
            change_marker_color_at(app, GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param),
                                   MARKER_COLOR_RED);
        return 0;
    case WM_CONTEXTMENU:
        return 0;
    case WM_CAPTURECHANGED:
        if (app) {
            BOOL was_dragging = app->dragging;
            app->mouse_down = FALSE;
            app->dragging = FALSE;
            if (was_dragging) request_viewport_frame(app);
        }
        return 0;
    case WM_MOUSELEAVE:
        if (app) {
            app->mouse_tracking = FALSE;
            app->cursor_world_valid = FALSE;
            schedule_status_update(app);
        }
        return 0;
    case WM_SETCURSOR:
        if (app && LOWORD(l_param) == HTCLIENT) {
            POINT point;
            GetCursorPos(&point);
            ScreenToClient(window, &point);
            if (app->dragging) SetCursor(LoadCursorW(NULL, IDC_SIZEALL));
            else if (app->marker_image &&
                     find_marker_at(app, point.x, point.y, marker_screen_size(app)) != SIZE_MAX)
                SetCursor(LoadCursorW(NULL, IDC_HAND));
            else SetCursor(LoadCursorW(NULL, IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_KEYDOWN:
        if (app && w_param == VK_HOME) {
            dvm_map_view_reset(&app->views[app->active_map]);
            update_controls(app);
            request_viewport_frame(app);
            return 0;
        }
        break;
    case WM_DESTROY:
        if (app) {
            cancel_viewport_frame(app);
            KillTimer(window, ID_ZOOM_QUALITY_TIMER);
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

static BOOL register_window_classes(HINSTANCE instance)
{
    WNDCLASSEXW window_class;
    ZeroMemory(&window_class, sizeof(window_class));
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = main_window_proc;
    window_class.hInstance = instance;
    window_class.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
    window_class.hIconSm = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
    window_class.hCursor = LoadCursorW(NULL, IDC_ARROW);
    window_class.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    window_class.lpszClassName = MAIN_CLASS_NAME;
    if (!RegisterClassExW(&window_class)) return FALSE;
    ZeroMemory(&window_class, sizeof(window_class));
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = viewport_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(NULL, IDC_ARROW);
    window_class.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    window_class.lpszClassName = VIEW_CLASS_NAME;
    return RegisterClassExW(&window_class) != 0;
}

static void cleanup_app(App *app)
{
    int map_index;
    if (!app) return;
    destroy_viewport_buffer(app);
    destroy_marker_display_image(app);
    if (app->marker_image) GdipDisposeImage(app->marker_image);
    for (map_index = 0; map_index < DVM_MAP_COUNT; ++map_index) {
        dvm_map_view_destroy(&app->views[map_index]);
        dvm_free_spawn_dataset(&app->datasets[map_index]);
    }
}

static BOOL initialize_content(App *app, wchar_t *error, size_t error_capacity)
{
    int map_index;
    for (map_index = 0; map_index < DVM_MAP_COUNT; ++map_index) {
        if (!dvm_load_spawn_dataset(app->base_directory, map_index,
                                    &app->datasets[map_index], error,
                                    error_capacity)) return FALSE;
        if (!dvm_map_view_load(&app->views[map_index], app->base_directory,
                               map_index, error, error_capacity)) return FALSE;
    }
    return TRUE;
}

static BOOL test_check(FILE *report, BOOL condition, const char *name, int *failures)
{
    fprintf(report, "%s: %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) ++*failures;
    return condition;
}

static BOOL test_marker_color_images(App *app, FILE *report)
{
    GpImage *normal;
    GpImage *green;
    GpImage *red;
    GpRect rectangle;
    BitmapData normal_data;
    BitmapData green_data;
    BitmapData red_data;
    BOOL normal_locked = FALSE;
    BOOL green_locked = FALSE;
    BOOL red_locked = FALSE;
    BOOL valid = TRUE;
    int failure_kind = 0;
    unsigned int colored_pixels = 0u;
    int size = marker_screen_size(app);
    int y;
    normal = get_colored_marker_display_image(app, size, MARKER_COLOR_NORMAL);
    green = get_colored_marker_display_image(app, size, MARKER_COLOR_GREEN);
    red = get_colored_marker_display_image(app, size, MARKER_COLOR_RED);
    if (!normal || !green || !red || normal == green || normal == red || green == red) {
        fprintf(report, "INFO: marker color cache creation or identity failed\n");
        return FALSE;
    }
    rectangle.X = 0;
    rectangle.Y = 0;
    rectangle.Width = size;
    rectangle.Height = size;
    ZeroMemory(&normal_data, sizeof(normal_data));
    ZeroMemory(&green_data, sizeof(green_data));
    ZeroMemory(&red_data, sizeof(red_data));
    normal_locked = GdipBitmapLockBits((GpBitmap *)normal, &rectangle, ImageLockModeRead,
                                        PixelFormat32bppPARGB, &normal_data) == Ok;
    green_locked = GdipBitmapLockBits((GpBitmap *)green, &rectangle, ImageLockModeRead,
                                      PixelFormat32bppPARGB, &green_data) == Ok;
    red_locked = GdipBitmapLockBits((GpBitmap *)red, &rectangle, ImageLockModeRead,
                                    PixelFormat32bppPARGB, &red_data) == Ok;
    if (!normal_locked || !green_locked || !red_locked) {
        valid = FALSE;
        failure_kind = 1;
    }
    for (y = 0; valid && y < size; ++y) {
        const BYTE *normal_row = (const BYTE *)normal_data.Scan0 +
                                 (ptrdiff_t)y * normal_data.Stride;
        const BYTE *green_row = (const BYTE *)green_data.Scan0 +
                                (ptrdiff_t)y * green_data.Stride;
        const BYTE *red_row = (const BYTE *)red_data.Scan0 +
                              (ptrdiff_t)y * red_data.Stride;
        int x;
        for (x = 0; x < size; ++x) {
            const BYTE *normal_pixel = normal_row + x * 4;
            const BYTE *green_pixel = green_row + x * 4;
            const BYTE *red_pixel = red_row + x * 4;
            BYTE alpha = normal_pixel[3];
            BYTE normal_blue;
            BYTE normal_green;
            BYTE normal_red;
            BYTE maximum;
            BYTE minimum;
            if (green_pixel[3] != alpha || red_pixel[3] != alpha) {
                valid = FALSE;
                failure_kind = 2;
                break;
            }
            if (alpha < 128u) continue;
            normal_blue = unpremultiply_channel(normal_pixel[0], alpha);
            normal_green = unpremultiply_channel(normal_pixel[1], alpha);
            normal_red = unpremultiply_channel(normal_pixel[2], alpha);
            maximum = normal_red > normal_green ? normal_red : normal_green;
            if (normal_blue > maximum) maximum = normal_blue;
            minimum = normal_red < normal_green ? normal_red : normal_green;
            if (normal_blue < minimum) minimum = normal_blue;
            if (maximum != 0u &&
                (unsigned int)(maximum - minimum) * 100u >=
                    (unsigned int)maximum * 20u) {
                if (!(green_pixel[1] > green_pixel[2] &&
                      green_pixel[1] > green_pixel[0] &&
                      red_pixel[2] > red_pixel[1] &&
                      red_pixel[2] > red_pixel[0])) {
                    valid = FALSE;
                    failure_kind = 3;
                    break;
                }
                ++colored_pixels;
            } else if (memcmp(normal_pixel, green_pixel, 4u) != 0 ||
                       memcmp(normal_pixel, red_pixel, 4u) != 0) {
                valid = FALSE;
                failure_kind = 4;
                break;
            }
        }
    }
    if (red_locked) GdipBitmapUnlockBits((GpBitmap *)red, &red_data);
    if (green_locked) GdipBitmapUnlockBits((GpBitmap *)green, &green_data);
    if (normal_locked) GdipBitmapUnlockBits((GpBitmap *)normal, &normal_data);
    if (!valid || colored_pixels == 0u)
        fprintf(report, "INFO: marker pixel test failure kind=%d colored=%u\n",
                failure_kind, colored_pixels);
    return valid && colored_pixels != 0u;
}

static int run_self_test(App *app)
{
    wchar_t report_path[DVM_PATH_CAP];
    wchar_t state_test_path[DVM_PATH_CAP];
    FILE *report;
    int failures = 0;
    int map_index;
    wchar_t state_error[512];
    if (!dvm_join_path(report_path, DVM_PATH_CAP, app->base_directory,
                       L"self_test_report.txt") ||
        !dvm_join_path(state_test_path, DVM_PATH_CAP, app->base_directory,
                       L"self_test_state.txt")) return 3;
    report = _wfopen(report_path, L"wb");
    if (!report) return 3;
    fprintf(report, "DayZVehicleMap self-test\n");
    {
        HICON embedded_icon = (HICON)LoadImageW(app->instance,
            MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR);
        test_check(report, embedded_icon != NULL, "icon.ico embedded as executable icon",
                   &failures);
        if (embedded_icon) DestroyIcon(embedded_icon);
    }
    {
        wchar_t marker_test_error[512];
        BOOL marker_loaded;
        marker_test_error[0] = L'\0';
        marker_loaded = load_marker_image(app, marker_test_error,
                                          sizeof(marker_test_error) /
                                              sizeof(marker_test_error[0]));
        test_check(report, marker_loaded, "yellow marker image loads for rendering tests",
                   &failures);
        test_check(report, marker_loaded && test_marker_color_images(app, report),
                   "cached marker recoloring preserves alpha and neutral icon detail",
                   &failures);
        if (marker_loaded) {
            GpImage *green_once = get_colored_marker_display_image(
                app, marker_screen_size(app), MARKER_COLOR_GREEN);
            GpImage *green_again = get_colored_marker_display_image(
                app, marker_screen_size(app), MARKER_COLOR_GREEN);
            GpImage *red_once = get_colored_marker_display_image(
                app, marker_screen_size(app), MARKER_COLOR_RED);
            GpImage *red_again = get_colored_marker_display_image(
                app, marker_screen_size(app), MARKER_COLOR_RED);
            test_check(report, green_once && red_once && green_once == green_again &&
                       red_once == red_again,
                       "green and red marker variants are reused from cache", &failures);
            destroy_marker_display_image(app);
            test_check(report, !app->marker_display_image &&
                       !app->marker_green_display_image &&
                       !app->marker_red_display_image &&
                       app->marker_display_size == 0,
                       "all marker variant caches are released together", &failures);
        }
    }
    for (map_index = 0; map_index < DVM_MAP_COUNT; ++map_index) {
        const MapInfo *map = dvm_map_info(map_index);
        SpawnDataset *dataset = &app->datasets[map_index];
        size_t expected_total = 0;
        int type_index;
        char check_name[128];
        for (type_index = 0; type_index < DVM_VEHICLE_TYPE_COUNT; ++type_index)
            expected_total += map->expected_type_counts[type_index];
        _snprintf(check_name, sizeof(check_name), "%s exact spawn total", map->id);
        check_name[sizeof(check_name) - 1] = '\0';
        test_check(report, dataset->count == expected_total, check_name, &failures);
        _snprintf(check_name, sizeof(check_name), "%s calibrated image dimensions", map->id);
        check_name[sizeof(check_name) - 1] = '\0';
        test_check(report,
                   app->views[map_index].image_width == map->expected_image_width &&
                   app->views[map_index].image_height == map->expected_image_height,
                   check_name, &failures);
        _snprintf(check_name, sizeof(check_name), "%s paved and dirt road vectors", map->id);
        check_name[sizeof(check_name) - 1] = '\0';
        test_check(report,
                   app->views[map_index].road_segment_count > 0u &&
                   app->views[map_index].asphalt_segment_count > 0u &&
                   app->views[map_index].dirt_segment_count > 0u,
                   check_name, &failures);
        dvm_map_view_resize(&app->views[map_index], 1000, 760);
    }
    {
        BOOL official_motorbike_sets_valid = TRUE;
        int official_map;
        for (official_map = 0; official_map < 3; ++official_map) {
            SpawnDataset *dataset = &app->datasets[official_map];
            size_t index;
            if (dataset->type_counts[7] == 0u ||
                dataset->type_counts[7] != dataset->type_counts[8]) {
                official_motorbike_sets_valid = FALSE;
                break;
            }
            for (index = 0; index < dataset->count; ++index) {
                const VehicleSpawn *spawn = &dataset->items[index];
                size_t other;
                BOOL found = FALSE;
                int matching_type;
                if (spawn->type_index != 7 && spawn->type_index != 8) continue;
                matching_type = spawn->type_index == 7 ? 8 : 7;
                for (other = 0; other < dataset->count; ++other) {
                    const VehicleSpawn *candidate = &dataset->items[other];
                    if (candidate->type_index == matching_type &&
                        candidate->x == spawn->x && candidate->z == spawn->z) {
                        found = TRUE;
                        break;
                    }
                }
                if (!found) {
                    official_motorbike_sets_valid = FALSE;
                    break;
                }
            }
            if (!official_motorbike_sets_valid) break;
        }
        test_check(report, official_motorbike_sets_valid,
                   "Jana 50 and Bitrak 682 share each verified official spawn set",
                   &failures);
        test_check(report,
                   app->datasets[3].type_counts[7] == 0u &&
                   app->datasets[3].type_counts[8] == 0u &&
                   app->datasets[4].type_counts[7] == 0u &&
                   app->datasets[4].type_counts[8] == 0u &&
                   app->datasets[5].type_counts[7] == 0u &&
                   app->datasets[5].type_counts[8] == 0u &&
                   app->datasets[6].type_counts[7] == 0u &&
                   app->datasets[6].type_counts[8] == 0u,
                   "new two-wheeled classes are limited to official supported maps",
                   &failures);
    }
    {
        MapView *view = &app->views[0];
        double screen_x;
        double screen_y;
        double world_x;
        double world_z;
        double before_x;
        double before_z;
        BOOL transformed = dvm_map_world_to_screen(view, 4321.25, 9876.5,
                                                   &screen_x, &screen_y) &&
                           dvm_map_screen_to_world(view, screen_x, screen_y,
                                                   &world_x, &world_z);
        test_check(report, transformed && fabs(world_x - 4321.25) < 0.000001 &&
                   fabs(world_z - 9876.5) < 0.000001,
                   "world/image transform round trip and north-up Z inversion", &failures);
        dvm_map_screen_to_world(view, 500.0, 380.0, &before_x, &before_z);
        dvm_map_view_zoom_at(view, 3.0, 500.0, 380.0);
        dvm_map_screen_to_world(view, 500.0, 380.0, &world_x, &world_z);
        test_check(report, fabs(world_x - before_x) < 0.000001 &&
                   fabs(world_z - before_z) < 0.000001,
                   "cursor-anchored zoom", &failures);
        {
            double old_pan = view->pan_x;
            dvm_map_view_pan(view, -120.0, -80.0);
            test_check(report, view->pan_x != old_pan, "two-axis pan while zoomed", &failures);
        }
    }
    {
        SpawnDataset *chernarus = &app->datasets[0];
        size_t other_before[DVM_MAP_COUNT];
        BOOL other_maps_unchanged = TRUE;
        int other_map;
        for (other_map = 1; other_map < DVM_MAP_COUNT; ++other_map)
            other_before[other_map] = dvm_remaining_total(&app->datasets[other_map]);
        chernarus->items[0].hidden = TRUE;
        for (other_map = 1; other_map < DVM_MAP_COUNT; ++other_map) {
            if (dvm_remaining_total(&app->datasets[other_map]) != other_before[other_map])
                other_maps_unchanged = FALSE;
        }
        test_check(report, dvm_remaining_total(chernarus) == chernarus->count - 1 &&
                   other_maps_unchanged,
                   "per-map checked-state isolation", &failures);
        chernarus->items[1].hidden = TRUE;
        {
            int second_type = chernarus->items[1].type_index;
            int first_type = chernarus->items[0].type_index;
            size_t other_before;
            if (second_type == first_type) {
                size_t i;
                for (i = 0; i < chernarus->count; ++i) {
                    if (chernarus->items[i].type_index != first_type) {
                        chernarus->items[i].hidden = TRUE;
                        second_type = chernarus->items[i].type_index;
                        break;
                    }
                }
            }
            other_before = dvm_remaining_for_type(chernarus, second_type);
            dvm_restore_type(chernarus, first_type);
            test_check(report,
                       dvm_remaining_for_type(chernarus, first_type) ==
                           chernarus->type_counts[first_type] &&
                       dvm_remaining_for_type(chernarus, second_type) == other_before,
                       "restore one type does not restore another", &failures);
        }
        dvm_restore_all(chernarus);
        test_check(report, dvm_remaining_total(chernarus) == chernarus->count &&
                   other_maps_unchanged,
                   "Restore All affects current dataset only", &failures);
        chernarus->type_visible[0] = FALSE;
        test_check(report, dvm_remaining_total(chernarus) == chernarus->count,
                   "filter visibility does not check markers", &failures);
        chernarus->type_visible[0] = TRUE;
    }
    {
        SpawnDataset *dataset = &app->datasets[0];
        VehicleSpawn *first = &dataset->items[0];
        VehicleSpawn *second = &dataset->items[1];
        size_t other_index;
        first->color_state = MARKER_COLOR_NORMAL;
        select_marker_color(first, MARKER_COLOR_GREEN);
        test_check(report, first->color_state == MARKER_COLOR_GREEN,
                   "right-click transition yellow to green", &failures);
        select_marker_color(first, MARKER_COLOR_GREEN);
        test_check(report, first->color_state == MARKER_COLOR_NORMAL,
                   "right-click transition green to yellow", &failures);
        first->color_state = MARKER_COLOR_RED;
        select_marker_color(first, MARKER_COLOR_GREEN);
        test_check(report, first->color_state == MARKER_COLOR_GREEN,
                   "right-click transition red to green", &failures);
        first->color_state = MARKER_COLOR_NORMAL;
        select_marker_color(first, MARKER_COLOR_RED);
        test_check(report, first->color_state == MARKER_COLOR_RED,
                   "middle-click transition yellow to red", &failures);
        select_marker_color(first, MARKER_COLOR_RED);
        test_check(report, first->color_state == MARKER_COLOR_NORMAL,
                   "middle-click transition red to yellow", &failures);
        first->color_state = MARKER_COLOR_GREEN;
        select_marker_color(first, MARKER_COLOR_RED);
        test_check(report, first->color_state == MARKER_COLOR_RED,
                   "middle-click transition green to red", &failures);
        first->color_state = MARKER_COLOR_GREEN;
        second->color_state = MARKER_COLOR_RED;
        test_check(report,
                   first->type_index == second->type_index &&
                   first->color_state == MARKER_COLOR_GREEN &&
                   second->color_state == MARKER_COLOR_RED &&
                   dataset->items[2].color_state == MARKER_COLOR_NORMAL,
                   "individual markers of one vehicle type hold independent colors",
                   &failures);
        dataset->type_visible[first->type_index] = FALSE;
        dvm_map_view_resize(&app->views[0], 901, 677);
        dvm_map_view_zoom_at(&app->views[0], 1.4, 450.0, 338.0);
        dvm_map_view_pan(&app->views[0], -17.0, 11.0);
        app->active_map = 1;
        app->active_map = 0;
        dataset->type_visible[first->type_index] = TRUE;
        test_check(report,
                   first->color_state == MARKER_COLOR_GREEN &&
                   second->color_state == MARKER_COLOR_RED,
                   "filter map switch resize zoom and pan preserve marker colors",
                   &failures);
        other_index = 0u;
        while (other_index < dataset->count &&
               dataset->items[other_index].type_index == first->type_index) ++other_index;
        if (other_index < dataset->count) {
            dataset->items[other_index].color_state = MARKER_COLOR_RED;
            dvm_restore_type(dataset, first->type_index);
            test_check(report,
                       first->color_state == MARKER_COLOR_NORMAL &&
                       second->color_state == MARKER_COLOR_NORMAL &&
                       dataset->items[other_index].color_state == MARKER_COLOR_RED,
                       "per-type Restore clears colors only in its existing scope",
                       &failures);
            test_check(report, dvm_has_restorable_state(dataset),
                       "colored marker enables Restore without being hidden", &failures);
        }
        dvm_restore_all(dataset);
        test_check(report, !dvm_has_restorable_state(dataset),
                   "Restore All clears every marker color in current map", &failures);
    }
    for (map_index = 0; map_index < DVM_MAP_COUNT; ++map_index) {
        size_t hidden_index = map_index == 0 ? 0u :
                              app->datasets[map_index].count - 1u;
        size_t color_index = hidden_index == 0u ? 1u : 0u;
        dvm_restore_all(&app->datasets[map_index]);
        app->datasets[map_index].items[hidden_index].hidden = TRUE;
        app->datasets[map_index].items[color_index].color_state =
            map_index & 1 ? MARKER_COLOR_RED : MARKER_COLOR_GREEN;
    }
    test_check(report,
               dvm_state_save(state_test_path, app->datasets, state_error,
                              sizeof(state_error) / sizeof(state_error[0])),
               "state save to executable directory", &failures);
    for (map_index = 0; map_index < DVM_MAP_COUNT; ++map_index)
        dvm_restore_all(&app->datasets[map_index]);
    {
        BOOL all_maps_loaded = dvm_state_load(state_test_path, app->datasets, state_error,
                                               sizeof(state_error) / sizeof(state_error[0]));
        for (map_index = 0; map_index < DVM_MAP_COUNT; ++map_index) {
            size_t hidden_index = map_index == 0 ? 0u :
                                  app->datasets[map_index].count - 1u;
            size_t color_index = hidden_index == 0u ? 1u : 0u;
            MarkerColorState expected = map_index & 1
                ? MARKER_COLOR_RED : MARKER_COLOR_GREEN;
            if (!app->datasets[map_index].items[hidden_index].hidden ||
                app->datasets[map_index].items[color_index].hidden ||
                app->datasets[map_index].items[color_index].color_state != expected)
                all_maps_loaded = FALSE;
        }
        test_check(report, all_maps_loaded,
                   "coordinate-keyed hidden and color state round trip across all maps",
                   &failures);
    }
    {
        FILE *malformed = _wfopen(state_test_path, L"ab");
        if (malformed) {
            fputs("broken,state,line\n"
                  "unknown,VehicleBoat,not-a-number,7,0,purple\n", malformed);
            fclose(malformed);
        }
        for (map_index = 0; map_index < DVM_MAP_COUNT; ++map_index)
            dvm_restore_all(&app->datasets[map_index]);
        state_error[0] = L'\0';
        test_check(report, malformed != NULL &&
                   dvm_state_load(state_test_path, app->datasets, state_error,
                                  sizeof(state_error) / sizeof(state_error[0])) &&
                   state_error[0] != L'\0' && app->datasets[0].items[0].hidden,
                   "malformed and stale state lines are safely ignored", &failures);
    }
    {
        SpawnDataset *dataset = &app->datasets[0];
        const MapInfo *map = dvm_map_info(0);
        const VehicleTypeInfo *type = dvm_vehicle_type_info(dataset->items[0].type_index);
        FILE *legacy = _wfopen(state_test_path, L"wb");
        BOOL legacy_loaded = FALSE;
        if (legacy && map && type) {
            fprintf(legacy, "DAYZVEHICLEMAP_STATE_V1\n# map,event,x,z\n"
                    "%s,%s,%.10f,%.10f\n", map->id, type->event_name,
                    dataset->items[0].x, dataset->items[0].z);
            fclose(legacy);
            dataset->items[0].color_state = MARKER_COLOR_RED;
            dataset->items[1].color_state = MARKER_COLOR_GREEN;
            legacy_loaded = dvm_state_load(state_test_path, app->datasets, state_error,
                                            sizeof(state_error) /
                                                sizeof(state_error[0]));
        } else if (legacy) {
            fclose(legacy);
        }
        test_check(report, legacy_loaded && dataset->items[0].hidden &&
                   dataset->items[0].color_state == MARKER_COLOR_NORMAL &&
                   dataset->items[1].color_state == MARKER_COLOR_NORMAL,
                   "V1 checked-state file loads with marker colors defaulted to yellow",
                   &failures);
    }
    {
        SpawnDataset *dataset = &app->datasets[0];
        BOOL restored_saved;
        dvm_restore_all(dataset);
        restored_saved = dvm_state_save(state_test_path, app->datasets, state_error,
                                         sizeof(state_error) /
                                             sizeof(state_error[0]));
        dataset->items[0].color_state = MARKER_COLOR_GREEN;
        dataset->items[1].color_state = MARKER_COLOR_RED;
        restored_saved = restored_saved &&
            dvm_state_load(state_test_path, app->datasets, state_error,
                           sizeof(state_error) / sizeof(state_error[0]));
        test_check(report, restored_saved &&
                   dataset->items[0].color_state == MARKER_COLOR_NORMAL &&
                   dataset->items[1].color_state == MARKER_COLOR_NORMAL,
                   "Restore state save removes persisted green and red entries",
                   &failures);
    }
    DeleteFileW(state_test_path);
    for (map_index = 0; map_index < DVM_MAP_COUNT; ++map_index)
        dvm_restore_all(&app->datasets[map_index]);
    {
        SpawnDataset *dataset = &app->datasets[0];
        MapView *view = &app->views[0];
        double screen_x;
        double screen_y;
        size_t hit;
        app->active_map = 0;
        dvm_map_view_reset(view);
        dvm_map_world_to_screen(view, dataset->items[0].x, dataset->items[0].z,
                                &screen_x, &screen_y);
        dvm_map_view_zoom_at(view, 4.0, screen_x, screen_y);
        dvm_map_world_to_screen(view, dataset->items[0].x, dataset->items[0].z,
                                &screen_x, &screen_y);
        hit = find_marker_at(app, (int)lround(screen_x), (int)lround(screen_y), 26);
        test_check(report, hit != SIZE_MAX, "marker hit-testing after zoom", &failures);
        if (hit != SIZE_MAX) dataset->items[hit].hidden = TRUE;
        test_check(report, dvm_remaining_total(dataset) == dataset->count - 1,
                   "one hit hides exactly one spawn", &failures);
        dvm_restore_all(dataset);
        {
            size_t first;
            size_t second;
            BOOL overlap_tested = FALSE;
            for (first = 0; first < dataset->count && !overlap_tested; ++first) {
                for (second = first + 1; second < dataset->count; ++second) {
                    if (dataset->items[first].x == dataset->items[second].x &&
                        dataset->items[first].z == dataset->items[second].z &&
                        dataset->items[first].type_index != dataset->items[second].type_index) {
                        size_t top_hit;
                        size_t next_hit;
                        dvm_map_world_to_screen(view, dataset->items[first].x,
                                                dataset->items[first].z,
                                                &screen_x, &screen_y);
                        top_hit = find_marker_at(app, (int)lround(screen_x),
                                                 (int)lround(screen_y), 26);
                        if (top_hit != SIZE_MAX) dataset->items[top_hit].hidden = TRUE;
                        next_hit = find_marker_at(app, (int)lround(screen_x),
                                                  (int)lround(screen_y), 26);
                        overlap_tested = top_hit != SIZE_MAX && next_hit != SIZE_MAX &&
                                         next_hit != top_hit;
                        break;
                    }
                }
            }
            test_check(report, overlap_tested,
                       "overlapping markers remain individually clickable", &failures);
            dvm_restore_all(dataset);
        }
    }
    fprintf(report, "RESULT: %s (%d failure%s)\n",
            failures == 0 ? "PASS" : "FAIL", failures, failures == 1 ? "" : "s");
    fclose(report);
    return failures == 0 ? 0 : 2;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous_instance,
                    PWSTR command_line, int show_command)
{
    App app;
    GdiplusStartupInput gdiplus_input;
    ULONG_PTR gdiplus_token = 0;
    INITCOMMONCONTROLSEX controls;
    wchar_t error[1024];
    wchar_t marker_error[1024];
    wchar_t state_warning[512];
    HWND window;
    MSG message;
    int result;
    BOOL self_test;
    (void)previous_instance;
    ZeroMemory(&app, sizeof(app));
    app.instance = instance;
    app.active_map = 0;
    self_test = is_self_test_command(command_line);
    enable_dpi_awareness();
    ZeroMemory(&gdiplus_input, sizeof(gdiplus_input));
    gdiplus_input.GdiplusVersion = 1;
    if (GdiplusStartup(&gdiplus_token, &gdiplus_input, NULL) != Ok) {
        MessageBoxW(NULL, L"Windows GDI+ could not be initialized.",
                    L"DayZ Vehicle Map", MB_OK | MB_ICONERROR);
        return 1;
    }
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&controls);
    if (!dvm_executable_directory(app.base_directory,
                                  sizeof(app.base_directory) / sizeof(app.base_directory[0])) ||
        !dvm_join_path(app.state_path,
                       sizeof(app.state_path) / sizeof(app.state_path[0]),
                       app.base_directory, L"checked_state.txt")) {
        MessageBoxW(NULL, L"Could not determine a safe executable-relative data path.",
                    L"DayZ Vehicle Map", MB_OK | MB_ICONERROR);
        GdiplusShutdown(gdiplus_token);
        return 1;
    }
    if (!initialize_content(&app, error, sizeof(error) / sizeof(error[0]))) {
        MessageBoxW(NULL, error, L"Required DayZ Vehicle Map data is unavailable",
                    MB_OK | MB_ICONERROR);
        cleanup_app(&app);
        GdiplusShutdown(gdiplus_token);
        return 1;
    }
    if (self_test) {
        result = run_self_test(&app);
        cleanup_app(&app);
        GdiplusShutdown(gdiplus_token);
        return result;
    }
    marker_error[0] = L'\0';
    load_marker_image(&app, marker_error, sizeof(marker_error) / sizeof(marker_error[0]));
    state_warning[0] = L'\0';
    dvm_state_load(app.state_path, app.datasets, state_warning,
                   sizeof(state_warning) / sizeof(state_warning[0]));
    if (!register_window_classes(instance)) {
        MessageBoxW(NULL, L"Could not register the native window classes.",
                    L"DayZ Vehicle Map", MB_OK | MB_ICONERROR);
        cleanup_app(&app);
        GdiplusShutdown(gdiplus_token);
        return 1;
    }
    window = CreateWindowExW(0, MAIN_CLASS_NAME, L"DayZ Vehicle Map",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1280, 820,
        NULL, NULL, instance, &app);
    if (!window) {
        MessageBoxW(NULL, L"Could not create the main window.",
                    L"DayZ Vehicle Map", MB_OK | MB_ICONERROR);
        cleanup_app(&app);
        GdiplusShutdown(gdiplus_token);
        return 1;
    }
    ShowWindow(window, show_command);
    UpdateWindow(window);
    if (marker_error[0])
        MessageBoxW(window, marker_error, L"Required yellow marker missing",
                    MB_OK | MB_ICONWARNING);
    if (state_warning[0])
        MessageBoxW(window, state_warning, L"State file warning",
                    MB_OK | MB_ICONWARNING);
    while ((result = GetMessageW(&message, NULL, 0, 0)) > 0) {
        if (IsDialogMessageW(window, &message)) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    cleanup_app(&app);
    GdiplusShutdown(gdiplus_token);
    return result < 0 ? 1 : (int)message.wParam;
}
