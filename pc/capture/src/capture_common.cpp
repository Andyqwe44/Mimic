/**
 * capture_common.cpp — FFI: content validation + window state query.
 */
#include "capture_methods.h"
#include "capture_internal.h"
#include <cstdio>
#include <cstring>
#include <algorithm>

int capture_is_solid_color(const uint8_t* pixels, int len) {
    if (len < 4) return 1;             // empty/1px = trivially solid
    if (len < 16) return 0;            // too small to sample reliably
    int step = pixel_step(len);
    int r0 = pixels[2], g0 = pixels[1], b0 = pixels[0];
    // Bound i so i+2 < len (avoids OOB read for non-multiple-of-4 buffers)
    int end = len - 4;
    if (end < 0) end = 0;
    for (int i = 0; i <= end; i += step) {
        if (pixels[i+2] != r0 || pixels[i+1] != g0 || pixels[i] != b0) return 0;
    }
    return 1;
}

int capture_has_magenta(const uint8_t* pixels, int len) {
    if (len < 16) return 0;
    int step = pixel_step(len);
    int magenta = 0, total = 0;
    int end = len - 4;
    if (end < 0) end = 0;
    for (int i = 0; i <= end; i += step) {
        if (pixels[i+2] == 255 && pixels[i+1] == 0 && pixels[i] == 255) magenta++;
        total++;
    }
    return total > 0 && magenta * 20 > total ? 1 : 0;
}

void capture_free_string(const char* s) { (void)s; }

const char* capture_query_window_state(HWND hwnd) {
    if (!hwnd || hwnd == GetDesktopWindow()) return "desktop";
    if (!is_window_valid(hwnd)) return "closed";
    if (IsIconic(hwnd)) return "minimized";
    if (!IsWindowVisible(hwnd)) return "hidden";
    HWND fg = GetForegroundWindow();
    if (fg == hwnd) return "foreground";
    return "background";
}
