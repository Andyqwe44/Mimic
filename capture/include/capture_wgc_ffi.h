/**
 * capture_wgc_ffi.h — C-compatible FFI wrapper for WgcCapture.
 * Called from Rust via extern "C". No C++ types in the interface.
 */
#pragma once
#include <windows.h>
#include <cstdint>
#include "../../common/include/export.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Opaque handle to a WGC stream session.
typedef struct WgcStreamHandle WgcStreamHandle;

/// Initialize WinRT MTA apartment. Call once at process startup.
GAM_API void wgc_init_apartment(void);

/// Deinitialize WinRT apartment. Call once at process shutdown.
GAM_API void wgc_deinit_apartment(void);

/// Start a WGC capture stream for the given window.
/// max_dim: if > 0, frames larger than this are downscaled preserving aspect ratio.
/// Returns handle on success, nullptr on failure.
GAM_API WgcStreamHandle* wgc_stream_start(HWND hwnd, int max_dim);

/// Read the latest frame from the stream.
/// Copies pixel data into buf (must be at least buf_size bytes).
/// Returns number of bytes written (0 = no new frame yet).
/// w/h/ch are filled with the frame dimensions (BGRA, 4 channels).
GAM_API int wgc_stream_read(WgcStreamHandle* h, uint8_t* buf, int buf_size,
                    int* out_w, int* out_h, int* out_ch);

/// Check if the stream is still running and healthy.
GAM_API int wgc_stream_is_ok(WgcStreamHandle* h);

/// Stop the stream and release all resources.
GAM_API void wgc_stream_stop(WgcStreamHandle* h);

/// Signal the worker thread to stop without blocking (fire-and-forget).
GAM_API void wgc_stream_signal_stop(WgcStreamHandle* h);

/// Single-frame capture (no streaming). Returns 0 on failure.
/// Internally creates a temporary DispatcherQueue for frame delivery.
GAM_API int wgc_capture_single(HWND hwnd, uint8_t* buf, int buf_size,
                       int* out_w, int* out_h, int* out_ch);

/// Monitor capture (for desktop). Uses CreateForMonitor internally.
/// hmon: HMONITOR handle (from MonitorFromWindow, etc.)
GAM_API WgcStreamHandle* wgc_stream_start_monitor(HMONITOR hmon, int max_dim);

/// Single-frame monitor capture.
GAM_API int wgc_capture_single_monitor(HMONITOR hmon, uint8_t* buf, int buf_size,
                               int* out_w, int* out_h, int* out_ch);

/// GPU frame callback: d3d11_device / d3d11_texture are ID3D11Device* / ID3D11Texture2D*.
/// Invoked on the WGC worker thread; texture is valid only during the call.
typedef void (*WgcGpuFrameCb)(void* ctx, void* d3d11_device, void* d3d11_texture, int w, int h);

/// When cpu_readback==0, worker skips Map and only invokes the GPU callback.
GAM_API void wgc_stream_set_gpu_frame_callback(WgcStreamHandle* h, WgcGpuFrameCb cb, void* ctx);
GAM_API void wgc_stream_set_cpu_readback(WgcStreamHandle* h, int enable);

/// Pump one wait cycle (for gpu-only mode). Returns 1 if a frame was delivered.
GAM_API int wgc_stream_pump(WgcStreamHandle* h);

#ifdef __cplusplus
}
#endif
