/**
 * capture_wgc.cpp — WGC FramePool capture implementation.
 *
 * Follows OBS winrt-capture.cpp architecture:
 *   - FrameArrived event registered (not polling without event)
 *   - Condition variable for efficient frame waiting
 *   - D3D texture desc used instead of ContentSize (more reliable)
 *   - Safe staging buffer create-before-destroy
 *   - Device loss handled via item.Closed event
 *   - Borderless capture on Win11
 *   - Format change detection
 */
#include "../include/capture_wgc.hpp"
#include "../../logger/logger.h"
#include <cstdlib>
#include <cstring>
#include <thread>
#include <atomic>

namespace wgc {

// ═══════════════════════════════════════════════════════
// WgcCapture implementation
// ═══════════════════════════════════════════════════════

bool WgcCapture::init(HWND hwnd) {
    if (ok_) return true;

    if (!hwnd || !IsWindow(hwnd)) {
        last_error_ = "invalid HWND";
        return false;
    }

    if (!create_d3d_device(hwnd)) return false;
    if (!create_capture_item(hwnd)) return false;
    if (!create_frame_pool()) return false;

    ok_ = true;
    LOG("wgc", "init OK: %dx%d format=%d", item_w_, item_h_, (int)format_);
    return true;
}

bool WgcCapture::init_monitor(HMONITOR hmon) {
    if (ok_) return true;

    if (!hmon) {
        last_error_ = "invalid HMONITOR";
        return false;
    }

    if (!create_d3d_device_monitor(hmon)) return false;
    if (!create_capture_item_monitor(hmon)) return false;
    if (!create_frame_pool()) return false;

    ok_ = true;
    LOG("wgc", "init_monitor OK: %dx%d", item_w_, item_h_);
    return true;
}

bool WgcCapture::create_d3d_device_monitor(HMONITOR hmon) {
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)factory.GetAddressOf());
    if (FAILED(hr)) {
        last_error_ = "CreateDXGIFactory1 failed";
        return false;
    }

    ComPtr<IDXGIAdapter1> adapter;
    bool found = false;
    for (UINT i = 0; factory->EnumAdapters1(i, adapter.GetAddressOf()) != DXGI_ERROR_NOT_FOUND; i++) {
        ComPtr<IDXGIOutput> output;
        for (UINT j = 0; adapter->EnumOutputs(j, output.GetAddressOf()) != DXGI_ERROR_NOT_FOUND; j++) {
            DXGI_OUTPUT_DESC desc;
            if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == hmon) {
                found = true; break;
            }
            output.Reset();
        }
        if (found) break;
        adapter.Reset();
    }

    D3D_DRIVER_TYPE driver = adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;
    IDXGIAdapter* adapter_ptr = adapter ? adapter.Get() : nullptr;

    hr = D3D11CreateDevice(
        adapter_ptr, driver, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION,
        device_.GetAddressOf(), nullptr, ctx_.GetAddressOf());

    if (FAILED(hr)) {
        last_error_ = "D3D11CreateDevice failed";
        return false;
    }
    return true;
}

bool WgcCapture::create_d3d_device(HWND hwnd) {
    // Find adapter matching the window's monitor
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)factory.GetAddressOf());
    if (FAILED(hr)) {
        last_error_ = "CreateDXGIFactory1 failed";
        return false;
    }

    ComPtr<IDXGIAdapter1> adapter;
    bool found = false;
    for (UINT i = 0; factory->EnumAdapters1(i, adapter.GetAddressOf()) != DXGI_ERROR_NOT_FOUND; i++) {
        ComPtr<IDXGIOutput> output;
        for (UINT j = 0; adapter->EnumOutputs(j, output.GetAddressOf()) != DXGI_ERROR_NOT_FOUND; j++) {
            DXGI_OUTPUT_DESC desc;
            if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == mon) {
                found = true; break;
            }
            output.Reset();
        }
        if (found) break;
        adapter.Reset();
    }

    D3D_DRIVER_TYPE driver = adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;
    IDXGIAdapter* adapter_ptr = adapter ? adapter.Get() : nullptr;

    hr = D3D11CreateDevice(
        adapter_ptr, driver, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION,
        device_.GetAddressOf(), nullptr, ctx_.GetAddressOf());

    if (FAILED(hr)) {
        last_error_ = "D3D11CreateDevice failed";
        return false;
    }
    return true;
}

