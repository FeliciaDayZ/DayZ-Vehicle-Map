#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <wchar.h>

#define ID_MAP_COMBO 101
#define ID_RESTORE_ALL 102
#define ID_FIT_MAP 103
#define ID_STATUS 105
#define ID_VIEWPORT 106
#define ID_TYPE_CHECK_BASE 200
#define ID_TYPE_RESTORE_BASE 300

static const char COLOR_MARKER_A_KEY[] =
    "enoch,VehicleOffroadHatchback,335.6468810000,11288.3896490000";
static const char COLOR_MARKER_B_KEY[] =
    "enoch,VehicleTruck01,6272.3600000000,6824.9200000000";
static const char COLOR_MARKER_A_GREEN[] =
    "enoch,VehicleOffroadHatchback,335.6468810000,11288.3896490000,0,green";
static const char COLOR_MARKER_A_RED[] =
    "enoch,VehicleOffroadHatchback,335.6468810000,11288.3896490000,0,red";
static const char COLOR_MARKER_B_GREEN[] =
    "enoch,VehicleTruck01,6272.3600000000,6824.9200000000,0,green";
static const char COLOR_MARKER_B_RED[] =
    "enoch,VehicleTruck01,6272.3600000000,6824.9200000000,0,red";

static BOOL wait_for_exit(HANDLE process, DWORD milliseconds)
{
    return WaitForSingleObject(process, milliseconds) == WAIT_OBJECT_0;
}

static BOOL copy_file_with_retry(const wchar_t *source, const wchar_t *destination)
{
    int attempt;
    for (attempt = 0; attempt < 30; ++attempt) {
        if (CopyFileW(source, destination, FALSE)) return TRUE;
        Sleep(100);
    }
    return FALSE;
}

static HWND wait_for_window(const wchar_t *class_name, const wchar_t *title,
                            DWORD milliseconds)
{
    DWORD elapsed = 0;
    while (elapsed < milliseconds) {
        HWND window = FindWindowW(class_name, title);
        if (window && IsWindowVisible(window)) return window;
        Sleep(50);
        elapsed += 50;
    }
    return NULL;
}

static BOOL file_contains_ascii(const wchar_t *path, const char *needle)
{
    FILE *file;
    long length;
    char *data;
    BOOL found = FALSE;
    file = _wfopen(path, L"rb");
    if (!file) return FALSE;
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
        length > 1024 * 1024 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return FALSE;
    }
    data = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)length + 1u);
    if (data && fread(data, 1u, (size_t)length, file) == (size_t)length) {
        data[length] = '\0';
        found = strstr(data, needle) != NULL;
    }
    if (data) HeapFree(GetProcessHeap(), 0, data);
    fclose(file);
    return found;
}

