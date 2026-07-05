/**
 * capture_wgc.cpp — WGC FramePool capture implementation.
 *
 * Design for 60+ FPS:
 *   - Triple-buffered staging textures: while CPU reads staging[N],
 *     GPU can copy next frame to staging[N+1].
 *   - Reuse all buffers (no per-frame alloc).
 *   - Do work between CopyResource and Map (e.g. TCP send of prev frame)
 *     so GPU has time to finish the copy before CPU maps.
 *   - Timing breakdown: cap → copy → readback → total.
 *
 * Output format (stdout stream mode):
 *   [timestamp_us:8][w:4][h:4][ch:4][reserved:4][pixels: w*h*ch]
 *
 * Usage:
 *   capture_wgc.exe <hwnd> [--single|--stream]
 */
#include "../include/capture_wgc.hpp"
#include <cstdlib>
#include <cstring>
#include <io.h>
#include <fcntl.h>
#include <thread>
#include <atomic>

namespace wgc {

// ═══════════════════════════════════════════════════════
// WgcCapture implementation
// ═══════════════════════════════════════════════════════

bool WgcCapture::init(HWND hwnd) {
    if (ok_) return true;

    // Must init WinRT apartment before calling WGC APIs
    // (caller should have done winrt::init_apartment)

    if (!hwnd || !IsWindow(hwnd)) {
        last_error_ = "invalid HWND";
        return false;
    }

    if (!create_d3d_device(hwnd)) return false;
    if (!create_capture_item(hwnd)) return false;
    if (!create_frame_pool()) return false;

    ok_ = true;
    fprintf(stderr, "[wgc] init OK: %dx%d\n", item_w_, item_h_);
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

    // Create GraphicsCaptureItem from HWND
    auto factory = winrt::get_activation_factory<wgc_rt::GraphicsCaptureItem>();
    auto interop = factory.as<IGraphicsCaptureItemInterop>();
    winrt::com_ptr<::IUnknown> item_unk;
    hr = interop->CreateForWindow(hwnd, winrt::guid_of<wgc_rt::GraphicsCaptureItem>(),
        item_unk.put_void());
    if (FAILED(hr)) {
        last_error_ = "CreateForWindow failed";
        fprintf(stderr, "[wgc] CreateForWindow failed 0x%08lX\n", hr);
        return false;
    }
    item_ = item_unk.as<wgc_rt::GraphicsCaptureItem>();
    auto sz = item_.Size();
    item_w_ = sz.Width;
    item_h_ = sz.Height;
    return true;
}

bool WgcCapture::create_frame_pool() {
    // Get WinRT Direct3D device
    ComPtr<IDXGIDevice> dxgi_dev;
    device_.As(&dxgi_dev);
    winrt::com_ptr<::IInspectable> insp;
    CreateDirect3D11DeviceFromDXGIDevice(dxgi_dev.Get(), insp.put());
    auto d3d_dev = insp.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();

    // Create FramePool with 2 buffered frames (min latency)
    auto item_size = winrt::Windows::Graphics::SizeInt32{ item_w_, item_h_ };
    pool_ = wgc_rt::Direct3D11CaptureFramePool::Create(
        d3d_dev,
        winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
        2, item_size);

    if (!pool_) {
        last_error_ = "CreateFramePool failed";
        return false;
    }

    session_ = pool_.CreateCaptureSession(item_);
    if (!session_) {
        last_error_ = "CreateCaptureSession failed";
        return false;
    }

    // Configure for max throughput
    session_.IsCursorCaptureEnabled(false);  // no cursor = slightly faster
    session_.StartCapture();

    last_w_ = item_w_;
    last_h_ = item_h_;
    return true;
}

bool WgcCapture::ensure_staging(int w, int h) {
    // Reuse staging textures if size matches
    if (staging_[0] && staging_w_[0] == w && staging_h_[0] == h)
        return true;

    for (int i = 0; i < STAGING_COUNT; i++) {
        staging_[i].Reset();
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = (UINT)w;
        desc.Height = (UINT)h;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        HRESULT hr = device_->CreateTexture2D(&desc, nullptr, staging_[i].GetAddressOf());
        if (FAILED(hr)) {
            last_error_ = "CreateTexture2D(staging) failed";
            return false;
        }
        staging_w_[i] = w;
        staging_h_[i] = h;
    }
    staging_idx_ = 0;
    last_w_ = w;
    last_h_ = h;
    return true;
}

bool WgcCapture::capture(WgcFrame& out, WgcTiming* timing) {
    if (!ok_) return false;

    uint64_t t0 = now_us();

    // Try to get next frame from FramePool (non-blocking)
    auto frame = pool_.TryGetNextFrame();
    if (!frame) return false;

    uint64_t t1 = now_us();

    // Get ID3D11Texture2D from WinRT surface
    auto surface = frame.Surface();
    auto access = surface.as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    ComPtr<ID3D11Texture2D> src_tex;
    HRESULT hr = access->GetInterface(__uuidof(ID3D11Texture2D), (void**)src_tex.GetAddressOf());
    if (FAILED(hr) || !src_tex) return false;

    D3D11_TEXTURE2D_DESC desc;
    src_tex->GetDesc(&desc);
    int fw = (int)desc.Width, fh = (int)desc.Height;

    // Ensure staging textures are sized correctly
    if (!ensure_staging(fw, fh)) return false;

    // Rotate staging buffer: use next slot for GPU copy
    int si = staging_idx_;
    staging_idx_ = (staging_idx_ + 1) % STAGING_COUNT;

    // GPU copy: source texture → staging texture
    ctx_->CopyResource(staging_[si].Get(), src_tex.Get());
    src_tex.Reset();  // release frame reference early
    uint64_t t2 = now_us();

    // CPU readback: Map staging texture
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = ctx_->Map(staging_[si].Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return false;

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

    if (timing) {
        timing->cap_us = t1 - t0;
        timing->copy_us = t2 - t1;
        timing->readback_us = t3 - t2;
        timing->total_us = t3 - t0;
    }

    return true;
}

void WgcCapture::shutdown() {
    if (ok_) {
        session_.Close();
        pool_.Close();
        ok_ = false;
    }
    for (int i = 0; i < STAGING_COUNT; i++) {
        staging_[i].Reset();
    }
    ctx_.Reset();
    device_.Reset();
}

} // namespace wgc
