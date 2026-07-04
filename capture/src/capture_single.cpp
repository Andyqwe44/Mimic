/**
 * capture_single.exe — single-frame screenshot, raw BGRA pixels to stdout
 *
 * Usage:   capture_single.exe <hwnd>
 *          hwnd=0  → full desktop (DXGI GPU-accelerated)
 *          hwnd≠0  → specific window (PrintWindow → DXGI crop → GDI fallback)
 *
 * Binary output format (LE): [w:4][h:4][ch:4][BGRA pixels...]
 * Debug info on stderr.
 */
#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#define NOMINMAX
#include <windows.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <io.h>
#include <fcntl.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

using Microsoft::WRL::ComPtr;

static void write_u32(uint32_t v) { fwrite(&v, 4, 1, stdout); }

// ── DXGI desktop capture (GPU) ──────────────────────────
static bool dxgi_capture(std::vector<uint8_t>& pixels, int& w, int& h) {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &ctx);
    if (FAILED(hr)) return false;

    ComPtr<IDXGIDevice> dxgi_dev;
    hr = device.As(&dxgi_dev);
    if (FAILED(hr)) return false;

    ComPtr<IDXGIAdapter> adapter;
    hr = dxgi_dev->GetAdapter(&adapter);
    if (FAILED(hr)) return false;

    ComPtr<IDXGIOutput> output;
    hr = adapter->EnumOutputs(0, &output);
    if (FAILED(hr)) return false;

    ComPtr<IDXGIOutput1> output1;
    hr = output.As(&output1);
    if (FAILED(hr)) return false;

    IDXGIOutputDuplication* dup = nullptr;
    hr = output1->DuplicateOutput(device.Get(), &dup);
    if (FAILED(hr)) return false;

    IDXGIResource* res = nullptr;
    DXGI_OUTDUPL_FRAME_INFO fi = {};
    hr = dup->AcquireNextFrame(100, &fi, &res);
    if (FAILED(hr)) { dup->Release(); return false; }

    ComPtr<ID3D11Texture2D> src_tex;
    hr = res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)src_tex.GetAddressOf());
    res->Release();
    if (FAILED(hr)) { dup->ReleaseFrame(); dup->Release(); return false; }

    D3D11_TEXTURE2D_DESC desc;
    src_tex->GetDesc(&desc);

    D3D11_TEXTURE2D_DESC staging = {};
    staging.Width = desc.Width; staging.Height = desc.Height;
    staging.MipLevels = 1; staging.ArraySize = 1;
    staging.Format = desc.Format; staging.SampleDesc.Count = 1;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ComPtr<ID3D11Texture2D> staging_tex;
    hr = device->CreateTexture2D(&staging, nullptr, &staging_tex);
    if (FAILED(hr)) { dup->ReleaseFrame(); dup->Release(); return false; }

    ctx->CopyResource(staging_tex.Get(), src_tex.Get());
    src_tex.Reset();
    dup->ReleaseFrame();
    dup->Release();

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = ctx->Map(staging_tex.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return false;

    int full_w = (int)desc.Width, full_h = (int)desc.Height;
    int pitch = (int)mapped.RowPitch;
    pixels.resize(full_w * full_h * 4);
    uint8_t* dst = pixels.data();
    uint8_t* src8 = (uint8_t*)mapped.pData;
    for (int y = 0; y < full_h; y++)
        memcpy(dst + y * full_w * 4, src8 + y * pitch, full_w * 4);

    ctx->Unmap(staging_tex.Get(), 0);
    w = full_w; h = full_h;
    return true;
}

