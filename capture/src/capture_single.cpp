/**
 * capture_single.exe — single-frame screenshot, raw BGRA pixels to stdout
 *
 * Usage:   capture_single.exe <hwnd>
 *          hwnd=0  → full desktop (DXGI GPU-accelerated)
 *          hwnd≠0  → specific window (PrintWindow → DXGI crop → GDI fallback)
 *
 * Binary output format (LE): [w:4][h:4][ch:4][BGRA pixels...]
 * Debug info on stderr.
 */
#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#define NOMINMAX
#include <windows.h>
#include <dwmapi.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <io.h>
#include <fcntl.h>

#include "capture.hpp"
#include "../../common/include/capture_helpers.hpp"
namespace ch = capture_helpers;

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

// ── PrintWindow with magenta sentinel ─────────────────────
static bool print_window_cap(HWND hwnd, std::vector<uint8_t>& pixels, int w, int h) {
    HDC screen = GetDC(nullptr);
    if (!screen) return false;

    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HBITMAP old = (HBITMAP)SelectObject(mem, bmp);

    RECT fill = {0, 0, w, h};
    HBRUSH magenta = CreateSolidBrush(RGB(255, 0, 255));
    FillRect(mem, &fill, magenta);
    DeleteObject(magenta);

    PrintWindow(hwnd, mem, PW_RENDERFULLCONTENT | PW_CLIENTONLY);

    BITMAPINFOHEADER bi = {};
    bi.biSize = sizeof(bi); bi.biWidth = w; bi.biHeight = -h;
    bi.biPlanes = 1; bi.biBitCount = 32; bi.biCompression = BI_RGB;

    pixels.resize(w * h * 4);
    GetDIBits(mem, bmp, 0, h, pixels.data(), (BITMAPINFO*)&bi, DIB_RGB_COLORS);

    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);

    // Reject solid color or magenta sentinel using shared helpers
    if (ch::is_solid_color(pixels.data(), pixels.size())) return false;
    if (ch::has_magenta_sentinel(pixels.data(), pixels.size())) return false;
    return true;
}

// ── DXGI crop to window rect ──────────────────────────────
static bool dxgi_crop(HWND hwnd, const RECT& r, std::vector<uint8_t>& pixels, int& w, int& h) {
    auto backend = create_capture_backend();
    if (!backend) return false;

    FrameBuffer fb;
    if (!backend->capture(fb)) return false;

    int cx = r.left > 0 ? r.left : 0;
    int cy = r.top > 0 ? r.top : 0;
    int cw = (r.right - r.left) < (fb.width - cx) ? (r.right - r.left) : (fb.width - cx);
    int ch = (r.bottom - r.top) < (fb.height - cy) ? (r.bottom - r.top) : (fb.height - cy);
    if (cw <= 0 || ch <= 0) return false;

    pixels.resize(cw * ch * 4);
    for (int y = 0; y < ch; y++) {
        int si = ((cy + y) * fb.width + cx) * 4;
        memcpy(pixels.data() + y * cw * 4, fb.data.data() + si, cw * 4);
    }
    w = cw; h = ch;
    return true;
}

// ── Window capture (PrintWindow → DXGI crop → GDI GetWindowDC) ──
static bool win_capture(HWND hwnd, std::vector<uint8_t>& pixels, int& w, int& h) {
    // Get window rect
    RECT r = {};
    DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r));
    if (r.right - r.left <= 0 || r.bottom - r.top <= 0) {
        if (!GetWindowRect(hwnd, &r)) return false;
    }
    w = r.right - r.left;
    h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return false;

    fprintf(stderr, "[capture] win_capture hwnd=%p %dx%d\n", hwnd, w, h);

    // 1) Try PrintWindow
    if (print_window_cap(hwnd, pixels, w, h)) {
        fprintf(stderr, "[capture] PrintWindow OK %dx%d\n", w, h);
        return true;
    }

    // 2) Try DXGI crop
    fprintf(stderr, "[capture] PrintWindow failed → DXGI crop fallback\n");
    if (dxgi_crop(hwnd, r, pixels, w, h)) {
        fprintf(stderr, "[capture] DXGI crop OK %dx%d\n", w, h);
        return true;
    }

    // 3) Last resort: GDI GetWindowDC
    fprintf(stderr, "[capture] DXGI crop failed → GDI GetWindowDC\n");
    HDC dc = GetWindowDC(hwnd);
    if (!dc) return false;
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
    HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, dc, 0, 0, SRCCOPY);
    BITMAPINFOHEADER bi = {};
    bi.biSize = sizeof(bi); bi.biWidth = w; bi.biHeight = -h;
    bi.biPlanes = 1; bi.biBitCount = 32; bi.biCompression = BI_RGB;
    pixels.resize(w * h * 4);
    GetDIBits(mem, bmp, 0, h, pixels.data(), (BITMAPINFO*)&bi, DIB_RGB_COLORS);
    SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem);
    ReleaseDC(hwnd, dc);
    fprintf(stderr, "[capture] GDI GetWindowDC OK %dx%d\n", w, h);
    return true;
}

// ── main ────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    _setmode(_fileno(stdout), _O_BINARY);

    HWND hwnd = (HWND)0;
    if (argc > 1) hwnd = (HWND)(ULONG_PTR)_strtoui64(argv[1], nullptr, 10);

    std::vector<uint8_t> pixels;
    int w = 0, h = 0;
    bool ok = false;

    if (hwnd == 0 || hwnd == GetDesktopWindow()) {
        // Desktop: use shared DxgiCapture → GdiCapture fallback
        auto backend = create_capture_backend();
        if (backend) {
            FrameBuffer fb;
            ok = backend->capture(fb);
            if (ok) {
                pixels = std::move(fb.data);
                w = fb.width; h = fb.height;
                fprintf(stderr, "[capture] %s: %dx%d OK\n", backend->name(), w, h);
            }
        }
    } else {
        ok = win_capture(hwnd, pixels, w, h);
    }

    if (!ok || w <= 0 || h <= 0) {
        fprintf(stderr, "[capture] FAILED: ok=%d w=%d h=%d\n", (int)ok, w, h);
        return 1;
    }

    fprintf(stderr, "[capture] output: %dx%d %zu bytes\n", w, h, pixels.size());
    uint8_t buf[12];
    ch::w32_le(buf, (uint32_t)w);
    ch::w32_le(buf + 4, (uint32_t)h);
    ch::w32_le(buf + 8, 4);
    fwrite(buf, 1, 12, stdout);
    fwrite(pixels.data(), 1, pixels.size(), stdout);
    fflush(stdout);
    return 0;
}
