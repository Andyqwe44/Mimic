/**
 * capture_wgc.hpp — Windows.Graphics.Capture (WGC) FramePool backend
 *
 * GPU-accelerated window capture via WinRT WGC API.
 * Works for occluded/background windows (NOT minimized).
 * Triple-buffered staging to overlap GPU copy with CPU readback.
 *
 * Architecture follows OBS winrt-capture patterns:
 *   - MTA apartment for main process (call wgc_init_apartment once)
 *   - DispatcherQueue on capture thread (STA-based, required for FrameArrived)
 *   - FrameArrived event → condition_variable → efficient frame waiting
 *   - Device loss recovery via item.Closed event
 *   - Borderless capture on Win11
 *
 * Usage:
 *   wgc_init_apartment();                    // once at startup
 *   WgcCapture cap;
 *   cap.init(hwnd);
 *   while (running) {
 *     WgcFrame frame;
 *     if (cap.wait_frame(frame, timeout_ms)) {
 *       // frame.pixels, frame.width, frame.height, frame.timestamp_us
 *     }
 *   }
 *   cap.shutdown();
 *   wgc_deinit_apartment();                  // once at shutdown
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
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.System.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <DispatcherQueue.h>
#include <cstdint>
#include <vector>
#include <cstdio>
#include <mutex>
#include <condition_variable>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "windowsapp.lib")

namespace wgc {

using Microsoft::WRL::ComPtr;
namespace wgc_rt = winrt::Windows::Graphics::Capture;
namespace wf = winrt::Windows::Foundation;
namespace wgd = winrt::Windows::Graphics::DirectX;
namespace wgdd = winrt::Windows::Graphics::DirectX::Direct3D11;

// ── WinRT apartment lifecycle (call once at process start/end) ──
inline void init_apartment() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
}
inline void uninit_apartment() {
    winrt::uninit_apartment();
}

// ── DispatcherQueue lifecycle (one per capture thread) ──
inline winrt::Windows::System::DispatcherQueueController
create_dispatcher_queue() {
    DispatcherQueueOptions opts = {};
    opts.dwSize = sizeof(DispatcherQueueOptions);
    opts.threadType = DQTYPE_THREAD_CURRENT;
    opts.apartmentType = DQTAT_COM_STA;
    winrt::Windows::System::DispatcherQueueController ctrl{nullptr};
    winrt::check_hresult(CreateDispatcherQueueController(
        opts,
        reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(
            winrt::put_abi(ctrl))));
    return ctrl;
}

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
    uint64_t cap_us = 0;       // wait → frame acquired
    uint64_t copy_us = 0;      // CopyResource GPU→staging
    uint64_t readback_us = 0;  // Map + memcpy
    uint64_t total_us = 0;     // End-to-end
};

// ── Check if borderless capture is supported (Win11+) ──
inline bool borderless_supported() {
    // IsBorderRequired was added in Windows 11 (build 22000+)
    return winrt::Windows::Foundation::Metadata::ApiInformation::
        IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession",
                          L"IsBorderRequired");
}

// ── Check if cursor toggle is supported ───────────────
inline bool cursor_toggle_supported() {
    return winrt::Windows::Foundation::Metadata::ApiInformation::
        IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession",
                          L"IsCursorCaptureEnabled");
}

// ── WGC Capture Session ───────────────────────────────
class WgcCapture {
public:
    ~WgcCapture() { shutdown(); }

    /// Initialize for a window handle. Must be called from a thread with
    /// a DispatcherQueue (use create_dispatcher_queue() on this thread first).
    /// Returns false if WGC unavailable.
    bool init(HWND hwnd);

    /// Initialize for a monitor (desktop capture).
    bool init_monitor(HMONITOR hmon);

    /// Try to capture one frame (non-blocking). Returns false if no new frame.
    bool capture(WgcFrame& out, WgcTiming* timing = nullptr);

    /// Wait for a frame with timeout (milliseconds). Returns false on timeout.
    bool wait_frame(WgcFrame& out, int timeout_ms, WgcTiming* timing = nullptr);

    /// Check if session is still valid.
    bool is_ok() const { return ok_; }

    /// Signal the frame CV to wake up any waiting thread (for clean shutdown).
    void signal_stop();

    /// Release all resources.
    void shutdown();

    const char* last_error() const { return last_error_; }
    int width() const { return last_w_; }
    int height() const { return last_h_; }

private:
    bool create_d3d_device(HWND hwnd);
    bool create_d3d_device_monitor(HMONITOR hmon);
    bool create_capture_item(HWND hwnd);
    bool create_capture_item_monitor(HMONITOR hmon);
    bool create_frame_pool();
    bool ensure_staging(int w, int h);
    void on_frame_arrived();
    void on_closed();
    void cleanup_winrt_objects();

    ComPtr<ID3D11Device>        device_;
    ComPtr<ID3D11DeviceContext> ctx_;
    wgc_rt::GraphicsCaptureItem             item_{nullptr};
    wgc_rt::Direct3D11CaptureFramePool      pool_{nullptr};
    wgc_rt::GraphicsCaptureSession          session_{nullptr};

    // WinRT event revokers (keeps registrations alive)
    winrt::event_token frame_arrived_token_;
    winrt::event_token closed_token_;

    // Triple-buffered staging textures
    static constexpr int STAGING_COUNT = 3;
    ComPtr<ID3D11Texture2D> staging_[STAGING_COUNT];
    int staging_w_[STAGING_COUNT] = {};
    int staging_h_[STAGING_COUNT] = {};
    int staging_idx_ = 0;

    // Frame synchronization: FrameArrived callback signals this
    std::mutex frame_mtx_;
    std::condition_variable frame_cv_;
    bool frame_ready_ = false;

    bool ok_ = false;
    int last_w_ = 0, last_h_ = 0;
    const char* last_error_ = "";
    int item_w_ = 0, item_h_ = 0;
    DXGI_FORMAT format_ = DXGI_FORMAT_B8G8R8A8_UNORM;
};

} // namespace wgc
