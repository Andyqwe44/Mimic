/**
 * DXGI Desktop Duplication Capture Backend
 *
 * Uses D3D11 + IDXGIOutputDuplication for low-latency (1-2ms)
 * GPU-accelerated screen capture. Requires Windows 8+.
 *
 * Flow:
 *   1. D3D11CreateDevice -> ID3D11Device + ID3D11DeviceContext
 *   2. Enumerate adapters -> IDXGIOutput1 -> DuplicateOutput()
 *   3. Per-frame: AcquireNextFrame -> CopyResource(staging) -> Map -> read
 *   4. Handle DXGI_ERROR_ACCESS_LOST (mode changes) by reinitializing
 */
#include "capture.hpp"

#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <chrono>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

// ==================== Helpers ====================

uint64_t capture_now_us() {
    using namespace std::chrono;
    return (uint64_t)duration_cast<microseconds>(
        high_resolution_clock::now().time_since_epoch()).count();
}

bool clamp_region(int& x, int& y, int& w, int& h, int limit_w, int limit_h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > limit_w) w = limit_w - x;
    if (y + h > limit_h) h = limit_h - y;
    return w > 0 && h > 0;
}

bool find_window_rect(const wchar_t* title, Rect& out) {
    HWND hwnd = FindWindowW(nullptr, title);
    if (!hwnd) {
        // Partial title match
        hwnd = FindWindowW(nullptr, nullptr);
        while (hwnd) {
            wchar_t buf[256];
            if (GetWindowTextW(hwnd, buf, 256) > 0) {
                if (wcsstr(buf, title) != nullptr) break;
            }
            hwnd = GetWindow(hwnd, GW_HWNDNEXT);
        }
    }
    if (!hwnd) return false;
    RECT r;
    if (!GetWindowRect(hwnd, &r)) return false;
    out.x = r.left; out.y = r.top;
    out.w = r.right - r.left; out.h = r.bottom - r.top;
    return true;
}

// ==================== DXGI Backend ====================

class DxgiCapture : public ICaptureBackend {
public:
    const char* name() const override { return "DXGI Desktop Duplication"; }

    bool init() override {
        if (initialized_) return true;

        HRESULT hr = D3D11CreateDevice(
            nullptr,                    // default adapter
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            0,                          // no flags
            nullptr, 0,                 // default feature level
            D3D11_SDK_VERSION,
            &d3d_device_,
            nullptr,
            &d3d_context_);

        if (FAILED(hr)) {
            fprintf(stderr, "DXGI: D3D11CreateDevice failed (0x%08lX)\n", hr);
            return false;
        }
        return init_duplication();
    }

    bool capture(FrameBuffer& out, const Rect* region) override {
        if (!d3d_context_ || !desk_dup_) {
            if (!reinit_dup()) return false;
        }

        IDXGIResource* desktop_resource = nullptr;
        DXGI_OUTDUPL_FRAME_INFO frame_info = {};

        HRESULT hr = desk_dup_->AcquireNextFrame(16, &frame_info, &desktop_resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT)
            return false;  // no new frame
        if (hr == DXGI_ERROR_ACCESS_LOST) {
            // Display mode changed, need to recreate
            cleanup_dup();
            if (!init_duplication()) return false;
            return capture(out, region);
        }
        if (FAILED(hr)) return false;

        // Get the GPU texture
        ID3D11Texture2D* src_texture = nullptr;
        hr = desktop_resource->QueryInterface(__uuidof(ID3D11Texture2D),
                                               (void**)&src_texture);
        desktop_resource->Release();
        if (FAILED(hr)) {
            desk_dup_->ReleaseFrame();
            return false;
        }

        D3D11_TEXTURE2D_DESC src_desc;
        src_texture->GetDesc(&src_desc);

        // Create staging texture if needed
        if (!staging_tex_ ||
            staging_width_ != src_desc.Width ||
            staging_height_ != src_desc.Height) {
            staging_tex_.Reset();
            D3D11_TEXTURE2D_DESC staging_desc = {};
            staging_desc.Width = src_desc.Width;
            staging_desc.Height = src_desc.Height;
            staging_desc.MipLevels = 1;
            staging_desc.ArraySize = 1;
            staging_desc.Format = src_desc.Format;
            staging_desc.SampleDesc.Count = 1;
            staging_desc.Usage = D3D11_USAGE_STAGING;
            staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

            hr = d3d_device_->CreateTexture2D(&staging_desc, nullptr, &staging_tex_);
            if (FAILED(hr)) {
                src_texture->Release();
                desk_dup_->ReleaseFrame();
                return false;
            }
            staging_width_ = src_desc.Width;
            staging_height_ = src_desc.Height;
        }

        // Copy GPU -> staging
        d3d_context_->CopyResource(staging_tex_.Get(), src_texture);
        src_texture->Release();
        desk_dup_->ReleaseFrame();

        // Map and read
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        hr = d3d_context_->Map(staging_tex_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) return false;

        int full_w = (int)staging_width_;
        int full_h = (int)staging_height_;
        int row_pitch = (int)mapped.RowPitch;

        // Determine capture region
        int cap_x = region ? region->x : 0;
        int cap_y = region ? region->y : 0;
        int cap_w = region ? region->w : full_w;
        int cap_h = region ? region->h : full_h;

        // Clamp to screen bounds
        if (!clamp_region(cap_x, cap_y, cap_w, cap_h, full_w, full_h)) return false;

        // BGRA format: 4 bytes per pixel
        out.width = cap_w;
        out.height = cap_h;
        out.channels = 4;
        out.data.resize(cap_w * cap_h * 4);
        out.timestamp_us = capture_now_us();

        uint8_t* dst = out.data.data();
        uint8_t* src = (uint8_t*)mapped.pData + cap_y * row_pitch + cap_x * 4;
        for (int y = 0; y < cap_h; y++) {
            memcpy(dst + y * cap_w * 4, src + y * row_pitch, cap_w * 4);
        }

        d3d_context_->Unmap(staging_tex_.Get(), 0);
        return true;
    }

