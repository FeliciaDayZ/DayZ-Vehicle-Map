#include <windows.h>
#include <gdiplus/gdiplus.h>
#include <stdio.h>
#include <urlmon.h>

#define TILE_COUNT 8
#define TILE_SIZE 256
#define MAP_PIXELS (TILE_COUNT * TILE_SIZE)

static const wchar_t *const TILE_URL =
    L"https://maps.izurvive.com/maps/ChernarusPlus-Sat/1.29.0/tiles/3/%d/%d.jpg";

static BOOL download_tiles(const wchar_t *directory)
{
    int x;
    int y;

    if (!CreateDirectoryW(directory, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        fwprintf(stderr, L"Could not create %ls (error %lu).\n",
                 directory, (unsigned long)GetLastError());
        return FALSE;
    }

    for (y = 0; y < TILE_COUNT; ++y) {
        for (x = 0; x < TILE_COUNT; ++x) {
            wchar_t url[512];
            wchar_t path[MAX_PATH];
            HRESULT result;

            _snwprintf(url, sizeof(url) / sizeof(url[0]), TILE_URL, x, y);
            url[(sizeof(url) / sizeof(url[0])) - 1u] = L'\0';
            _snwprintf(path, sizeof(path) / sizeof(path[0]),
                       L"%ls\\tile_%d_%d.jpg", directory, x, y);
            path[(sizeof(path) / sizeof(path[0])) - 1u] = L'\0';
            result = URLDownloadToFileW(NULL, url, path, 0, NULL);
            if (FAILED(result)) {
                fwprintf(stderr, L"Download failed for %ls (HRESULT 0x%08lx).\n",
                         url, (unsigned long)result);
                return FALSE;
            }
        }
    }
    return TRUE;
}

static BOOL save_png(GpBitmap *bitmap, const wchar_t *path)
{
    const CLSID png_encoder = {
        0x557cf406u, 0x1a04u, 0x11d3u,
        { 0x9au, 0x73u, 0x00u, 0x00u, 0xf8u, 0x1eu, 0xf3u, 0x2eu }
    };
    GpStatus status = GdipSaveImageToFile((GpImage *)bitmap, path, &png_encoder, NULL);
    if (status != Ok) {
        fwprintf(stderr, L"Could not save %ls (GDI+ status %d).\n", path, status);
        return FALSE;
    }
    return TRUE;
}

static BOOL build_map(const wchar_t *tile_directory, const wchar_t *output_path)
{
    const REAL map_coverage = 0.9644165039f;
    const REAL covered_pixels = (REAL)MAP_PIXELS * map_coverage;
    const REAL top_padding = (REAL)MAP_PIXELS - covered_pixels;
    GpBitmap *mosaic = NULL;
    GpBitmap *output = NULL;
    GpGraphics *graphics = NULL;
    GpStatus status;
    BOOL success = FALSE;
    int x;
    int y;

    status = GdipCreateBitmapFromScan0(MAP_PIXELS, MAP_PIXELS, 0,
                                       PixelFormat24bppRGB, NULL, &mosaic);
    if (status != Ok || !mosaic ||
        GdipGetImageGraphicsContext(mosaic, &graphics) != Ok) {
        fwprintf(stderr, L"Could not create the source mosaic.\n");
        goto cleanup;
    }
    GdipSetCompositingMode(graphics, CompositingModeSourceCopy);

    for (y = 0; y < TILE_COUNT; ++y) {
        for (x = 0; x < TILE_COUNT; ++x) {
            wchar_t path[MAX_PATH];
            GpImage *tile = NULL;

            _snwprintf(path, sizeof(path) / sizeof(path[0]),
                       L"%ls\\tile_%d_%d.jpg", tile_directory, x, y);
            path[(sizeof(path) / sizeof(path[0])) - 1u] = L'\0';
            if (GdipLoadImageFromFile(path, &tile) != Ok || !tile) {
                fwprintf(stderr, L"Could not load %ls.\n", path);
                goto cleanup;
            }
            status = GdipDrawImageRectI(graphics, tile,
                                        x * TILE_SIZE, y * TILE_SIZE,
                                        TILE_SIZE, TILE_SIZE);
            GdipDisposeImage(tile);
            if (status != Ok) {
                fwprintf(stderr, L"Could not draw %ls.\n", path);
                goto cleanup;
            }
        }
    }
    GdipDeleteGraphics(graphics);
    graphics = NULL;

    status = GdipCreateBitmapFromScan0(MAP_PIXELS, MAP_PIXELS, 0,
                                       PixelFormat24bppRGB, NULL, &output);
    if (status != Ok || !output ||
        GdipGetImageGraphicsContext(output, &graphics) != Ok) {
        fwprintf(stderr, L"Could not create the output bitmap.\n");
        goto cleanup;
    }

    GdipSetCompositingMode(graphics, CompositingModeSourceCopy);
    GdipSetCompositingQuality(graphics, CompositingQualityHighQuality);
    GdipSetInterpolationMode(graphics, InterpolationModeHighQualityBilinear);
    GdipSetPixelOffsetMode(graphics, PixelOffsetModeHalf);
    status = GdipDrawImageRectRect(graphics, (GpImage *)mosaic,
                                   0.0f, 0.0f,
                                   (REAL)MAP_PIXELS, (REAL)MAP_PIXELS,
                                   0.0f, top_padding,
                                   covered_pixels, covered_pixels,
                                   UnitPixel, NULL, NULL, NULL);
    if (status != Ok || !save_png(output, output_path)) goto cleanup;

    success = TRUE;

cleanup:
    if (graphics) GdipDeleteGraphics(graphics);
    if (output) GdipDisposeImage((GpImage *)output);
    if (mosaic) GdipDisposeImage((GpImage *)mosaic);
    return success;
}

int wmain(int argc, wchar_t **argv)
{
    const wchar_t *output_path = argc > 1
        ? argv[1]
        : L"build\\chernarusplus.corrected.png";
    const wchar_t *tile_directory = L"build\\chernarus_tiles";
    GdiplusStartupInput input;
    ULONG_PTR token = 0;
    BOOL success;

    ZeroMemory(&input, sizeof(input));
    input.GdiplusVersion = 1;
    if (GdiplusStartup(&token, &input, NULL) != Ok) {
        fwprintf(stderr, L"Could not start GDI+.\n");
        return 1;
    }

    success = download_tiles(tile_directory) &&
              build_map(tile_directory, output_path);
    GdiplusShutdown(token);
    if (!success) return 1;

    wprintf(L"Built calibrated Chernarus map: %ls\n", output_path);
    return 0;
}