static BOOL marker_region_has_color(HWND viewport, int center_x, int center_y,
                                    char color)
{
    HDC dc;
    int matching = 0;
    int y;
    /* Visual changes are delivered on the next capped frame. */
    Sleep(60);
    RedrawWindow(viewport, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
    dc = GetDC(viewport);
    if (!dc) return FALSE;
    for (y = -9; y <= 9; ++y) {
        int x;
        for (x = -9; x <= 9; ++x) {
            COLORREF pixel = GetPixel(dc, center_x + x, center_y + y);
            int red;
            int green;
            int blue;
            if (pixel == CLR_INVALID) continue;
            red = GetRValue(pixel);
            green = GetGValue(pixel);
            blue = GetBValue(pixel);
            if ((color == 'g' && green > red + 35 && green > blue + 35) ||
                (color == 'r' && red > green + 35 && red > blue + 35) ||
                (color == 'y' && red > green + 35 && green > blue + 35))
                ++matching;
        }
    }
    ReleaseDC(viewport, dc);
    return matching >= 8;
}

static void send_marker_button(HWND viewport, UINT down_message, UINT up_message,
                               WPARAM button_flag, int x, int y)
{
    SendMessageW(viewport, down_message, button_flag, MAKELPARAM(x, y));
    SendMessageW(viewport, up_message, 0, MAKELPARAM(x, y));
    RedrawWindow(viewport, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
}

static BOOL write_window_bmp(HWND window, const wchar_t *path)
{
    RECT bounds;
    int width;
    int height;
    HDC screen_dc = NULL;
    HDC memory_dc = NULL;
    HBITMAP bitmap = NULL;
    HGDIOBJ old_bitmap = NULL;
    BITMAPINFO info;
    BITMAPFILEHEADER file_header;
    BYTE *pixels = NULL;
    DWORD image_size;
    HANDLE file = INVALID_HANDLE_VALUE;
    DWORD written;
    BOOL success = FALSE;
    if (!GetWindowRect(window, &bounds)) return FALSE;
    width = bounds.right - bounds.left;
    height = bounds.bottom - bounds.top;
    if (width <= 0 || height <= 0 || width > 10000 || height > 10000) return FALSE;
    image_size = (DWORD)((uint64_t)(unsigned int)width *
                         (uint64_t)(unsigned int)height * 4u);
    screen_dc = GetDC(NULL);
    if (!screen_dc) goto cleanup;
    memory_dc = CreateCompatibleDC(screen_dc);
    bitmap = CreateCompatibleBitmap(screen_dc, width, height);
    if (!memory_dc || !bitmap) goto cleanup;
    old_bitmap = SelectObject(memory_dc, bitmap);
    if (!PrintWindow(window, memory_dc, 2u))
        BitBlt(memory_dc, 0, 0, width, height, screen_dc,
               bounds.left, bounds.top, SRCCOPY | CAPTUREBLT);
    ZeroMemory(&info, sizeof(info));
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    info.bmiHeader.biSizeImage = image_size;
    pixels = HeapAlloc(GetProcessHeap(), 0, image_size);
    if (!pixels || !GetDIBits(memory_dc, bitmap, 0, (UINT)height,
                              pixels, &info, DIB_RGB_COLORS)) goto cleanup;
    ZeroMemory(&file_header, sizeof(file_header));
    file_header.bfType = 0x4D42;
    file_header.bfOffBits = sizeof(file_header) + sizeof(info.bmiHeader);
    file_header.bfSize = file_header.bfOffBits + image_size;
    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) goto cleanup;
    if (!WriteFile(file, &file_header, sizeof(file_header), &written, NULL) ||
        written != sizeof(file_header)) goto cleanup;
    if (!WriteFile(file, &info.bmiHeader, sizeof(info.bmiHeader), &written, NULL) ||
        written != sizeof(info.bmiHeader)) goto cleanup;
    if (!WriteFile(file, pixels, image_size, &written, NULL) || written != image_size)
        goto cleanup;
    success = TRUE;

cleanup:
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (pixels) HeapFree(GetProcessHeap(), 0, pixels);
    if (old_bitmap) SelectObject(memory_dc, old_bitmap);
    if (bitmap) DeleteObject(bitmap);
    if (memory_dc) DeleteDC(memory_dc);
    if (screen_dc) ReleaseDC(NULL, screen_dc);
    return success;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous_instance,
                    PWSTR command_line, int show_command)
{
    wchar_t module_path[1024];
    wchar_t project_directory[1024];
    wchar_t executable_path[1024];
    wchar_t process_command[1200];
    wchar_t report_path[1024];
    wchar_t capture_path[1024];
    wchar_t livonia_capture_path[1024];
    wchar_t marker_path[1024];
    wchar_t state_path[1024];
    wchar_t state_backup_path[1024];
    wchar_t windows_directory[MAX_PATH];
    wchar_t *slash;
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    HWND dialog;
    HWND main_window = NULL;
    HWND combo;
    HWND viewport;
    HWND status;
    HWND first_filter;
    HWND second_filter;
    RECT view_bounds;
    int livonia_marker_fit_x = 0;
    int livonia_marker_fit_y = 0;
    int color_marker_fit_x = 0;
    int color_marker_fit_y = 0;
    FILE *report;
    int failures = 0;
    wchar_t status_before[512];
    wchar_t status_after[512];
    BOOL had_state_backup = FALSE;
    BOOL marker_present;
    (void)instance;
    (void)previous_instance;
    (void)command_line;
    (void)show_command;
    ZeroMemory(&process, sizeof(process));
    if (!GetModuleFileNameW(NULL, module_path,
                            sizeof(module_path) / sizeof(module_path[0]))) return 3;
    wcscpy(project_directory, module_path);
    slash = wcsrchr(project_directory, L'\\');
    if (!slash) return 3;
    *slash = L'\0';
    slash = wcsrchr(project_directory, L'\\');
    if (!slash) return 3;
    *slash = L'\0';
    _snwprintf(executable_path, sizeof(executable_path) / sizeof(executable_path[0]),
               L"%ls\\DayZVehicleMap.exe", project_directory);
    _snwprintf(process_command, sizeof(process_command) / sizeof(process_command[0]),
               L"\"%ls\"", executable_path);
    _snwprintf(report_path, sizeof(report_path) / sizeof(report_path[0]),
               L"%ls\\ui_smoke_report.txt", project_directory);
    _snwprintf(capture_path, sizeof(capture_path) / sizeof(capture_path[0]),
               L"%ls\\ui_smoke.bmp", project_directory);
    _snwprintf(livonia_capture_path,
               sizeof(livonia_capture_path) / sizeof(livonia_capture_path[0]),
               L"%ls\\ui_smoke_livonia.bmp", project_directory);
    _snwprintf(marker_path, sizeof(marker_path) / sizeof(marker_path[0]),
               L"%ls\\data\\marker.png", project_directory);
    marker_present = GetFileAttributesW(marker_path) != INVALID_FILE_ATTRIBUTES;
    _snwprintf(state_path, sizeof(state_path) / sizeof(state_path[0]),
               L"%ls\\checked_state.txt", project_directory);
    _snwprintf(state_backup_path,
               sizeof(state_backup_path) / sizeof(state_backup_path[0]),
               L"%ls\\build\\ui_smoke_state.backup", project_directory);
    report = _wfopen(report_path, L"wb");
    if (!report) return 3;
    setvbuf(report, NULL, _IONBF, 0);
    DeleteFileW(state_backup_path);
    if (GetFileAttributesW(state_path) != INVALID_FILE_ATTRIBUTES) {
        had_state_backup = CopyFileW(state_path, state_backup_path, FALSE);
        if (!had_state_backup) {
            fprintf(report, "FAIL: could not preserve existing checked_state.txt\n");
            fclose(report);
            return 3;
        }
    }
    {
        FILE *seed = _wfopen(state_path, L"wb");
        if (!seed) {
            fprintf(report, "FAIL: could not create isolated persistence fixture\n");
            goto finished;
        }
        fputs("DAYZVEHICLEMAP_STATE_V1\n# map,event,x,z\n"
              "chernarusplus,VehicleCivilianSedan,12071.933594,9129.989258\n"
              "enoch,VehicleCivilianSedan,8475.086914,12021.825195\n", seed);
        fclose(seed);
        fprintf(report, "PASS: isolated coordinate-keyed persistence fixture created\n");
    }
    ZeroMemory(&startup, sizeof(startup));
    ZeroMemory(&process, sizeof(process));
    startup.cb = sizeof(startup);
    GetWindowsDirectoryW(windows_directory,
                         sizeof(windows_directory) / sizeof(windows_directory[0]));
    if (!CreateProcessW(executable_path, process_command, NULL, NULL, FALSE, 0,
                        NULL, windows_directory, &startup, &process)) {
        fprintf(report, "FAIL: launch from a non-project working directory\n");
        fclose(report);
        return 2;
    }
    fprintf(report, "PASS: launched from a non-project working directory\n");
    WaitForInputIdle(process.hProcess, 15000);
    main_window = wait_for_window(L"DayZVehicleMapMainWindow", L"DayZ Vehicle Map", 15000);
    dialog = FindWindowW(L"#32770", L"Required yellow marker missing");
    if (marker_present && !dialog) {
        fprintf(report, "PASS: required yellow marker loaded without a warning\n");
    } else if (!marker_present && dialog) {
        HWND ok_button = GetDlgItem(dialog, IDOK);
        if (!main_window) main_window = GetWindow(dialog, GW_OWNER);
        if (ok_button) SendMessageW(ok_button, BM_CLICK, 0, 0);
        fprintf(report, "PASS: useful native missing-marker warning displayed\n");
    } else {
        fprintf(report, "FAIL: marker availability warning behavior\n");
        ++failures;
    }
    if (!main_window) {
        fprintf(report, "FAIL: main native window not found\n");
        ++failures;
        TerminateProcess(process.hProcess, 2);
        goto finished;
    }
    fprintf(report, "PASS: main native window created\n");
    combo = GetDlgItem(main_window, ID_MAP_COMBO);
    viewport = GetDlgItem(main_window, ID_VIEWPORT);
    status = GetDlgItem(main_window, ID_STATUS);
    first_filter = GetDlgItem(main_window, ID_TYPE_CHECK_BASE);
    second_filter = GetDlgItem(main_window, ID_TYPE_CHECK_BASE + 1);
    if (combo && viewport && status && first_filter && second_filter) {
        fprintf(report, "PASS: map selector, filters, status, and viewport controls exist\n");
    } else {
        fprintf(report, "FAIL: one or more required controls are missing\n");
        ++failures;
    }
    if (combo && SendMessageW(combo, CB_GETCOUNT, 0, 0) == 7)
        fprintf(report, "PASS: map selector contains all seven supported maps\n");
    else { fprintf(report, "FAIL: map selector does not contain seven maps\n"); ++failures; }
    if (status) {
        status_before[0] = L'\0';
        GetWindowTextW(status, status_before,
                       sizeof(status_before) / sizeof(status_before[0]));
        if (wcsstr(status_before, L"Remaining: 648 of 649"))
            fprintf(report, "PASS: Chernarus checked state loaded from executable directory\n");
        else { fprintf(report, "FAIL: Chernarus checked state load\n"); ++failures; }
    }
    {
        wchar_t jana_text[128];
        wchar_t bitrak_text[128];
        HWND jana_filter = GetDlgItem(main_window, ID_TYPE_CHECK_BASE + 7);
        HWND bitrak_filter = GetDlgItem(main_window, ID_TYPE_CHECK_BASE + 8);
        jana_text[0] = L'\0';
        bitrak_text[0] = L'\0';
        if (jana_filter) GetWindowTextW(jana_filter, jana_text,
                                        sizeof(jana_text) / sizeof(jana_text[0]));
        if (bitrak_filter) GetWindowTextW(bitrak_filter, bitrak_text,
                                          sizeof(bitrak_text) / sizeof(bitrak_text[0]));
        if (jana_filter && bitrak_filter && IsWindowVisible(jana_filter) &&
            IsWindowVisible(bitrak_filter) && wcsstr(jana_text, L"97/97") &&
            wcsstr(bitrak_text, L"97/97"))
            fprintf(report, "PASS: Chernarus exposes both two-wheeled spawn sets\n");
        else { fprintf(report, "FAIL: Chernarus two-wheeled spawn sets\n"); ++failures; }
    }
    if (combo) {
        SendMessageW(combo, CB_SETCURSEL, 1, 0);
        SendMessageW(main_window, WM_COMMAND, MAKEWPARAM(ID_MAP_COMBO, CBN_SELCHANGE),
                     (LPARAM)combo);
        Sleep(1500);
        if (SendMessageW(combo, CB_GETCURSEL, 0, 0) == 1)
            fprintf(report, "PASS: switched to Livonia\n");
        else { fprintf(report, "FAIL: Livonia switch\n"); ++failures; }
        if (status) {
            GetWindowTextW(status, status_after,
                           sizeof(status_after) / sizeof(status_after[0]));
            if (wcsstr(status_after, L"Remaining: 306 of 307"))
                fprintf(report, "PASS: Livonia uses its independent checked state\n");
            else { fprintf(report, "FAIL: Livonia checked-state isolation\n"); ++failures; }
        }
    }
    if (GetDlgItem(main_window, ID_TYPE_CHECK_BASE + 7) &&
        GetDlgItem(main_window, ID_TYPE_CHECK_BASE + 8)) {
        wchar_t jana_text[128];
        wchar_t bitrak_text[128];
        HWND jana_filter = GetDlgItem(main_window, ID_TYPE_CHECK_BASE + 7);
        HWND bitrak_filter = GetDlgItem(main_window, ID_TYPE_CHECK_BASE + 8);
        GetWindowTextW(jana_filter, jana_text,
                       sizeof(jana_text) / sizeof(jana_text[0]));
        GetWindowTextW(bitrak_filter, bitrak_text,
                       sizeof(bitrak_text) / sizeof(bitrak_text[0]));
        if (IsWindowVisible(jana_filter) && IsWindowVisible(bitrak_filter) &&
            wcsstr(jana_text, L"Jana 50 (Moped)  35/35") &&
            wcsstr(bitrak_text, L"Bitrak 682 (Motorbike)  35/35"))
            fprintf(report, "PASS: Livonia exposes both new two-wheeled vehicle filters\n");
        else { fprintf(report, "FAIL: Livonia two-wheeled filter labels/counts\n"); ++failures; }
        SendMessageW(jana_filter, BM_CLICK, 0, 0);
        if (SendMessageW(jana_filter, BM_GETCHECK, 0, 0) == BST_UNCHECKED &&
            SendMessageW(bitrak_filter, BM_GETCHECK, 0, 0) == BST_CHECKED)
            fprintf(report, "PASS: moped and motorbike filters toggle independently\n");
        else { fprintf(report, "FAIL: independent two-wheeled filters\n"); ++failures; }
        SendMessageW(jana_filter, BM_CLICK, 0, 0);
    } else {
        fprintf(report, "FAIL: two-wheeled filter controls are missing\n");
        ++failures;
    }
    if (viewport && status) {
        RECT client;
        double draw_size;
        double pan_x;
        double pan_y;
        GetClientRect(viewport, &client);
        draw_size = (double)(client.right < client.bottom ? client.right : client.bottom);
        pan_x = ((double)client.right - draw_size) * 0.5;
        pan_y = ((double)client.bottom - draw_size) * 0.5;
        livonia_marker_fit_x = (int)(pan_x + 6272.36 / 12800.0 * draw_size + 0.5);
        livonia_marker_fit_y = (int)(pan_y + (1.0 - 6824.92 / 12800.0) * draw_size + 0.5);
        SendMessageW(viewport, WM_LBUTTONDOWN, MK_LBUTTON,
                     MAKELPARAM(livonia_marker_fit_x, livonia_marker_fit_y));
        SendMessageW(viewport, WM_LBUTTONUP, 0,
                     MAKELPARAM(livonia_marker_fit_x, livonia_marker_fit_y));
        GetWindowTextW(status, status_after,
                       sizeof(status_after) / sizeof(status_after[0]));
        if (wcsstr(status_after, L"Remaining: 305 of 307"))
            fprintf(report, "PASS: left click before zoom hid exactly one marker\n");
        else { fprintf(report, "FAIL: marker click before zoom\n"); ++failures; }
        SendMessageW(GetDlgItem(main_window, ID_TYPE_RESTORE_BASE + 5), BM_CLICK, 0, 0);
        GetWindowTextW(status, status_after,
                       sizeof(status_after) / sizeof(status_after[0]));
        if (wcsstr(status_after, L"Remaining: 306 of 307"))
            fprintf(report, "PASS: per-type restore recovered the clicked marker only\n");
        else { fprintf(report, "FAIL: clicked-marker per-type restore\n"); ++failures; }
    }
    if (first_filter && second_filter) {
        SendMessageW(first_filter, BM_CLICK, 0, 0);
        SendMessageW(second_filter, BM_CLICK, 0, 0);
        if (SendMessageW(first_filter, BM_GETCHECK, 0, 0) == BST_UNCHECKED &&
            SendMessageW(second_filter, BM_GETCHECK, 0, 0) == BST_UNCHECKED)
            fprintf(report, "PASS: multiple vehicle filters toggled independently\n");
        else { fprintf(report, "FAIL: multiple filter toggle\n"); ++failures; }
        SendMessageW(first_filter, BM_CLICK, 0, 0);
        SendMessageW(second_filter, BM_CLICK, 0, 0);
    }
    status_before[0] = L'\0';
    status_after[0] = L'\0';
    if (viewport && status && GetWindowRect(viewport, &view_bounds)) {
        int screen_x = (view_bounds.left + view_bounds.right) / 2;
        int screen_y = (view_bounds.top + view_bounds.bottom) / 2;
        LPARAM wheel_point = MAKELPARAM((SHORT)screen_x, (SHORT)screen_y);
        GetWindowTextW(status, status_before,
                       sizeof(status_before) / sizeof(status_before[0]));
        SendMessageW(viewport, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA * 3), wheel_point);
        Sleep(250);
        GetWindowTextW(status, status_after,
                       sizeof(status_after) / sizeof(status_after[0]));
        if (wcscmp(status_before, status_after) != 0)
            fprintf(report, "PASS: mouse-wheel zoom updated view status\n");
        else { fprintf(report, "FAIL: zoom status did not change\n"); ++failures; }
        {
            RECT client;
            int center_x;
            int center_y;
            int step;
            LARGE_INTEGER frequency;
            LARGE_INTEGER start;
            LARGE_INTEGER end;
            double milliseconds_per_event;
            GetClientRect(viewport, &client);
            center_x = client.right / 2;
            center_y = client.bottom / 2;
            SendMessageW(viewport, WM_LBUTTONDOWN, MK_LBUTTON,
                         MAKELPARAM(center_x, center_y));
            QueryPerformanceFrequency(&frequency);
            QueryPerformanceCounter(&start);
            for (step = 1; step <= 20; ++step) {
                SendMessageW(viewport, WM_MOUSEMOVE, MK_LBUTTON,
                             MAKELPARAM(center_x + step * 3, center_y + step * 2));
                RedrawWindow(viewport, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
            }
            QueryPerformanceCounter(&end);
            SendMessageW(viewport, WM_LBUTTONUP, 0,
                         MAKELPARAM(center_x + 60, center_y + 40));
            RedrawWindow(viewport, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
            milliseconds_per_event =
                (double)(end.QuadPart - start.QuadPart) * 1000.0 /
                (double)frequency.QuadPart / 20.0;
            if (milliseconds_per_event < 100.0)
                fprintf(report, "PASS: drag-pan input averaged %.1f ms/event\n",
                        milliseconds_per_event);
            else {
                fprintf(report, "FAIL: drag-pan input averaged %.1f ms/event\n",
                        milliseconds_per_event);
                ++failures;
            }
            {
                const double zoom_factor = 1.815848;
                int marker_x = (int)((double)center_x +
                    ((double)livonia_marker_fit_x - (double)center_x) * zoom_factor +
                    60.0 + 0.5);
                int marker_y = (int)((double)center_y +
                    ((double)livonia_marker_fit_y - (double)center_y) * zoom_factor +
                    40.0 + 0.5);
                SendMessageW(viewport, WM_LBUTTONDOWN, MK_LBUTTON,
                             MAKELPARAM(marker_x, marker_y));
                SendMessageW(viewport, WM_LBUTTONUP, 0,
                             MAKELPARAM(marker_x, marker_y));
                GetWindowTextW(status, status_after,
                               sizeof(status_after) / sizeof(status_after[0]));
                if (wcsstr(status_after, L"Remaining: 305 of 307"))
                    fprintf(report, "PASS: left click after zoom and pan hid exactly one marker\n");
                else { fprintf(report, "FAIL: marker click after zoom and pan\n"); ++failures; }
                SendMessageW(GetDlgItem(main_window, ID_TYPE_RESTORE_BASE + 5),
                             BM_CLICK, 0, 0);
            }
        }
    }
    if (GetDlgItem(main_window, ID_TYPE_RESTORE_BASE)) {
        SendMessageW(GetDlgItem(main_window, ID_TYPE_RESTORE_BASE), BM_CLICK, 0, 0);
        SendMessageW(GetDlgItem(main_window, ID_FIT_MAP), BM_CLICK, 0, 0);
        GetWindowTextW(status, status_after,
                       sizeof(status_after) / sizeof(status_after[0]));
        if (wcsstr(status_after, L"Remaining: 307 of 307"))
            fprintf(report, "PASS: per-type restore updated Livonia only and saved promptly\n");
        else { fprintf(report, "FAIL: Livonia per-type restore\n"); ++failures; }
        fprintf(report, "PASS: Fit Map control invoked\n");
    }
    if (viewport) {
        RECT client;
        double draw_size;
        double pan_x;
        double pan_y;
        GetClientRect(viewport, &client);
        draw_size = (double)(client.right < client.bottom ? client.right : client.bottom);
        pan_x = ((double)client.right - draw_size) * 0.5;
        pan_y = ((double)client.bottom - draw_size) * 0.5;
        color_marker_fit_x = (int)(pan_x + 335.646881 / 12800.0 * draw_size + 0.5);
        color_marker_fit_y = (int)(pan_y + (1.0 - 11288.389649 / 12800.0) *
                                   draw_size + 0.5);
        RedrawWindow(viewport, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
        if (marker_region_has_color(viewport, color_marker_fit_x,
                                    color_marker_fit_y, 'y'))
            fprintf(report, "PASS: old-format state leaves marker visually yellow\n");
        else { fprintf(report, "FAIL: normal marker yellow appearance\n"); ++failures; }

        send_marker_button(viewport, WM_RBUTTONDOWN, WM_RBUTTONUP, MK_RBUTTON,
                           color_marker_fit_x, color_marker_fit_y);
        if (file_contains_ascii(state_path, COLOR_MARKER_A_GREEN) &&
            marker_region_has_color(viewport, color_marker_fit_x,
                                    color_marker_fit_y, 'g'))
            fprintf(report, "PASS: RIGHT-click yellow marker became green visually and persistently\n");
        else { fprintf(report, "FAIL: RIGHT-click yellow to green\n"); ++failures; }
        if (IsWindowEnabled(GetDlgItem(main_window, ID_TYPE_RESTORE_BASE + 3)))
            fprintf(report, "PASS: colored marker enables its existing per-type Restore\n");
        else { fprintf(report, "FAIL: color-only per-type Restore availability\n"); ++failures; }
        SendMessageW(GetDlgItem(main_window, ID_TYPE_RESTORE_BASE + 3), BM_CLICK, 0, 0);
        RedrawWindow(viewport, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
        if (!file_contains_ascii(state_path, COLOR_MARKER_A_KEY) &&
            marker_region_has_color(viewport, color_marker_fit_x,
                                    color_marker_fit_y, 'y'))
            fprintf(report, "PASS: per-type Restore immediately and persistently reset green to yellow\n");
        else { fprintf(report, "FAIL: per-type Restore color reset\n"); ++failures; }

        send_marker_button(viewport, WM_RBUTTONDOWN, WM_RBUTTONUP, MK_RBUTTON,
                           color_marker_fit_x, color_marker_fit_y);
        send_marker_button(viewport, WM_RBUTTONDOWN, WM_RBUTTONUP, MK_RBUTTON,
                           color_marker_fit_x, color_marker_fit_y);
        if (!file_contains_ascii(state_path, COLOR_MARKER_A_GREEN) &&
            !file_contains_ascii(state_path, COLOR_MARKER_A_RED))
            fprintf(report, "PASS: RIGHT-click green marker toggled back to yellow\n");
        else {
            fprintf(report, "FAIL: RIGHT-click green to yellow (green=%d red=%d)\n",
                    file_contains_ascii(state_path, COLOR_MARKER_A_GREEN),
                    file_contains_ascii(state_path, COLOR_MARKER_A_RED));
            ++failures;
        }
        send_marker_button(viewport, WM_MBUTTONDOWN, WM_MBUTTONUP, MK_MBUTTON,
                           color_marker_fit_x, color_marker_fit_y);
        send_marker_button(viewport, WM_MBUTTONDOWN, WM_MBUTTONUP, MK_MBUTTON,
                           color_marker_fit_x, color_marker_fit_y);
        if (!file_contains_ascii(state_path, COLOR_MARKER_A_GREEN) &&
            !file_contains_ascii(state_path, COLOR_MARKER_A_RED))
            fprintf(report, "PASS: MIDDLE-click red marker toggled back to yellow\n");
        else {
            fprintf(report, "FAIL: MIDDLE-click red to yellow (green=%d red=%d)\n",
                    file_contains_ascii(state_path, COLOR_MARKER_A_GREEN),
                    file_contains_ascii(state_path, COLOR_MARKER_A_RED));
            ++failures;
        }

        send_marker_button(viewport, WM_MBUTTONDOWN, WM_MBUTTONUP, MK_MBUTTON,
                           color_marker_fit_x, color_marker_fit_y);
        send_marker_button(viewport, WM_RBUTTONDOWN, WM_RBUTTONUP, MK_RBUTTON,
                           color_marker_fit_x, color_marker_fit_y);
        if (file_contains_ascii(state_path, COLOR_MARKER_A_GREEN))
            fprintf(report, "PASS: RIGHT-click red marker changed it to green only\n");
        else { fprintf(report, "FAIL: RIGHT-click red to green\n"); ++failures; }
        send_marker_button(viewport, WM_MBUTTONDOWN, WM_MBUTTONUP, MK_MBUTTON,
                           color_marker_fit_x, color_marker_fit_y);
        if (file_contains_ascii(state_path, COLOR_MARKER_A_RED) &&
            marker_region_has_color(viewport, color_marker_fit_x,
                                    color_marker_fit_y, 'r'))
            fprintf(report, "PASS: MIDDLE-click green marker changed it to red only\n");
        else { fprintf(report, "FAIL: MIDDLE-click green to red\n"); ++failures; }

        send_marker_button(viewport, WM_MBUTTONDOWN, WM_MBUTTONUP, MK_MBUTTON,
                           livonia_marker_fit_x, livonia_marker_fit_y);
        send_marker_button(viewport, WM_RBUTTONDOWN, WM_RBUTTONUP, MK_RBUTTON,
                           livonia_marker_fit_x, livonia_marker_fit_y);
        if (file_contains_ascii(state_path, COLOR_MARKER_A_RED) &&
            file_contains_ascii(state_path, COLOR_MARKER_B_GREEN) &&
            marker_region_has_color(viewport, livonia_marker_fit_x,
                                    livonia_marker_fit_y, 'g'))
            fprintf(report, "PASS: two individual markers independently display and persist red/green\n");
        else { fprintf(report, "FAIL: independent red and green markers\n"); ++failures; }
        if (!file_contains_ascii(state_path, COLOR_MARKER_B_RED))
            fprintf(report, "PASS: one logical color state prevents simultaneous red and green\n");
        else { fprintf(report, "FAIL: conflicting marker colors persisted\n"); ++failures; }

        SendMessageW(GetDlgItem(main_window, ID_TYPE_CHECK_BASE + 3), BM_CLICK, 0, 0);
        SendMessageW(GetDlgItem(main_window, ID_TYPE_CHECK_BASE + 3), BM_CLICK, 0, 0);
        RedrawWindow(viewport, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
        if (file_contains_ascii(state_path, COLOR_MARKER_A_RED) &&
            marker_region_has_color(viewport, color_marker_fit_x,
                                    color_marker_fit_y, 'r'))
            fprintf(report, "PASS: filter disable/re-enable retained individual marker color\n");
        else { fprintf(report, "FAIL: filter retained marker color\n"); ++failures; }
        if (SendMessageW(viewport, WM_CONTEXTMENU, (WPARAM)viewport,
                         MAKELPARAM(color_marker_fit_x, color_marker_fit_y)) == 0)
            fprintf(report, "PASS: viewport suppresses unwanted RIGHT-click context menu\n");
        else { fprintf(report, "FAIL: RIGHT-click context-menu suppression\n"); ++failures; }
    }
    if (write_window_bmp(main_window, livonia_capture_path))
        fprintf(report, "PASS: captured rendered Livonia view to ui_smoke_livonia.bmp\n");
    else { fprintf(report, "FAIL: Livonia window capture\n"); ++failures; }
    if (combo && viewport && status && first_filter) {
        static const wchar_t *new_map_names[] = {
            L"Sakhal", L"Namalsk", L"Deer Isle", L"Bitterroot", L"Banov"
        };
        static const unsigned int new_map_totals[] = { 431u, 554u, 190u, 99u, 239u };
        int selection;
        for (selection = 2; selection < 7; ++selection) {
            wchar_t expected[96];
            SendMessageW(combo, CB_SETCURSEL, (WPARAM)selection, 0);
            SendMessageW(main_window, WM_COMMAND,
                         MAKEWPARAM(ID_MAP_COMBO, CBN_SELCHANGE), (LPARAM)combo);
            RedrawWindow(viewport, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
            GetWindowTextW(status, status_after,
                           sizeof(status_after) / sizeof(status_after[0]));
            _snwprintf(expected, sizeof(expected) / sizeof(expected[0]),
                       L"Remaining: %u of %u", new_map_totals[selection - 2],
                       new_map_totals[selection - 2]);
            if (SendMessageW(combo, CB_GETCURSEL, 0, 0) == selection &&
                wcsstr(status_after, new_map_names[selection - 2]) &&
                wcsstr(status_after, expected)) {
                fprintf(report, "PASS: switched to and rendered added map %ls\n",
                        new_map_names[selection - 2]);
            } else {
                fprintf(report, "FAIL: added map switch/render for %ls\n",
                        new_map_names[selection - 2]);
                ++failures;
            }
            if (selection == 2) {
                wchar_t jana_text[128];
                wchar_t bitrak_text[128];
                HWND jana_filter = GetDlgItem(main_window, ID_TYPE_CHECK_BASE + 7);
                HWND bitrak_filter = GetDlgItem(main_window, ID_TYPE_CHECK_BASE + 8);
                GetWindowTextW(jana_filter, jana_text,
                               sizeof(jana_text) / sizeof(jana_text[0]));
                GetWindowTextW(bitrak_filter, bitrak_text,
                               sizeof(bitrak_text) / sizeof(bitrak_text[0]));
                if (IsWindowVisible(jana_filter) && IsWindowVisible(bitrak_filter) &&
                    wcsstr(jana_text, L"70/70") && wcsstr(bitrak_text, L"70/70"))
                    fprintf(report, "PASS: Sakhal exposes both two-wheeled spawn sets\n");
                else { fprintf(report, "FAIL: Sakhal two-wheeled spawn sets\n"); ++failures; }
            }
            SendMessageW(first_filter, BM_CLICK, 0, 0);
            if (SendMessageW(first_filter, BM_GETCHECK, 0, 0) == BST_UNCHECKED)
                fprintf(report, "PASS: %ls vehicle filter toggled independently\n",
                        new_map_names[selection - 2]);
            else {
                fprintf(report, "FAIL: %ls vehicle filter toggle\n",
                        new_map_names[selection - 2]);
                ++failures;
            }
            SendMessageW(first_filter, BM_CLICK, 0, 0);
        }
    }
    if (combo) {
        SendMessageW(combo, CB_SETCURSEL, 0, 0);
        SendMessageW(main_window, WM_COMMAND, MAKEWPARAM(ID_MAP_COMBO, CBN_SELCHANGE),
                     (LPARAM)combo);
        Sleep(500);
        if (SendMessageW(combo, CB_GETCURSEL, 0, 0) == 0)
            fprintf(report, "PASS: switched back to Chernarus with independent view state\n");
        else { fprintf(report, "FAIL: Chernarus switch\n"); ++failures; }
        if (status) {
            GetWindowTextW(status, status_after,
                           sizeof(status_after) / sizeof(status_after[0]));
            if (wcsstr(status_after, L"Remaining: 648 of 649"))
                fprintf(report, "PASS: Livonia restore did not alter Chernarus\n");
            else { fprintf(report, "FAIL: restore crossed map boundary\n"); ++failures; }
        }
    }
    if (write_window_bmp(main_window, capture_path))
        fprintf(report, "PASS: captured rendered native window to ui_smoke.bmp\n");
    else { fprintf(report, "FAIL: native window capture\n"); ++failures; }
    PostMessageW(main_window, WM_CLOSE, 0, 0);
    if (!wait_for_exit(process.hProcess, 10000)) {
        fprintf(report, "FAIL: clean application shutdown\n");
        ++failures;
        TerminateProcess(process.hProcess, 2);
    } else {
        fprintf(report, "PASS: clean application shutdown\n");
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    ZeroMemory(&process, sizeof(process));

    ZeroMemory(&startup, sizeof(startup));
    startup.cb = sizeof(startup);
    if (!CreateProcessW(executable_path, process_command, NULL, NULL, FALSE, 0,
                        NULL, windows_directory, &startup, &process)) {
        fprintf(report, "FAIL: persistence relaunch\n");
        ++failures;
        goto finished;
    }
    WaitForInputIdle(process.hProcess, 15000);
    dialog = FindWindowW(L"#32770", L"Required yellow marker missing");
    main_window = dialog ? GetWindow(dialog, GW_OWNER) : NULL;
    if (dialog) {
        HWND ok_button = GetDlgItem(dialog, IDOK);
        if (ok_button) SendMessageW(ok_button, BM_CLICK, 0, 0);
    }
    if (!main_window)
        main_window = wait_for_window(L"DayZVehicleMapMainWindow", L"DayZ Vehicle Map", 15000);
    status = main_window ? GetDlgItem(main_window, ID_STATUS) : NULL;
    if (status) {
        GetWindowTextW(status, status_after,
                       sizeof(status_after) / sizeof(status_after[0]));
        if (wcsstr(status_after, L"Remaining: 648 of 649"))
            fprintf(report, "PASS: checked state survived close and reopen\n");
        else { fprintf(report, "FAIL: checked state did not survive reopen\n"); ++failures; }
        combo = GetDlgItem(main_window, ID_MAP_COMBO);
        viewport = GetDlgItem(main_window, ID_VIEWPORT);
        SendMessageW(combo, CB_SETCURSEL, 1, 0);
        SendMessageW(main_window, WM_COMMAND, MAKEWPARAM(ID_MAP_COMBO, CBN_SELCHANGE),
                     (LPARAM)combo);
        RedrawWindow(viewport, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
        if (marker_region_has_color(viewport, color_marker_fit_x,
                                    color_marker_fit_y, 'r') &&
            marker_region_has_color(viewport, livonia_marker_fit_x,
                                    livonia_marker_fit_y, 'g'))
            fprintf(report, "PASS: red and green marker colors survived close and reopen\n");
        else { fprintf(report, "FAIL: marker colors after persistence relaunch\n"); ++failures; }
        send_marker_button(viewport, WM_MBUTTONDOWN, WM_MBUTTONUP, MK_MBUTTON,
                           color_marker_fit_x, color_marker_fit_y);
        send_marker_button(viewport, WM_RBUTTONDOWN, WM_RBUTTONUP, MK_RBUTTON,
                           livonia_marker_fit_x, livonia_marker_fit_y);
        if (!file_contains_ascii(state_path, COLOR_MARKER_A_GREEN) &&
            !file_contains_ascii(state_path, COLOR_MARKER_A_RED) &&
            !file_contains_ascii(state_path, COLOR_MARKER_B_GREEN) &&
            !file_contains_ascii(state_path, COLOR_MARKER_B_RED))
            fprintf(report, "PASS: persisted red/green states retained their toggle semantics\n");
        else {
            fprintf(report,
                    "FAIL: persisted color toggle semantics (ag=%d ar=%d bg=%d br=%d)\n",
                    file_contains_ascii(state_path, COLOR_MARKER_A_GREEN),
                    file_contains_ascii(state_path, COLOR_MARKER_A_RED),
                    file_contains_ascii(state_path, COLOR_MARKER_B_GREEN),
                    file_contains_ascii(state_path, COLOR_MARKER_B_RED));
            ++failures;
        }

        send_marker_button(viewport, WM_RBUTTONDOWN, WM_RBUTTONUP, MK_RBUTTON,
                           color_marker_fit_x, color_marker_fit_y);
        send_marker_button(viewport, WM_MBUTTONDOWN, WM_MBUTTONUP, MK_MBUTTON,
                           livonia_marker_fit_x, livonia_marker_fit_y);
        SendMessageW(GetDlgItem(main_window, ID_RESTORE_ALL), BM_CLICK, 0, 0);
        RedrawWindow(viewport, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
        if (!file_contains_ascii(state_path, COLOR_MARKER_A_KEY) &&
            !file_contains_ascii(state_path, COLOR_MARKER_B_KEY) &&
            marker_region_has_color(viewport, color_marker_fit_x,
                                    color_marker_fit_y, 'y') &&
            marker_region_has_color(viewport, livonia_marker_fit_x,
                                    livonia_marker_fit_y, 'y'))
            fprintf(report, "PASS: Restore All cleared Livonia colors in memory and persistence\n");
        else { fprintf(report, "FAIL: Restore All marker color reset\n"); ++failures; }

        SendMessageW(combo, CB_SETCURSEL, 0, 0);
        SendMessageW(main_window, WM_COMMAND, MAKEWPARAM(ID_MAP_COMBO, CBN_SELCHANGE),
                     (LPARAM)combo);
        GetWindowTextW(status, status_after,
                       sizeof(status_after) / sizeof(status_after[0]));
        if (wcsstr(status_after, L"Remaining: 648 of 649"))
            fprintf(report, "PASS: Livonia color Restore did not alter Chernarus state\n");
        else { fprintf(report, "FAIL: color Restore crossed map boundary\n"); ++failures; }
        SendMessageW(GetDlgItem(main_window, ID_RESTORE_ALL), BM_CLICK, 0, 0);
        GetWindowTextW(status, status_after,
                       sizeof(status_after) / sizeof(status_after[0]));
        if (wcsstr(status_after, L"Remaining: 649 of 649"))
            fprintf(report, "PASS: Restore All restored only current Chernarus state\n");
        else { fprintf(report, "FAIL: Chernarus Restore All after reopen\n"); ++failures; }
        PostMessageW(main_window, WM_CLOSE, 0, 0);
    } else {
        fprintf(report, "FAIL: relaunched native window not found\n");
        ++failures;
    }
    if (!wait_for_exit(process.hProcess, 10000)) {
        TerminateProcess(process.hProcess, 2);
        fprintf(report, "FAIL: persistence relaunch clean shutdown\n");
        ++failures;
    } else {
        fprintf(report, "PASS: persistence relaunch clean shutdown\n");
    }

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    ZeroMemory(&process, sizeof(process));
    ZeroMemory(&startup, sizeof(startup));
    startup.cb = sizeof(startup);
    if (!CreateProcessW(executable_path, process_command, NULL, NULL, FALSE, 0,
                        NULL, windows_directory, &startup, &process)) {
        fprintf(report, "FAIL: post-Restore persistence relaunch\n");
        ++failures;
        goto finished;
    }
    WaitForInputIdle(process.hProcess, 15000);
    main_window = wait_for_window(L"DayZVehicleMapMainWindow", L"DayZ Vehicle Map", 15000);
    combo = main_window ? GetDlgItem(main_window, ID_MAP_COMBO) : NULL;
    viewport = main_window ? GetDlgItem(main_window, ID_VIEWPORT) : NULL;
    if (main_window && combo && viewport) {
        SendMessageW(combo, CB_SETCURSEL, 1, 0);
        SendMessageW(main_window, WM_COMMAND, MAKEWPARAM(ID_MAP_COMBO, CBN_SELCHANGE),
                     (LPARAM)combo);
        RedrawWindow(viewport, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
        if (marker_region_has_color(viewport, color_marker_fit_x,
                                    color_marker_fit_y, 'y') &&
            marker_region_has_color(viewport, livonia_marker_fit_x,
                                    livonia_marker_fit_y, 'y'))
            fprintf(report, "PASS: restored markers remained yellow after another restart\n");
        else { fprintf(report, "FAIL: stale colors returned after Restore restart\n"); ++failures; }
        send_marker_button(viewport, WM_RBUTTONDOWN, WM_RBUTTONUP, MK_RBUTTON,
                           color_marker_fit_x, color_marker_fit_y);
        send_marker_button(viewport, WM_MBUTTONDOWN, WM_MBUTTONUP, MK_MBUTTON,
                           livonia_marker_fit_x, livonia_marker_fit_y);
        if (file_contains_ascii(state_path, COLOR_MARKER_A_KEY) &&
            file_contains_ascii(state_path, COLOR_MARKER_B_KEY))
            fprintf(report, "PASS: markers can be colored again after Restore\n");
        else { fprintf(report, "FAIL: recoloring after Restore\n"); ++failures; }
        SendMessageW(GetDlgItem(main_window, ID_RESTORE_ALL), BM_CLICK, 0, 0);
        PostMessageW(main_window, WM_CLOSE, 0, 0);
    } else {
        fprintf(report, "FAIL: post-Restore native window not found\n");
        ++failures;
    }
    if (!wait_for_exit(process.hProcess, 10000)) {
        TerminateProcess(process.hProcess, 2);
        fprintf(report, "FAIL: post-Restore relaunch clean shutdown\n");
        ++failures;
    } else {
        fprintf(report, "PASS: post-Restore relaunch clean shutdown\n");
    }

finished:
    if (process.hProcess && WaitForSingleObject(process.hProcess, 0) == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 2);
        WaitForSingleObject(process.hProcess, 2000);
    }
    if (process.hThread) CloseHandle(process.hThread);
    if (process.hProcess) CloseHandle(process.hProcess);
    if (had_state_backup) {
        if (copy_file_with_retry(state_backup_path, state_path)) {
            DeleteFileW(state_backup_path);
            fprintf(report, "PASS: pre-test checked_state.txt restored\n");
        } else {
            fprintf(report, "FAIL: could not restore pre-test state; backup retained\n");
            ++failures;
        }
    } else {
        DeleteFileW(state_path);
        DeleteFileW(state_backup_path);
        fprintf(report, "PASS: no persistence fixture left behind\n");
    }
    fprintf(report, "RESULT: %s (%d failure%s)\n",
            failures == 0 ? "PASS" : "FAIL", failures, failures == 1 ? "" : "s");
    fclose(report);
    return failures == 0 ? 0 : 2;
}
