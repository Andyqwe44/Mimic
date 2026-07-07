/**
 * DXGI Desktop Duplication + GDI Capture Backends
 *
 * DxgiCapture: GPU-accelerated, configurable (skip virtual adapters, solid-output detection).
 * GdiCapture: CPU fallback.
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

#include "../../common/include/capture_helpers.hpp"
namespace ch = capture_helpers;

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
    DxgiCapture() = default;
    explicit DxgiCapture(const DxgiOptions& opts) : opts_(opts) {}

    const char* name() const override { return "DXGI Desktop Duplication"; }

    bool init() override {
        if (initialized_) return true;

        HRESULT hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION,
            &d3d_device_, nullptr, &d3d_context_);

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
            return false;
        if (hr == DXGI_ERROR_ACCESS_LOST) {
            cleanup_dup();
            if (!init_duplication()) return false;
            return capture(out, region);
        }
        if (FAILED(hr)) return false;

        ID3D11Texture2D* src_texture = nullptr;
        hr = desktop_resource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&src_texture);
        desktop_resource->Release();
        if (FAILED(hr)) { desk_dup_->ReleaseFrame(); return false; }

        D3D11_TEXTURE2D_DESC src_desc;
        src_texture->GetDesc(&src_desc);

        if (!staging_tex_ || staging_width_ != src_desc.Width || staging_height_ != src_desc.Height) {
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
            if (FAILED(hr)) { src_texture->Release(); desk_dup_->ReleaseFrame(); return false; }
            staging_width_ = src_desc.Width;
            staging_height_ = src_desc.Height;
        }

        d3d_context_->CopyResource(staging_tex_.Get(), src_texture);
        src_texture->Release();
        desk_dup_->ReleaseFrame();

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        hr = d3d_context_->Map(staging_tex_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) return false;

        int full_w = (int)staging_width_;
        int full_h = (int)staging_height_;
        int row_pitch = (int)mapped.RowPitch;

        int cap_x = region ? region->x : 0;
        int cap_y = region ? region->y : 0;
        int cap_w = region ? region->w : full_w;
        int cap_h = region ? region->h : full_h;

        if (!clamp_region(cap_x, cap_y, cap_w, cap_h, full_w, full_h)) return false;

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

        // Solid-output detection: if enabled and output is uniform, retry next
        if (opts_.skip_solid_outputs && ch::is_solid_color(out.data.data(), out.data.size())) {
            // Specifically check black (virtual display hallmark)
            bool is_black = (out.data[0] == 0 && out.data[1] == 0 && out.data[2] == 0);
            // Try next output
            if (try_next_output()) {
                return capture(out, region);  // retry with new output
            }
            if (is_black) return false;  // no more outputs, black frame is useless
        }

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
        IDXGIDevice* dxgi_device = nullptr;
        HRESULT hr = d3d_device_->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgi_device);
        if (FAILED(hr)) return false;

        IDXGIAdapter* adapter = nullptr;
        hr = dxgi_device->GetAdapter(&adapter);
        dxgi_device->Release();
        if (FAILED(hr)) return false;

        // Get DXGI factory for adapter enumeration
        IDXGIFactory1* factory = nullptr;
        if (opts_.skip_virtual_adapters) {
            HRESULT fhr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory);
            if (FAILED(fhr)) { factory = nullptr; }
        }

        bool found = false;
        IDXGIAdapter* current_adapter = adapter;
        UINT adapter_idx = 0;

        do {
            // Check adapter name for virtual/remote/indirect
            if (opts_.skip_virtual_adapters && current_adapter) {
                DXGI_ADAPTER_DESC adesc;
                if (SUCCEEDED(current_adapter->GetDesc(&adesc))) {
                    if (wcsstr(adesc.Description, L"Virtual") ||
                        wcsstr(adesc.Description, L"Remote") ||
                        wcsstr(adesc.Description, L"Indirect")) {
                        fprintf(stderr, "DXGI: skip adapter %u: %S\n", adapter_idx, adesc.Description);
                        goto next_adapter;
                    }
                }
            }

            // Try outputs on this adapter
            {
                IDXGIOutput* output = nullptr;
                for (UINT oi = 0; current_adapter && SUCCEEDED(current_adapter->EnumOutputs(oi, &output)); oi++) {
                    // Check output dimensions
                    if (opts_.min_output_width > 0 || opts_.min_output_height > 0) {
                        DXGI_OUTPUT_DESC odesc;
                        if (SUCCEEDED(output->GetDesc(&odesc))) {
                            int ow = odesc.DesktopCoordinates.right - odesc.DesktopCoordinates.left;
                            int oh = odesc.DesktopCoordinates.bottom - odesc.DesktopCoordinates.top;
                            if (!odesc.AttachedToDesktop || !odesc.Monitor ||
                                (opts_.min_output_width > 0 && ow < opts_.min_output_width) ||
                                (opts_.min_output_height > 0 && oh < opts_.min_output_height)) {
                                output->Release(); output = nullptr;
                                continue;
                            }
                        }
                    }

                    IDXGIOutput1* output1 = nullptr;
                    hr = output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1);
                    output->Release();
                    if (FAILED(hr)) continue;

                    hr = output1->DuplicateOutput(d3d_device_.Get(), desk_dup_.GetAddressOf());
                    output1->Release();
                    if (SUCCEEDED(hr)) {
                        found = true;
                        current_output_idx_ = oi;
                        break;
                    }
                }
            }

            if (found) break;

next_adapter:
            // Release previous adapter's resources
            if (current_adapter && current_adapter != adapter) {
                current_adapter->Release();
            }

            // Try next adapter (only when virtual-skip is enabled)
            if (opts_.skip_virtual_adapters && factory) {
                adapter_idx++;
                current_adapter = nullptr;
                if (factory->EnumAdapters(adapter_idx, &current_adapter) == DXGI_ERROR_NOT_FOUND) {
                    break;  // no more adapters
                }
            } else {
                break;  // only try first adapter
            }
        } while (!found);

        // Clean up the original adapter we got from the device
        adapter->Release();
        if (factory) factory->Release();

        if (!found) {
            fprintf(stderr, "DXGI: DuplicateOutput failed on all outputs\n");
            return false;
        }

        initialized_ = true;
        return true;
    }

    /// Try next output on same adapter. Returns true if switched successfully.
    bool try_next_output() {
        if (!d3d_device_) return false;

        IDXGIDevice* dxgi_device = nullptr;
        if (FAILED(d3d_device_->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgi_device))) return false;

        IDXGIAdapter* adapter = nullptr;
        HRESULT hr = dxgi_device->GetAdapter(&adapter);
        dxgi_device->Release();
        if (FAILED(hr)) return false;

        // Release current duplication
        desk_dup_.Reset();
        staging_tex_.Reset();

        // Try next output
        bool found = false;
        IDXGIOutput* output = nullptr;
        for (UINT oi = current_output_idx_ + 1; SUCCEEDED(adapter->EnumOutputs(oi, &output)); oi++) {
            IDXGIOutput1* output1 = nullptr;
            hr = output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1);
            output->Release();
            if (FAILED(hr)) continue;

            hr = output1->DuplicateOutput(d3d_device_.Get(), desk_dup_.GetAddressOf());
            output1->Release();
            if (SUCCEEDED(hr)) {
                found = true;
                current_output_idx_ = oi;
                fprintf(stderr, "DXGI: switched to output %u\n", oi);
                break;
            }
        }

        adapter->Release();
        return found;
    }

    bool reinit_dup() {
        cleanup_dup();
        return init_duplication();
    }

    void cleanup_dup() {
        staging_tex_.Reset();
        desk_dup_.Reset();
        initialized_ = false;
    }

    Microsoft::WRL::ComPtr<ID3D11Device>        d3d_device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3d_context_;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> desk_dup_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>     staging_tex_;
    UINT staging_width_ = 0, staging_height_ = 0;
    UINT current_output_idx_ = 0;
    bool initialized_ = false;
    DxgiOptions opts_;
};

// ==================== GDI Fallback ====================

class GdiCapture : public ICaptureBackend {
public:
    const char* name() const override { return "GDI BitBlt"; }

    bool init() override { return true; }

    bool capture(FrameBuffer& out, const Rect* region) override {
        HDC hdc_screen = CreateDCW(L"DISPLAY", nullptr, nullptr, nullptr);
        if (!hdc_screen) return false;

        int screen_w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        int screen_h = GetSystemMetrics(SM_CYVIRTUALSCREEN);

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
        bi.biHeight = -cap_h;
        bi.biPlanes = 1;
        bi.biBitCount = 32;
        bi.biCompression = BI_RGB;

        out.width = cap_w;
        out.height = cap_h;
        out.channels = 4;
        out.data.resize(cap_w * cap_h * 4);
        out.timestamp_us = capture_now_us();

        GetDIBits(hdc_mem, hbitmap, 0, cap_h, out.data.data(),
                  (BITMAPINFO*)&bi, DIB_RGB_COLORS);

        SelectObject(hdc_mem, old_bmp);
        DeleteObject(hbitmap);
        DeleteDC(hdc_mem);
        DeleteDC(hdc_screen);
        return true;
    }

    bool get_window_rect(const wchar_t* title, Rect& out) override {
        return find_window_rect(title, out);
    }

    void shutdown() override {}
};

// ==================== Factory ====================

std::unique_ptr<ICaptureBackend> create_capture_backend() {
    return create_capture_backend(DxgiOptions{});
}

std::unique_ptr<ICaptureBackend> create_capture_backend(const DxgiOptions& opts) {
    auto dxgi = std::make_unique<DxgiCapture>(opts);
    if (dxgi->init()) return dxgi;

    fprintf(stderr, "DXGI unavailable, falling back to GDI\n");
    auto gdi = std::make_unique<GdiCapture>();
    gdi->init();
    return gdi;
}