// ── PrintWindow (PW_RENDERFULLCONTENT | PW_CLIENTONLY) ──
// Returns true if content looks valid (not solid / not blank)
static bool print_window(HWND hwnd, std::vector<uint8_t>& pixels, int w, int h) {
    HDC screen = GetDC(nullptr);
    if (!screen) { fprintf(stderr, "[capture] GetDC(NULL) failed\n"); return false; }

    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HBITMAP old = (HBITMAP)SelectObject(mem, bmp);

    // Fill with sentinel color (magenta) to detect PrintWindow not drawing at all
    RECT fill = {0, 0, w, h};
    HBRUSH magenta = CreateSolidBrush(RGB(255, 0, 255));
    FillRect(mem, &fill, magenta);
    DeleteObject(magenta);

    // PW_RENDERFULLCONTENT: DWM renders full content (DirectComposition windows)
    // PW_CLIENTONLY: capture client area only (no title bar/borders)
    BOOL ok = PrintWindow(hwnd, mem, PW_RENDERFULLCONTENT | PW_CLIENTONLY);
    fprintf(stderr, "[capture] PrintWindow(hwnd=%p, %dx%d, PW_RENDERFULLCONTENT|PW_CLIENTONLY) => %d\n",
        hwnd, w, h, (int)ok);

    BITMAPINFOHEADER bi = {};
    bi.biSize = sizeof(bi); bi.biWidth = w; bi.biHeight = -h;
    bi.biPlanes = 1; bi.biBitCount = 32; bi.biCompression = BI_RGB;

    pixels.resize(w * h * 4);
    GetDIBits(mem, bmp, 0, h, pixels.data(), (BITMAPINFO*)&bi, DIB_RGB_COLORS);

    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);

    // Check if content is valid (not solid magenta/black/white)
    if (pixels.empty()) return false;
    uint8_t r0 = pixels[2], g0 = pixels[1], b0 = pixels[0];
    int samples = 0, same = 0;
    int step = (int)pixels.size() / 400; if (step < 4) step = 4;
    for (size_t i = 0; i < pixels.size(); i += (size_t)step * 4) {
        samples++;
        if (pixels[i+2] == r0 && pixels[i+1] == g0 && pixels[i] == b0) same++;
    }
    bool solid = (samples > 0 && same == samples);
    fprintf(stderr, "[capture] PrintWindow content: %d/%d samples same=first(%02x,%02x,%02x), solid=%d\n",
        same, samples, r0, g0, b0, (int)solid);
    return !solid;
}

// ── DXGI crop to window rect ────────────────────────────
static bool dxgi_crop(HWND hwnd, const RECT& r, std::vector<uint8_t>& pixels, int& w, int& h) {
    std::vector<uint8_t> full;
    int fw = 0, fh = 0;
    if (!dxgi_capture(full, fw, fh)) {
        fprintf(stderr, "[capture] DXGI crop: dxgi_capture failed\n");
        return false;
    }
    int cx = r.left > 0 ? r.left : 0;
    int cy = r.top > 0 ? r.top : 0;
    int cw = (r.right - r.left) < (fw - cx) ? (r.right - r.left) : (fw - cx);
    int ch = (r.bottom - r.top) < (fh - cy) ? (r.bottom - r.top) : (fh - cy);
    if (cw <= 0 || ch <= 0) {
        fprintf(stderr, "[capture] DXGI crop: invalid rect %dx%d at %d,%d (screen %dx%d)\n", cw, ch, cx, cy, fw, fh);
        return false;
    }
    pixels.resize(cw * ch * 4);
    for (int y = 0; y < ch; y++) {
        int si = ((cy + y) * fw + cx) * 4;
        memcpy(pixels.data() + y * cw * 4, full.data() + si, cw * 4);
    }
    w = cw; h = ch;
    fprintf(stderr, "[capture] DXGI crop: %dx%d at %d,%d OK\n", cw, ch, cx, cy);
    return true;
}

// ── GDI fallback for desktop ────────────────────────────
static bool gdi_desktop(std::vector<uint8_t>& pixels, int& w, int& h) {
    HDC dc = GetDC(nullptr);
    if (!dc) return false;
    w = GetSystemMetrics(SM_CXSCREEN);
    h = GetSystemMetrics(SM_CYSCREEN);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
    HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, dc, 0, 0, SRCCOPY);

    BITMAPINFOHEADER bi = {};
    bi.biSize = sizeof(bi); bi.biWidth = w; bi.biHeight = -h;
    bi.biPlanes = 1; bi.biBitCount = 32; bi.biCompression = BI_RGB;
    pixels.resize(w * h * 4);
    GetDIBits(mem, bmp, 0, h, pixels.data(), (BITMAPINFO*)&bi, DIB_RGB_COLORS);
    SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem);
    ReleaseDC(nullptr, dc);
    fprintf(stderr, "[capture] GDI desktop: %dx%d OK\n", w, h);
    return true;
}

