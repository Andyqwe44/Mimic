/**
 * capture_wgc.hpp — Windows.Graphics.Capture (WGC) FramePool backend
 *
 * GPU-accelerated window capture via WinRT WGC API.
 * Works for occluded/background windows (NOT minimized).
 * Triple-buffered staging to overlap GPU copy with CPU readback.
 *
 * Usage:
 *   WgcCapture cap;
 *   cap.init(hwnd);
 *   while (running) {
 *     WgcFrame frame;
 *     if (cap.capture(frame)) {
 *       // frame.pixels, frame.width, frame.height, frame.timestamp_us
 *     }
 *   }
 *   cap.shutdown();
 */
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#define NOMINMAX
#define _SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Foundation.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <cstdint>
#include <vector>
#include <cstdio>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "windowsapp.lib")

namespace wgc {

using Microsoft::WRL::ComPtr;
namespace wgc_rt = winrt::Windows::Graphics::Capture;
namespace wf = winrt::Windows::Foundation;

// ── High-precision timestamp ──────────────────────────
inline uint64_t now_us() {
    LARGE_INTEGER freq, cnt;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&cnt);
    return (uint64_t)(cnt.QuadPart * 1'000'000 / freq.QuadPart);
}

// ── Capture result ────────────────────────────────────
struct WgcFrame {
    std::vector<uint8_t> pixels;
    int width = 0;
    int height = 0;
    int channels = 4;  // always BGRA
    uint64_t timestamp_us = 0;
};

// ── Timing breakdown (per-frame) ──────────────────────
struct WgcTiming {
    uint64_t cap_us = 0;       // TryGetNextFrame → frame acquired
    uint64_t copy_us = 0;      // CopyResource GPU→staging
    uint64_t readback_us = 0;  // Map + memcpy row-by-row
    uint64_t total_us = 0;     // End-to-end capture
};

// ── WGC Capture Session ───────────────────────────────
class WgcCapture {
public:
    ~WgcCapture() { shutdown(); }

    /// Initialize for a window handle. Returns false if WGC unavailable.
    bool init(HWND hwnd);

    /// Capture one frame. Returns false if no new frame available.
    /// Pixels are BGRA (4 channels).
    bool capture(WgcFrame& out, WgcTiming* timing = nullptr);

    /// Check if session is still valid (window not closed, etc.)
    bool is_ok() const { return ok_; }

    /// Release all resources.
    void shutdown();

    const char* last_error() const { return last_error_; }

private:
    bool create_d3d_device(HWND hwnd);
    bool create_capture_item(HWND hwnd);
    bool create_frame_pool();
    bool ensure_staging(int w, int h);

    ComPtr<ID3D11Device>        device_;
    ComPtr<ID3D11DeviceContext> ctx_;
    wgc_rt::GraphicsCaptureItem             item_{nullptr};
    wgc_rt::Direct3D11CaptureFramePool      pool_{nullptr};
    wgc_rt::GraphicsCaptureSession          session_{nullptr};

    // Triple-buffered staging textures
    static constexpr int STAGING_COUNT = 3;
    ComPtr<ID3D11Texture2D> staging_[STAGING_COUNT];
    int staging_w_[STAGING_COUNT] = {};
    int staging_h_[STAGING_COUNT] = {};
    int staging_idx_ = 0;

    bool ok_ = false;
    int last_w_ = 0, last_h_ = 0;
    const char* last_error_ = "";
    int item_w_ = 0, item_h_ = 0;
};

} // namespace wgc
