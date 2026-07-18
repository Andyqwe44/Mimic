/**
 * capture_desktop.cpp — FFI: DesktopBlt (full-screen GDI BitBlt).
 * Uses virtual screen DC to support multi-monitor setups correctly.
 */
#include "capture_methods.h"
#include "capture_internal.h"
#include <cstring>
#include <vector>

int capture_desktop_bitblt(uint8_t* buf, int buf_size, int* w, int* h) {
    *w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    *h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (*w <= 0 || *h <= 0) return 0;

    // Use virtual screen DC (covers all monitors)
    HDC dc = create_virtual_screen_dc();
    if (!dc) return 0;
    std::vector<uint8_t> pixels;
    bool ok = bitblt_bgra_full(dc, dc, *w, *h, pixels);
    DeleteDC(dc);
    if (!ok) return 0;

    int needed = (int)pixels.size();
    if (needed > buf_size) return 0;
    memcpy(buf, pixels.data(), needed);
    return needed;
}
