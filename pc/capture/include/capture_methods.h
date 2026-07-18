/**
 * capture_methods.h — C-compatible FFI for all GDI capture methods.
 * Called from Rust via extern "C". All methods return BGRA pixels.
 */
#pragma once
#include <windows.h>
#include <cstdint>
#include "../../common/include/export.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Single-method captures. Each returns bytes written (0 = failure).
/// buf must be pre-allocated; buf_size is the max capacity.
/// w/h are filled with actual dimensions. Pixels are BGRA (4 channels).

GAM_API int capture_gdi_getwindowdc(HWND hwnd, uint8_t* buf, int buf_size, int* w, int* h);
GAM_API int capture_printwindow(HWND hwnd, uint8_t* buf, int buf_size, int* w, int* h);
GAM_API int capture_screen_bitblt(HWND hwnd, uint8_t* buf, int buf_size, int* w, int* h);
GAM_API int capture_desktop_bitblt(uint8_t* buf, int buf_size, int* w, int* h);

/// Auto-detect with 3-method fallback chain (GetWindowDC→PrintWindow→ScreenBitBlt).
/// method_out receives the name of the successful method.
/// Caller must free with capture_free_string().
GAM_API int capture_auto_detect(HWND hwnd, uint8_t* buf, int buf_size,
                        int* w, int* h, const char** method_out);
GAM_API void capture_free_string(const char* s);

/// Query window capture state.
/// Returns: "desktop"|"foreground"|"background"|"minimized"|"hidden"|"closed"
/// Caller must free with capture_free_string().
GAM_API const char* capture_query_window_state(HWND hwnd);

/// Validate content: returns 1 if pixels represent a solid color (all same RGB).
GAM_API int capture_is_solid_color(const uint8_t* pixels, int len);

/// Validate content: returns 1 if >5% of pixels are magenta (R=255,B=255,G=0).
GAM_API int capture_has_magenta(const uint8_t* pixels, int len);

#ifdef __cplusplus
}
#endif
