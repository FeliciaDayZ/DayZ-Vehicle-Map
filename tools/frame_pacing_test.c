#define wWinMain dvm_application_entry
#include "../src/main.c"
#undef wWinMain

typedef struct FrameStats {
    LONGLONG previous;
    unsigned int frames;
    double minimum_ms;
} FrameStats;

static void observe_frame(const App *app, FrameStats *stats)
{
    LONGLONG finished = app->viewport_frame_finished.QuadPart;
    if (!finished || finished == stats->previous) return;
    if (stats->previous) {
        double elapsed = (double)(finished - stats->previous) * 1000.0 /
                         (double)app->viewport_clock_frequency.QuadPart;
        if (elapsed < stats->minimum_ms) stats->minimum_ms = elapsed;
    }
    stats->previous = finished;
    ++stats->frames;
}

static void pump_messages(App *app, FrameStats *stats)
{
    MSG message;
    while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
        observe_frame(app, stats);
    }
}

static void settle(App *app, FrameStats *stats, DWORD milliseconds)
{
    ULONGLONG end = GetTickCount64() + milliseconds;
    while (GetTickCount64() < end) {
        pump_messages(app, stats);
        MsgWaitForMultipleObjectsEx(0, NULL, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    pump_messages(app, stats);
}

static void force_paint(App *app, FrameStats *stats)
{
    RedrawWindow(app->viewport, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
    observe_frame(app, stats);
}

int main(void)
{
    App app;
    FrameStats stats = { 0, 0, 1000000.0 };
    GdiplusStartupInput input;
    ULONG_PTR token = 0;
    INITCOMMONCONTROLSEX controls = { sizeof(controls), ICC_STANDARD_CLASSES };
    HWND window;
    unsigned int before;
    ULONGLONG end;
    int step = 0;
    int failures = 0;
    ZeroMemory(&app, sizeof(app));
    ZeroMemory(&input, sizeof(input));
    input.GdiplusVersion = 1;
    if (GdiplusStartup(&token, &input, NULL) != Ok) return 2;
    app.instance = GetModuleHandleW(NULL);
    app.views[0].map = dvm_map_info(0);
    app.views[0].image_width = app.views[0].image_height = 128u;
    InitCommonControlsEx(&controls);
    enable_dpi_awareness();
    if (!register_window_classes(app.instance)) return 2;
    window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        MAIN_CLASS_NAME, L"Frame pacing test", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        -30000, -30000, 800, 600, NULL, NULL, app.instance, &app);
    if (!window) return 2;
    ShowWindow(window, SW_SHOWNOACTIVATE);
    force_paint(&app, &stats);
    settle(&app, &stats, 150);
    test_check(stdout, app.viewport_buffer_valid && stats.frames > 0,
               "initial frame rendered", &failures);

    before = stats.frames;
    end = GetTickCount64() + 200;
    while (GetTickCount64() < end) {
        force_paint(&app, &stats);
        pump_messages(&app, &stats);
        Sleep(1);
    }
    test_check(stdout, stats.frames == before && !app.viewport_frame_pending,
               "unchanged forced paints reuse the cached frame", &failures);

    before = stats.frames;
    end = GetTickCount64() + 650;
    SendMessageW(app.viewport, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(200, 200));
    while (GetTickCount64() < end) {
        ++step;
        SendMessageW(app.viewport, WM_MOUSEMOVE, MK_LBUTTON,
                     MAKELPARAM(200 + step % 40, 200 + step % 30));
        SendMessageW(app.viewport, WM_MOUSEWHEEL,
                     MAKEWPARAM(0, step & 1 ? WHEEL_DELTA : -WHEEL_DELTA), 0);
        force_paint(&app, &stats);
        pump_messages(&app, &stats);
        Sleep(1);
    }
    SendMessageW(app.viewport, WM_LBUTTONUP, 0, MAKELPARAM(200, 200));
    settle(&app, &stats, 250);
    test_check(stdout, stats.frames > before + 1 && !app.zooming &&
               !app.viewport_dirty && !app.viewport_frame_pending,
               "rapid pan/zoom coalesces and the final quality frame arrives",
               &failures);

    before = stats.frames;
    end = GetTickCount64() + 400;
    while (GetTickCount64() < end) {
        ++step;
        SetWindowPos(window, NULL, 0, 0, 800 + step % 40, 600 + step % 30,
                     SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER);
        observe_frame(&app, &stats);
        force_paint(&app, &stats);
        pump_messages(&app, &stats);
        Sleep(1);
    }
    settle(&app, &stats, 150);
    {
        RECT client;
        GetClientRect(app.viewport, &client);
        test_check(stdout, stats.frames > before && !app.viewport_dirty &&
                   app.viewport_buffer_width == client.right &&
                   app.viewport_buffer_height == client.bottom,
                   "resize paints are capped and the final size is rendered", &failures);
    }

    request_viewport_frame(&app);
    force_paint(&app, &stats);
    request_viewport_frame(&app);
    ShowWindow(window, SW_MINIMIZE);
    before = stats.frames;
    force_paint(&app, &stats);
    settle(&app, &stats, 200);
    test_check(stdout, stats.frames == before && app.viewport_dirty &&
               !app.viewport_frame_pending,
               "minimized window retains pending changes without rendering", &failures);
    ShowWindow(window, SW_SHOWNOACTIVATE);
    settle(&app, &stats, 200);
    test_check(stdout, stats.frames > before && !app.viewport_dirty &&
               !app.viewport_frame_pending,
               "restore renders the pending changes", &failures);

    before = stats.frames;
    ShowWindow(window, SW_HIDE);
    request_viewport_frame(&app);
    force_paint(&app, &stats);
    settle(&app, &stats, 150);
    test_check(stdout, stats.frames == before && !app.viewport_frame_pending,
               "hidden window does not render or keep a frame timer", &failures);
    ShowWindow(window, SW_SHOWNOACTIVATE);
    settle(&app, &stats, 150);
    before = stats.frames;
    settle(&app, &stats, 200);
    test_check(stdout, stats.frames == before && !app.viewport_frame_pending &&
               !app.status_update_pending,
               "idle window stops rendering and update timers", &failures);
    printf("INFO: %u frames; shortest interval %.3f ms\n", stats.frames, stats.minimum_ms);
    test_check(stdout, stats.minimum_ms >= 40.0,
               "all rendered frames are at least 40 ms apart (maximum 25 FPS)", &failures);

    SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    DestroyWindow(window);
    if (app.ui_font) DeleteObject(app.ui_font);
    cleanup_app(&app);
    GdiplusShutdown(token);
    printf("RESULT: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
