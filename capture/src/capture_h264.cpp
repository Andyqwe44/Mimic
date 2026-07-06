/**
 * capture_h264.exe — GPU capture → MF H.264 hardware encode → stream
 *
 * Usage:   capture_h264.exe <hwnd>
 *          hwnd=0 → desktop (DXGI Desktop Duplication, skip virtual displays)
 *          hwnd≠0 → window (WGC FramePool → PrintWindow fallback)
 *
 * Output protocol (binary stdout, LE):
 *   Line 1: method name (text) + '\n'
 *   Each frame: [size:4 LE] then `size` bytes of H.264 NAL units
 *   size=0 → unchanged frame (re-use previous)
 *
 * TCP: listens on localhost:9998, same protocol as stdout.
 * Stdin: "q\n" → quit.  Stderr: debug/log info.
 */
#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#define NOMINMAX
#define _SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS
#include <windows.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <io.h>
#include <fcntl.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "ws2_32.lib")

#include "mf_encoder.hpp"
#include "../include/capture_wgc.hpp"
#include "../../common/include/capture_helpers.hpp"

using Microsoft::WRL::ComPtr;
namespace ch = capture_helpers;
static std::atomic<bool> g_running{true};

// ── H.264-specific scale (16px alignment required by encoder) ──
static const int MAX_DIM = 640;
static const int ALIGN = 16;
static int align_down(int v) { return (v / ALIGN) * ALIGN; }

static void scale_bgra_h264(const uint8_t* src, int sw, int sh,
                            std::vector<uint8_t>& dst, int& dw, int& dh) {
    float s = (float)MAX_DIM / (float)sw;
    if (s >= 1.0f) {
        dw = align_down(sw); dh = align_down(sh);
    } else {
        dw = align_down((int)(sw * s));
        dh = align_down((int)(sh * s));
    }
    if (dw < 64) dw = 64; if (dh < 64) dh = 64;
    dst.resize(dw * dh * 4);
    for (int y = 0; y < dh; y++) {
        int sy = (int)(y / s); if (sy >= sh) sy = sh - 1;
        for (int x = 0; x < dw; x++) {
            int sx = (int)(x / s); if (sx >= sw) sx = sw - 1;
            memcpy(dst.data() + (y * dw + x) * 4, src + (sy * sw + sx) * 4, 4);
        }
    }
}

// ── DXGI capture on shared device (needed because H.264 encoder shares the device) ──
static bool dxgi_capture_shared(ComPtr<ID3D11Device> dev, ComPtr<ID3D11DeviceContext> ctx,
                                std::vector<uint8_t>& pixels, int& w, int& h) {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)factory.GetAddressOf())))
        return false;

    ComPtr<IDXGIAdapter1> adapter1;
    for (UINT ai = 0; factory->EnumAdapters1(ai, adapter1.GetAddressOf()) != DXGI_ERROR_NOT_FOUND; ai++) {
        DXGI_ADAPTER_DESC1 adesc;
        if (FAILED(adapter1->GetDesc1(&adesc))) { adapter1.Reset(); continue; }

        // Skip virtual/remote/indirect display adapters
        if (wcsstr(adesc.Description, L"Virtual") ||
            wcsstr(adesc.Description, L"Remote") ||
            wcsstr(adesc.Description, L"Indirect")) {
            fprintf(stderr, "[dxgi] skip adapter %u: %S\n", ai, adesc.Description);
            adapter1.Reset(); continue;
        }

        ComPtr<IDXGIOutput> output;
        for (UINT oi = 0; adapter1->EnumOutputs(oi, output.GetAddressOf()) != DXGI_ERROR_NOT_FOUND; oi++) {
            DXGI_OUTPUT_DESC odesc;
            if (FAILED(output->GetDesc(&odesc))) { output.Reset(); continue; }

            if (!odesc.Monitor || !odesc.AttachedToDesktop) {
                fprintf(stderr, "[dxgi] skip output %u: no monitor\n", oi);
                output.Reset(); continue;
            }
            int ow = odesc.DesktopCoordinates.right - odesc.DesktopCoordinates.left;
            int oh = odesc.DesktopCoordinates.bottom - odesc.DesktopCoordinates.top;
            if (ow < 640 || oh < 480) {
                fprintf(stderr, "[dxgi] skip output %u: tiny %dx%d\n", oi, ow, oh);
                output.Reset(); continue;
            }

            ComPtr<IDXGIOutput1> output1;
            if (FAILED(output.As(&output1))) { output.Reset(); continue; }

            IDXGIOutputDuplication* dup = nullptr;
            if (FAILED(output1->DuplicateOutput(dev.Get(), &dup))) { output.Reset(); continue; }

            IDXGIResource* res = nullptr; DXGI_OUTDUPL_FRAME_INFO fi = {};
            HRESULT acq = dup->AcquireNextFrame(30, &fi, &res);
            if (FAILED(acq)) { dup->Release(); output.Reset(); continue; }

            ComPtr<ID3D11Texture2D> src;
            res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)src.GetAddressOf());
            res->Release();

            D3D11_TEXTURE2D_DESC desc; src->GetDesc(&desc);
            int fw = (int)desc.Width, fh = (int)desc.Height;

            D3D11_TEXTURE2D_DESC sd = {};
            sd.Width = desc.Width; sd.Height = desc.Height; sd.MipLevels = 1;
            sd.ArraySize = 1; sd.Format = desc.Format; sd.SampleDesc.Count = 1;
            sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> st;
            if (FAILED(dev->CreateTexture2D(&sd, nullptr, &st))) {
                dup->ReleaseFrame(); dup->Release(); output.Reset(); continue;
            }
            ctx->CopyResource(st.Get(), src.Get()); src.Reset();
            dup->ReleaseFrame(); dup->Release();

            D3D11_MAPPED_SUBRESOURCE m = {};
            if (FAILED(ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) { output.Reset(); continue; }

            int pitch = (int)m.RowPitch;
            pixels.resize(fw * fh * 4);
            uint8_t* dst = pixels.data();
            uint8_t* s = (uint8_t*)m.pData;
            for (int y = 0; y < fh; y++) memcpy(dst + y * fw * 4, s + y * pitch, fw * 4);
            ctx->Unmap(st.Get(), 0);

            // Solid black check using shared helper
            if (ch::is_solid_color(pixels.data(), pixels.size())) {
                // Specifically black = virtual display
                if (pixels[0] == 0 && pixels[1] == 0 && pixels[2] == 0) {
                    fprintf(stderr, "[dxgi] solid black → skip output %u\n", oi);
                    pixels.clear(); output.Reset(); continue;
                }
            }

            w = fw; h = fh;
            fprintf(stderr, "[dxgi] OK: %S output %u %dx%d\n", adesc.Description, oi, w, h);
            return true;
        }
        adapter1.Reset();
    }
    return false;
}