bool WgcCapture::create_capture_item_monitor(HMONITOR hmon) {
    ComPtr<IDXGIDevice> dxgi_dev;
    if (FAILED(device_.As(&dxgi_dev))) {
        last_error_ = "no IDXGIDevice";
        return false;
    }

    winrt::com_ptr<::IInspectable> d3d_inspectable;
    HRESULT hr = CreateDirect3D11DeviceFromDXGIDevice(dxgi_dev.Get(), d3d_inspectable.put());
    if (FAILED(hr)) {
        last_error_ = "CreateDirect3D11DeviceFromDXGIDevice failed";
        return false;
    }

    // Create GraphicsCaptureItem from HMONITOR via IGraphicsCaptureItemInterop
    auto factory = winrt::get_activation_factory<wgc_rt::GraphicsCaptureItem>();
    auto interop = factory.as<IGraphicsCaptureItemInterop>();
    winrt::com_ptr<::IUnknown> item_unk;
    hr = interop->CreateForMonitor(hmon, winrt::guid_of<wgc_rt::GraphicsCaptureItem>(),
        item_unk.put_void());
    if (FAILED(hr)) {
        last_error_ = "CreateForMonitor failed";
        LOG("wgc", "CreateForMonitor failed 0x%08lX", hr);
        return false;
    }
    item_ = item_unk.as<wgc_rt::GraphicsCaptureItem>();
    auto sz = item_.Size();
    item_w_ = sz.Width;
    item_h_ = sz.Height;

    closed_token_ = item_.Closed([this](wgc_rt::GraphicsCaptureItem const&,
                                         wf::IInspectable const&) {
        on_closed();
    });

    return true;
}

bool WgcCapture::create_capture_item(HWND hwnd) {
    // Get IDirect3DDevice from ID3D11Device
    ComPtr<IDXGIDevice> dxgi_dev;
    if (FAILED(device_.As(&dxgi_dev))) {
        last_error_ = "no IDXGIDevice";
        return false;
    }

    winrt::com_ptr<::IInspectable> d3d_inspectable;
    HRESULT hr = CreateDirect3D11DeviceFromDXGIDevice(dxgi_dev.Get(), d3d_inspectable.put());
    if (FAILED(hr)) {
        last_error_ = "CreateDirect3D11DeviceFromDXGIDevice failed";
        return false;
    }

    // Create GraphicsCaptureItem from HWND via IGraphicsCaptureItemInterop
    auto factory = winrt::get_activation_factory<wgc_rt::GraphicsCaptureItem>();
    auto interop = factory.as<IGraphicsCaptureItemInterop>();
    winrt::com_ptr<::IUnknown> item_unk;
    hr = interop->CreateForWindow(hwnd, winrt::guid_of<wgc_rt::GraphicsCaptureItem>(),
        item_unk.put_void());
    if (FAILED(hr)) {
        last_error_ = "CreateForWindow failed";
        LOG("wgc", "CreateForWindow failed 0x%08lX", hr);
        return false;
    }
    item_ = item_unk.as<wgc_rt::GraphicsCaptureItem>();
    auto sz = item_.Size();
    item_w_ = sz.Width;
    item_h_ = sz.Height;

    // Register Closed event for device loss detection
    closed_token_ = item_.Closed([this](wgc_rt::GraphicsCaptureItem const&,
                                         wf::IInspectable const&) {
        on_closed();
    });

    return true;
}

