/**
 * capture_auto.cpp — FFI: auto-detect with 3-method fallback chain.
 * GetWindowDC → PrintWindow → ScreenBitBlt.
 */
#include "capture_methods.h"
#include "capture_internal.h"
#include <cstring>

int capture_auto_detect(HWND hwnd, uint8_t* buf, int buf_size,
                        int* w, int* h, const char** method_out) {
    // Desktop: simple GDI BitBlt
    if (!hwnd || hwnd == GetDesktopWindow()) {
        int ret = capture_desktop_bitblt(buf, buf_size, w, h);
        if (ret > 0) *method_out = "DesktopBlt";
        return ret;
    }

    if (!is_window_valid(hwnd)) {
        *method_out = "ALL_FAILED";
        return 0;
    }

    // Try method 1: GetWindowDC
    int ret = capture_gdi_getwindowdc(hwnd, buf, buf_size, w, h);
    if (ret > 0) { *method_out = "GDI(GetWindowDC)"; return ret; }

    // Try method 2: PrintWindow
    ret = capture_printwindow(hwnd, buf, buf_size, w, h);
    if (ret > 0) { *method_out = "PrintWindow"; return ret; }

    // Try method 3: ScreenBitBlt
    ret = capture_screen_bitblt(hwnd, buf, buf_size, w, h);
    if (ret > 0) { *method_out = "ScreenBitBlt"; return ret; }

    *method_out = "ALL_FAILED";
    return 0;
}