// ── GDI desktop fallback (shared) ─────────────────────────
static bool gdi_desk(std::vector<uint8_t>& p, int& w, int& h) {
    HDC dc = GetDC(nullptr); if (!dc) return false;
    w = GetSystemMetrics(SM_CXSCREEN); h = GetSystemMetrics(SM_CYSCREEN);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
    SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, dc, 0, 0, SRCCOPY);
    BITMAPINFOHEADER bi = {}; bi.biSize = sizeof(bi); bi.biWidth = w; bi.biHeight = -h;
    bi.biPlanes = 1; bi.biBitCount = 32; bi.biCompression = BI_RGB;
    p.resize(w * h * 4);
    GetDIBits(mem, bmp, 0, h, p.data(), (BITMAPINFO*)&bi, DIB_RGB_COLORS);
    DeleteObject(bmp); DeleteDC(mem); ReleaseDC(nullptr, dc);
    return true;
}

// ── PrintWindow fallback ──────────────────────────────────
static bool print_window_cap(HWND hwnd, std::vector<uint8_t>& cur, int& w, int& h, RECT& wr) {
    int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
    if (ww <= 0 || wh <= 0) return false;
    HDC screen = GetDC(nullptr);
    if (!screen) return false;
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, ww, wh);
    SelectObject(mem, bmp);
    RECT fill = {0, 0, ww, wh};
    HBRUSH mBrush = CreateSolidBrush(RGB(255, 0, 255));
    FillRect(mem, &fill, mBrush); DeleteObject(mBrush);
    PrintWindow(hwnd, mem, PW_RENDERFULLCONTENT | PW_CLIENTONLY);
    BITMAPINFOHEADER bi = {};
    bi.biSize = sizeof(bi); bi.biWidth = ww; bi.biHeight = -wh;
    bi.biPlanes = 1; bi.biBitCount = 32; bi.biCompression = BI_RGB;
    cur.resize(ww * wh * 4);
    GetDIBits(mem, bmp, 0, wh, cur.data(), (BITMAPINFO*)&bi, DIB_RGB_COLORS);
    SelectObject(mem, (HBITMAP)GetStockObject(NULL_BRUSH));
    DeleteObject(bmp); DeleteDC(mem); ReleaseDC(nullptr, screen);
    // Check magenta sentinel + solid color using shared helpers
    if (ch::is_solid_color(cur.data(), cur.size())) return false;
    if (ch::has_magenta_sentinel(cur.data(), cur.size())) return false;
    w = ww; h = wh;
    return true;
}

// ── WGC capture (shared library) ──────────────────────────
static wgc::WgcCapture g_wgc;

