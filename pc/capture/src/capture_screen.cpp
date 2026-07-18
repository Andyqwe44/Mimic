/**
 * capture_screen.cpp — FFI: ScreenBitBlt capture method.
 * Captures window area from the virtual screen (supports multi-monitor).
 * Uses virtual screen DC instead of GetDC(nullptr) to handle negative
 * coordinates and windows spanning multiple monitors correctly.
 */
#include "capture_methods.h"
#include "capture_internal.h"
#include <cstring>
#include <vector>

int capture_screen_bitblt(HWND hwnd, uint8_t* buf, int buf_size, int* w, int* h) {
    DpiGuard dpi(hwnd);

    RECT wr;
    if (!GetWindowRect(hwnd, &wr)) return 0;
    *w = wr.right - wr.left;
    *h = wr.bottom - wr.top;
    if (*w <= 0 || *h <= 0) return 0;

    // Use virtual screen DC (covers ALL monitors, handles negative coordinates)
    HDC sc = create_virtual_screen_dc();
    if (!sc) return 0;

    // Get virtual screen origin (may be negative on multi-monitor)
    int vs_x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vs_y = GetSystemMetrics(SM_YVIRTUALSCREEN);

    // Window coordinates relative to virtual screen origin
    int src_x = wr.left - vs_x;
    int src_y = wr.top - vs_y;

    // Clamp to virtual screen bounds
    int vs_w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vs_h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (src_x < 0) { *w += src_x; src_x = 0; }
    if (src_y < 0) { *h += src_y; src_y = 0; }
    if (src_x + *w > vs_w) *w = vs_w - src_x;
    if (src_y + *h > vs_h) *h = vs_h - src_y;
    if (*w <= 0 || *h <= 0) { DeleteDC(sc); return 0; }

    std::vector<uint8_t> pixels;
    bool ok = bitblt_bgra(sc, sc, src_x, src_y, *w, *h, pixels);
    DeleteDC(sc);
    if (!ok) return 0;
    if (capture_is_solid_color(pixels.data(), (int)pixels.size())) return 0;

    int needed = (int)pixels.size();
    if (needed > buf_size) return 0;
    memcpy(buf, pixels.data(), needed);
    return needed;
}
