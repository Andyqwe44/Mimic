/**
 * capture_pw.cpp — FFI: PrintWindow capture method.
 * Uses PW_RENDERFULLCONTENT | PW_CLIENTONLY for client area.
 * Magenta sentinel detects when PrintWindow doesn't render.
 * DPI awareness context ensures correct rendering on high-DPI.
 */
#include "capture_methods.h"
#include "capture_internal.h"
#include <cstring>
#include <vector>

int capture_printwindow(HWND hwnd, uint8_t* buf, int buf_size, int* w, int* h) {
    DpiGuard dpi(hwnd);

    RECT cr;
    if (!GetClientRect(hwnd, &cr)) return 0;
    *w = cr.right - cr.left;
    *h = cr.bottom - cr.top;
    if (*w <= 0 || *h <= 0) return 0;

    HDC sdc = GetDC(nullptr);
    if (!sdc) return 0;
    HDC mdc = CreateCompatibleDC(sdc);
    if (!mdc) { ReleaseDC(nullptr, sdc); return 0; }
    HBITMAP bmp = CreateCompatibleBitmap(sdc, *w, *h);
    if (!bmp) { DeleteDC(mdc); ReleaseDC(nullptr, sdc); return 0; }

    HGDIOBJ old = SelectObject(mdc, bmp);
    // Fill with magenta sentinel to detect PrintWindow not drawing
    RECT fill_r = {0, 0, *w, *h};
    HBRUSH brush = CreateSolidBrush(RGB(255, 0, 255));
    FillRect(mdc, &fill_r, brush);
    DeleteObject(brush);

    int pw_ok = PrintWindow(hwnd, mdc, PW_RENDERFULLCONTENT | PW_CLIENTONLY);

    BITMAPINFOHEADER bi = {};
    bi.biSize = sizeof(BITMAPINFOHEADER);
    bi.biWidth = *w; bi.biHeight = -*h; bi.biPlanes = 1;
    bi.biBitCount = 32; bi.biCompression = BI_RGB;

    std::vector<uint8_t> pixels(*w * *h * 4);
    int copied = GetDIBits(mdc, bmp, 0, *h, pixels.data(),
                           (BITMAPINFO*)&bi, DIB_RGB_COLORS);
    SelectObject(mdc, old); DeleteObject(bmp); DeleteDC(mdc); ReleaseDC(nullptr, sdc);

    if (!copied || !pw_ok) return 0;
    if (capture_is_solid_color(pixels.data(), (int)pixels.size())) return 0;
    if (capture_has_magenta(pixels.data(), (int)pixels.size())) return 0;

    int needed = (int)pixels.size();
    if (needed > buf_size) return 0;
    memcpy(buf, pixels.data(), needed);
    return needed;
}