// ── TCP server ────────────────────────────────────────────
struct TcpServer {
    SOCKET listen_sock = INVALID_SOCKET;
    std::vector<SOCKET> clients;
    std::mutex mtx;
    std::thread accept_thread;
    std::atomic<bool> running{true};
};
static TcpServer g_tcp;

static void tcp_accept_loop() {
    while (g_tcp.running) {
        SOCKET client = accept(g_tcp.listen_sock, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            if (!g_tcp.running) break;
            Sleep(100); continue;
        }
        u_long mode = 0; ioctlsocket(client, FIONBIO, &mode);
        {
            std::lock_guard<std::mutex> lk(g_tcp.mtx);
            g_tcp.clients.push_back(client);
        }
        fprintf(stderr, "[tcp] client connected (%zu total)\n", g_tcp.clients.size());
    }
}

static bool tcp_start(uint16_t port) {
    WSADATA wsa; if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    g_tcp.listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (g_tcp.listen_sock == INVALID_SOCKET) return false;
    int opt = 1;
    setsockopt(g_tcp.listen_sock, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
    sockaddr_in addr = {};
    addr.sin_family = AF_INET; addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(g_tcp.listen_sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
    { fprintf(stderr, "[tcp] bind failed: %d\n", WSAGetLastError()); return false; }
    if (listen(g_tcp.listen_sock, 5) == SOCKET_ERROR)
    { fprintf(stderr, "[tcp] listen failed: %d\n", WSAGetLastError()); return false; }
    fprintf(stderr, "[tcp] listening on 127.0.0.1:%u\n", port);
    g_tcp.accept_thread = std::thread(tcp_accept_loop);
    return true;
}

static bool tcp_send_all(SOCKET s, const char* data, int len) {
    int sent = 0;
    while (sent < len) {
        int n = send(s, data + sent, len - sent, 0);
        if (n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) { Sleep(1); continue; }
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

static void tcp_broadcast_frame(const std::vector<uint8_t>& h264_data) {
    uint32_t sz = (uint32_t)h264_data.size();
    uint8_t hdr[4]; ch::w32_le(hdr, sz);
    std::lock_guard<std::mutex> lk(g_tcp.mtx);
    auto it = g_tcp.clients.begin();
    while (it != g_tcp.clients.end()) {
        bool ok = tcp_send_all(*it, (char*)hdr, 4);
        if (ok && sz > 0) ok = tcp_send_all(*it, (char*)h264_data.data(), (int)sz);
        if (!ok) {
            closesocket(*it); it = g_tcp.clients.erase(it);
            fprintf(stderr, "[tcp] client disconnected (%zu remain)\n", g_tcp.clients.size());
        } else { ++it; }
    }
}

static void tcp_stop() {
    g_tcp.running = false;
    if (g_tcp.listen_sock != INVALID_SOCKET) { closesocket(g_tcp.listen_sock); g_tcp.listen_sock = INVALID_SOCKET; }
    if (g_tcp.accept_thread.joinable()) g_tcp.accept_thread.join();
    for (auto c : g_tcp.clients) closesocket(c);
    g_tcp.clients.clear();
    WSACleanup();
}

static void stdin_thread() {
    char c; while (g_running && fread(&c, 1, 1, stdin) > 0 && c != 'q') {}
    g_running = false;
}

// ── main ────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    _setmode(_fileno(stdout), _O_BINARY); _setmode(_fileno(stdin), _O_BINARY);

    HWND hwnd = (HWND)0;
    if (argc > 1) hwnd = (HWND)(ULONG_PTR)_strtoui64(argv[1], nullptr, 10);
    bool desk = (hwnd == 0 || hwnd == GetDesktopWindow());
    fprintf(stderr, "[h264] hwnd=%p desktop=%d\n", hwnd, (int)desk);

    // Create D3D11 device (shared with H.264 encoder)
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &ctx);
    if (FAILED(hr)) { fprintf(stderr, "[h264] D3D11CreateDevice failed: 0x%08lX\n", hr); return 1; }

    // Init capture source
    const char* method = "GDI+H264";
    bool use_wgc = false;
    RECT wr = {};

    if (!desk) {
        use_wgc = g_wgc.init(hwnd);
        if (use_wgc) {
            method = "FramePool+H264";
        } else {
            fprintf(stderr, "[h264] WGC failed, PrintWindow fallback\n");
            method = "PrintWindow+H264";
            DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &wr, sizeof(wr));
            if (wr.right - wr.left <= 0) GetWindowRect(hwnd, &wr);
        }
    } else {
        std::vector<uint8_t> t; int tw = 0, th = 0;
        if (dxgi_capture_shared(device, ctx, t, tw, th)) {
            method = "DXGI+H264";
            fprintf(stderr, "[h264] desktop DXGI OK\n");
        } else {
            fprintf(stderr, "[h264] desktop DXGI failed → GDI fallback\n");
        }
    }

    // Handshake
    fprintf(stdout, "%s\n", method); fflush(stdout);
    fprintf(stderr, "[h264] method=%s\n", method);

    // Init MF H.264 encoder (deferred until first frame for size)
    MfH264Encoder encoder;
    bool encoder_ok = false;
    int enc_w = 0, enc_h = 0;

    // Start TCP + stdin
    tcp_start(9998);
    std::thread(stdin_thread).detach();

    // Main capture loop
    std::vector<uint8_t> prev_h264;
    int frames = 0, skipped = 0;
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    while (g_running) {
        int w = 0, h = 0;
        std::vector<uint8_t> cur;
        bool ok = false;

        // Capture
        if (use_wgc) {
            wgc::WgcFrame wf;
            ok = g_wgc.capture(wf);
            if (ok) { cur = std::move(wf.pixels); w = wf.width; h = wf.height; }
            else { Sleep(1); continue; }
        } else if (desk) {
            if (strcmp(method, "DXGI+H264") == 0)
                ok = dxgi_capture_shared(device, ctx, cur, w, h);
            if (!ok) ok = gdi_desk(cur, w, h);
        } else {
            ok = print_window_cap(hwnd, cur, w, h, wr);
            if (!ok) {
                // DXGI crop fallback
                std::vector<uint8_t> full; int fw = 0, fh = 0;
                if (dxgi_capture_shared(device, ctx, full, fw, fh)) {
                    int cx = wr.left > 0 ? wr.left : 0;
                    int cy = wr.top > 0 ? wr.top : 0;
                    int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
                    int cw = (ww < (fw - cx)) ? ww : (fw - cx);
                    int ch = (wh < (fh - cy)) ? wh : (fh - cy);
                    if (cw > 0 && ch > 0) {
                        cur.resize(cw * ch * 4);
                        for (int y = 0; y < ch; y++) {
                            int si = ((cy + y) * fw + cx) * 4;
                            memcpy(cur.data() + y * cw * 4, full.data() + si, cw * 4);
                        }
                        w = cw; h = ch; ok = true;
                    }
                }
            }
        }

        if (!ok || w <= 0 || h <= 0) { Sleep(1); continue; }

        // Scale (H.264-specific 16px alignment)
        std::vector<uint8_t> scaled;
        int sw = 0, sh = 0;
        scale_bgra_h264(cur.data(), w, h, scaled, sw, sh);

        // Init encoder on first frame
        if (!encoder_ok) {
            encoder_ok = encoder.init(device.Get(), ctx.Get(), sw, sh);
            if (!encoder_ok) { fprintf(stderr, "[h264] encoder init failed\n"); break; }
            enc_w = sw; enc_h = sh;
            fprintf(stderr, "[h264] encoder init OK %dx%d\n", enc_w, enc_h);
        }
        if (sw != enc_w || sh != enc_h) {
            fprintf(stderr, "[h264] size changed %dx%d → %dx%d, skip frame\n", enc_w, enc_h, sw, sh);
            continue;
        }

        // H.264 encode
        std::vector<uint8_t> h264_data;
        if (!encoder.encode_frame(scaled.data(), h264_data)) {
            fprintf(stderr, "[h264] encode failed\n"); continue;
        }

        if (h264_data.empty()) { skipped++; continue; }

        // Frame differ on H.264 data
        bool unchanged = prev_h264.size() == h264_data.size()
                      && ch::frames_equal(prev_h264.data(), h264_data.data(), h264_data.size());

        uint32_t sz = unchanged ? 0 : (uint32_t)h264_data.size();
        fwrite(&sz, 4, 1, stdout);
        if (!unchanged) {
            fwrite(h264_data.data(), 1, sz, stdout);
            prev_h264 = std::move(h264_data);
            frames++;
        } else { skipped++; }
        fflush(stdout);

        // TCP broadcast
        if (unchanged) {
            uint32_t zero = 0;
            uint8_t zhdr[4]; ch::w32_le(zhdr, zero);
            std::lock_guard<std::mutex> lk(g_tcp.mtx);
            for (auto& c : g_tcp.clients) { send(c, (char*)zhdr, 4, 0); }
        } else {
            tcp_broadcast_frame(prev_h264);
        }

        // FPS log
        if (frames > 0 && frames % 120 == 0) {
            QueryPerformanceCounter(&t1);
            double elapsed = (double)(t1.QuadPart - t0.QuadPart) / freq.QuadPart;
            fprintf(stderr, "[h264] %d frames in %.1fs = %.1f fps\n",
                frames, elapsed, frames / elapsed);
        }
    }

    encoder.shutdown();
    g_wgc.shutdown();
    tcp_stop();
    fprintf(stderr, "[h264] exit: %d frames, %d skipped\n", frames, skipped);
    return 0;
}