bool WgcCapture::create_frame_pool() {
    // Get WinRT Direct3D device
    ComPtr<IDXGIDevice> dxgi_dev;
    if (FAILED(device_.As(&dxgi_dev))) {
        last_error_ = "As<IDXGIDevice> failed";
        return false;
    }
    winrt::com_ptr<::IInspectable> insp;
    HRESULT hr = CreateDirect3D11DeviceFromDXGIDevice(dxgi_dev.Get(), insp.put());
    if (FAILED(hr)) {
        last_error_ = "CreateDirect3D11DeviceFromDXGIDevice failed";
        return false;
    }
    auto d3d_dev = insp.as<wgdd::IDirect3DDevice>();

    // Create FramePool with 3 buffered frames (was 2 — OBS uses 2 but
    // OBS processes frames entirely on GPU; we need CPU readback headroom)
    auto item_size = winrt::Windows::Graphics::SizeInt32{ item_w_, item_h_ };
    pool_ = wgc_rt::Direct3D11CaptureFramePool::Create(
        d3d_dev,
        winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
        3, item_size);

    if (!pool_) {
        last_error_ = "CreateFramePool failed";
        return false;
    }

    // Register FrameArrived event early (before CreateCaptureSession)
    frame_arrived_token_ = pool_.FrameArrived([this](
        wgc_rt::Direct3D11CaptureFramePool const&,
        wf::IInspectable const&) {
        on_frame_arrived();
    });

    session_ = pool_.CreateCaptureSession(item_);
    if (!session_) {
        last_error_ = "CreateCaptureSession failed";
        return false;
    }

    // Borderless capture (Win11+)
    if (borderless_supported()) {
        session_.IsBorderRequired(false);
    }

    // Disable WGC cursor capture (better perf, we can add our own later)
    if (cursor_toggle_supported()) {
        session_.IsCursorCaptureEnabled(false);
    }

    session_.StartCapture();

    last_w_ = item_w_;
    last_h_ = item_h_;
    return true;
}

bool WgcCapture::ensure_staging(int w, int h) {
    // Fast path: size matches, reuse
    if (staging_[0] && staging_w_[0] == w && staging_h_[0] == h)
        return true;

    // Safe pattern: create all new textures first, then swap in.
    // This avoids leaving partial state if creation fails midway.
    ComPtr<ID3D11Texture2D> new_staging[STAGING_COUNT];
    int new_w[STAGING_COUNT] = {};
    int new_h[STAGING_COUNT] = {};

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = (UINT)w;
    desc.Height = (UINT)h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    for (int i = 0; i < STAGING_COUNT; i++) {
        HRESULT hr = device_->CreateTexture2D(&desc, nullptr, new_staging[i].GetAddressOf());
        if (FAILED(hr)) {
            last_error_ = "CreateTexture2D(staging) failed";
            return false;
        }
        new_w[i] = w;
        new_h[i] = h;
    }

    // All succeeded — release old textures and swap
    for (int i = 0; i < STAGING_COUNT; i++) {
        staging_[i] = std::move(new_staging[i]);
        staging_w_[i] = new_w[i];
        staging_h_[i] = new_h[i];
    }
    staging_idx_ = 0;
    last_w_ = w;
    last_h_ = h;
    return true;
}

void WgcCapture::on_frame_arrived() {
    // Called from WinRT DispatcherQueue thread.
    // Just signal the condition variable — actual processing happens in capture()
    {
        std::lock_guard<std::mutex> lk(frame_mtx_);
        frame_ready_ = true;
    }
    frame_cv_.notify_one();
}

void WgcCapture::on_closed() {
    // Window destroyed or capture item lost
    LOG("wgc", "item closed");
    ok_ = false;
    {
        std::lock_guard<std::mutex> lk(frame_mtx_);
        frame_ready_ = false;
    }
    frame_cv_.notify_all();
}