// ── Window capture (PrintWindow → DXGI crop → GDI) ─────
static bool win_capture(HWND hwnd, std::vector<uint8_t>& pixels, int& w, int& h) {
    fprintf(stderr, "[capture] win_capture hwnd=%p\n", hwnd);

    // Get window rect
    RECT r = {};
    DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r));
    if (r.right - r.left <= 0 || r.bottom - r.top <= 0) {
        if (!GetWindowRect(hwnd, &r)) {
            fprintf(stderr, "[capture] GetWindowRect failed\n");
            return false;
        }
    }
    w = r.right - r.left;
    h = r.bottom - r.top;
    fprintf(stderr, "[capture] window rect: %dx%d at (%ld,%ld)\n", w, h, r.left, r.top);
    if (w <= 0 || h <= 0) return false;

    // 1) Try PrintWindow
    bool pw_ok = print_window(hwnd, pixels, w, h);

    // 2) If PrintWindow failed, try DXGI crop
    if (!pw_ok) {
        fprintf(stderr, "[capture] PrintWindow solid → DXGI crop fallback\n");
        if (dxgi_crop(hwnd, r, pixels, w, h)) {
            fprintf(stderr, "[capture] win_capture: DXGI crop OK %dx%d\n", w, h);
            return true;
        }
    }

    // 3) If we got here with pw_ok, we have valid content
    if (pw_ok) {
        fprintf(stderr, "[capture] win_capture: PrintWindow OK %dx%d\n", w, h);
        return true;
    }

    // 4) Last resort: GDI GetWindowDC
    fprintf(stderr, "[capture] DXGI crop failed → GDI GetWindowDC fallback\n");
    HDC dc = GetWindowDC(hwnd);
    if (!dc) { fprintf(stderr, "[capture] GetWindowDC failed\n"); return false; }
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
    HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, dc, 0, 0, SRCCOPY);
    BITMAPINFOHEADER bi = {};
    bi.biSize = sizeof(bi); bi.biWidth = w; bi.biHeight = -h;
    bi.biPlanes = 1; bi.biBitCount = 32; bi.biCompression = BI_RGB;
    pixels.resize(w * h * 4);
    GetDIBits(mem, bmp, 0, h, pixels.data(), (BITMAPINFO*)&bi, DIB_RGB_COLORS);
    SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem);
    ReleaseDC(hwnd, dc);
    fprintf(stderr, "[capture] win_capture: GDI GetWindowDC OK %dx%d\n", w, h);
    return true;
}

// ── Check if image is solid color ────────────────────────
static bool solid_check(const uint8_t* data, int len) {
    if (len < 16) return len < 4;
    int step = len / 400; if (step < 4) step = 4;
    uint8_t r0 = data[2], g0 = data[1], b0 = data[0];
    int samples = 0, same = 0;
    for (int i = 0; i < len; i += step * 4) {
        samples++;
        if (data[i+2] == r0 && data[i+1] == g0 && data[i] == b0) same++;
    }
    bool solid = (samples > 0 && same == samples);
    if (solid) fprintf(stderr, "[capture] solid_check: SOLID color (%02x,%02x,%02x) %d/%d same\n", r0, g0, b0, same, samples);
    return solid;
}

// ── Desktop capture (DXGI → GDI fallback if solid) ──────
static bool desktop_capture(std::vector<uint8_t>& pixels, int& w, int& h) {
    fprintf(stderr, "[capture] desktop_capture: trying DXGI...\n");
    if (dxgi_capture(pixels, w, h)) {
        if (!solid_check(pixels.data(), (int)pixels.size())) {
            fprintf(stderr, "[capture] desktop_capture: DXGI OK %dx%d (not solid)\n", w, h);
            return true;
        }
        fprintf(stderr, "[capture] desktop_capture: DXGI returned solid → GDI fallback\n");
    } else {
        fprintf(stderr, "[capture] desktop_capture: DXGI failed, GDI fallback\n");
    }
    return gdi_desktop(pixels, w, h);
}

// ── main ────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    _setmode(_fileno(stdout), _O_BINARY);  // prevent \n→\r\n in pixel data

    HWND hwnd = (HWND)0;
    if (argc > 1) hwnd = (HWND)(ULONG_PTR)_strtoui64(argv[1], nullptr, 10);

    std::vector<uint8_t> pixels;
    int w = 0, h = 0;
    bool ok = false;

    if (hwnd == 0 || hwnd == GetDesktopWindow()) {
        ok = desktop_capture(pixels, w, h);
    } else {
        ok = win_capture(hwnd, pixels, w, h);
    }

    if (!ok || w <= 0 || h <= 0) {
        fprintf(stderr, "[capture] FAILED: ok=%d w=%d h=%d\n", (int)ok, w, h);
        return 1;
    }

    fprintf(stderr, "[capture] output: %dx%d %zu bytes\n", w, h, pixels.size());
    write_u32((uint32_t)w);
    write_u32((uint32_t)h);
    write_u32(4);
    fwrite(pixels.data(), 1, pixels.size(), stdout);
    fflush(stdout);
    return 0;
}