    bool get_window_rect(const wchar_t* title, Rect& out) override {
        return find_window_rect(title, out);
    }

    void shutdown() override {
        cleanup_dup();
        d3d_context_.Reset();
        d3d_device_.Reset();
        initialized_ = false;
    }

private:
    bool init_duplication() {
        // Get DXGI device
        IDXGIDevice* dxgi_device = nullptr;
        HRESULT hr = d3d_device_->QueryInterface(__uuidof(IDXGIDevice),
                                                   (void**)&dxgi_device);
        if (FAILED(hr)) return false;

        // Get adapter
        IDXGIAdapter* adapter = nullptr;
        hr = dxgi_device->GetAdapter(&adapter);
        dxgi_device->Release();
        if (FAILED(hr)) return false;

        // Get output (monitor)
        IDXGIOutput* output = nullptr;
        hr = adapter->EnumOutputs(0, &output);
        adapter->Release();
        if (FAILED(hr)) return false;

        // Get IDXGIOutput1 for duplication
        IDXGIOutput1* output1 = nullptr;
        hr = output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1);
        output->Release();
        if (FAILED(hr)) return false;

        hr = output1->DuplicateOutput(d3d_device_.Get(), &desk_dup_);
        output1->Release();
        if (FAILED(hr)) {
            fprintf(stderr, "DXGI: DuplicateOutput failed (0x%08lX)\n", hr);
            return false;
        }

        initialized_ = true;
        return true;
    }

    bool reinit_dup() {
        cleanup_dup();
        return init_duplication();
    }

    void cleanup_dup() {
        staging_tex_.Reset();
        if (desk_dup_) { desk_dup_->Release(); desk_dup_ = nullptr; }
        initialized_ = false;
    }

    Microsoft::WRL::ComPtr<ID3D11Device>        d3d_device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3d_context_;
    IDXGIOutputDuplication* desk_dup_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>     staging_tex_;
    UINT staging_width_ = 0;
    UINT staging_height_ = 0;
    bool initialized_ = false;
};

// ==================== GDI Fallback ====================

class GdiCapture : public ICaptureBackend {
public:
    const char* name() const override { return "GDI BitBlt"; }

    bool init() override { return true; }

    bool capture(FrameBuffer& out, const Rect* region) override {
        HDC hdc_screen = GetDC(nullptr);
        if (!hdc_screen) return false;

        int screen_w = GetSystemMetrics(SM_CXSCREEN);
        int screen_h = GetSystemMetrics(SM_CYSCREEN);

        int cap_x = region ? region->x : 0;
        int cap_y = region ? region->y : 0;
        int cap_w = region ? region->w : screen_w;
        int cap_h = region ? region->h : screen_h;

        if (!clamp_region(cap_x, cap_y, cap_w, cap_h, screen_w, screen_h))
        { ReleaseDC(nullptr, hdc_screen); return false; }

        HDC hdc_mem = CreateCompatibleDC(hdc_screen);
        HBITMAP hbitmap = CreateCompatibleBitmap(hdc_screen, cap_w, cap_h);
        HBITMAP old_bmp = (HBITMAP)SelectObject(hdc_mem, hbitmap);

        BitBlt(hdc_mem, 0, 0, cap_w, cap_h, hdc_screen, cap_x, cap_y, SRCCOPY);

        BITMAPINFOHEADER bi = {};
        bi.biSize = sizeof(BITMAPINFOHEADER);
        bi.biWidth = cap_w;
        bi.biHeight = -cap_h;  // top-down
        bi.biPlanes = 1;
        bi.biBitCount = 32;
        bi.biCompression = BI_RGB;

        out.width = cap_w;
        out.height = cap_h;
        out.channels = 4;  // BGRA
        out.data.resize(cap_w * cap_h * 4);
        out.timestamp_us = capture_now_us();

        GetDIBits(hdc_mem, hbitmap, 0, cap_h, out.data.data(),
                  (BITMAPINFO*)&bi, DIB_RGB_COLORS);

        SelectObject(hdc_mem, old_bmp);
        DeleteObject(hbitmap);
        DeleteDC(hdc_mem);
        ReleaseDC(nullptr, hdc_screen);
        return true;
    }

    bool get_window_rect(const wchar_t* title, Rect& out) override {
        return find_window_rect(title, out);
    }

    void shutdown() override {}
};

// ==================== Factory ====================

std::unique_ptr<ICaptureBackend> create_capture_backend() {
    auto dxgi = std::make_unique<DxgiCapture>();
    if (dxgi->init()) return dxgi;

    fprintf(stderr, "DXGI unavailable, falling back to GDI\n");
    auto gdi = std::make_unique<GdiCapture>();
    gdi->init();
    return gdi;
}