bool WgcCapture::capture(WgcFrame& out, WgcTiming* timing) {
    if (!ok_) return false;

    uint64_t t0 = now_us();

    // Try to get next frame from FramePool (non-blocking)
    auto frame = pool_.TryGetNextFrame();
    if (!frame) {
        // DON'T reset frame_ready_ here — on_frame_arrived may have
        // set it to true between TryGetNextFrame returning null and now.
        // Resetting it would lose the notification (race condition).
        return false;
    }

    uint64_t t1 = now_us();

    // Get ID3D11Texture2D from WinRT surface
    // IDirect3DDxgiInterfaceAccess is an ABI interface, not a projected type
    auto surface = frame.Surface();
    auto access = surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    ComPtr<ID3D11Texture2D> src_tex;
    HRESULT hr = access->GetInterface(__uuidof(ID3D11Texture2D), (void**)src_tex.GetAddressOf());
    if (FAILED(hr) || !src_tex) {
        std::lock_guard<std::mutex> lk(frame_mtx_);
        frame_ready_ = false;
        return false;
    }

    // Use D3D texture desc for dimensions (more reliable than ContentSize — OBS pattern)
    D3D11_TEXTURE2D_DESC desc;
    src_tex->GetDesc(&desc);
    int fw = (int)desc.Width, fh = (int)desc.Height;

    // Format change detection
    if (desc.Format != format_) {
        LOG("wgc", "format changed %d->%d, marking inactive",
                (int)format_, (int)desc.Format);
        ok_ = false;
        frame_cv_.notify_all();
        return false;
    }

    // Ensure staging textures are sized correctly
    if (!ensure_staging(fw, fh)) {
        return false;
    }

    // Rotate staging buffer: use next slot for GPU copy
    int si = staging_idx_;
    staging_idx_ = (staging_idx_ + 1) % STAGING_COUNT;

    // GPU copy: source texture → staging texture
    ctx_->CopyResource(staging_[si].Get(), src_tex.Get());
    src_tex.Reset();  // release frame reference early so GPU can reuse surface
    uint64_t t2 = now_us();

    // CPU readback: Map staging texture (blocks until GPU copy complete)
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = ctx_->Map(staging_[si].Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        std::lock_guard<std::mutex> lk(frame_mtx_);
        frame_ready_ = false;
        return false;
    }

    int pitch = (int)mapped.RowPitch;
    int px_count = fw * fh * 4;
    out.pixels.resize(px_count);
    out.width = fw;
    out.height = fh;
    out.channels = 4;
    out.timestamp_us = t0;

    // Fast path: if RowPitch == width*4, single memcpy
    if (pitch == fw * 4) {
        memcpy(out.pixels.data(), mapped.pData, px_count);
    } else {
        // Row-by-row copy (padded GPU rows)
        uint8_t* dst = out.pixels.data();
        uint8_t* src = (uint8_t*)mapped.pData;
        for (int y = 0; y < fh; y++) {
            memcpy(dst + y * fw * 4, src + y * pitch, fw * 4);
        }
    }

    ctx_->Unmap(staging_[si].Get(), 0);
    uint64_t t3 = now_us();

    // Frame consumed; clear ready flag (new FrameArrived will set it again)
    {
        std::lock_guard<std::mutex> lk(frame_mtx_);
        frame_ready_ = false;
    }

    if (timing) {
        timing->cap_us = t1 - t0;
        timing->copy_us = t2 - t1;
        timing->readback_us = t3 - t2;
        timing->total_us = t3 - t0;
    }

    return true;
}

bool WgcCapture::wait_frame(WgcFrame& out, int timeout_ms, WgcTiming* timing) {
    if (!ok_) return false;

    // Check if frame is already ready (non-blocking)
    if (capture(out, timing)) return true;

    // Wait for FrameArrived signal
    std::unique_lock<std::mutex> lk(frame_mtx_);
    if (!frame_cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                            [this] { return frame_ready_ || !ok_; })) {
        return false; // timeout
    }
    if (!ok_) return false;
    lk.unlock();

    // Now capture the frame
    return capture(out, timing);
}

void WgcCapture::cleanup_winrt_objects() {
    // Unregister events first
    if (frame_arrived_token_.value) {
        pool_.FrameArrived(frame_arrived_token_);
        frame_arrived_token_.value = 0;
    }
    if (closed_token_.value) {
        item_.Closed(closed_token_);
        closed_token_.value = 0;
    }

    // Close WinRT objects (try/catch each, matching OBS pattern)
    if (session_) {
        try { session_.Close(); } catch (...) {}
        session_ = nullptr;
    }
    if (pool_) {
        try { pool_.Close(); } catch (...) {}
        pool_ = nullptr;
    }
    item_ = nullptr;
}

void WgcCapture::signal_stop() {
    ok_ = false;
    std::lock_guard<std::mutex> lk(frame_mtx_);
    frame_ready_ = true;
    frame_cv_.notify_all();
}

void WgcCapture::shutdown() {
    ok_ = false;
    frame_cv_.notify_all();

    cleanup_winrt_objects();

    for (int i = 0; i < STAGING_COUNT; i++) {
        staging_[i].Reset();
    }
    ctx_.Reset();
    device_.Reset();
}

} // namespace wgc
